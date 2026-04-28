/*
 * Control Ventiladores 24V (2-pin) – ESP32-C3
 * =====================================================
 * Alimentación placa: 24V (fuente impresora) → MP1584EN → 3.3V ESP8266-12
 * Ventiladores 24V: solo 2-pin (todo o nada)
 *
 * PRIMER INICIO / CAMBIO DE RED:
 *   Si no hay credenciales WiFi en EEPROM, arranca en modo AP "SnapFan-Setup"
 *   (red abierta). Conectar a esa red y abrir http://192.168.4.1 para
 *   configurar SSID y contraseña. Tras guardar, el ESP se reinicia y conecta.
 *   Para volver a configurar WiFi: botón "WiFi" en la web → /wifireset.
 *
 * MODOS DE OPERACIÓN (por zona, independientes):
 *   AUTO   – encendido/apagado según temperatura e histéresis.
 *   MANUAL – encendido/apagado manual desde la web.
 *
 * LÓGICA AUTO:
 *   temp >= THigh             → ON
 *   temp <  THigh − Hyst      → OFF (corte 24V via MOSFET)
 *   zona de histéresis        → mantiene estado anterior
 *
 * Pines ESP32-C3:
 *   GPIO5 → MOSFET zona 1 (HIGH=ON)   GPIO6 → MOSFET zona 2 (HIGH=ON)
 *   GPIO4 → OneWire DS18B20 × 2       GPIO7 → LED (activo LOW)
 *   LED parpadeo rápido = modo AP   parpadeo lento = modo AUTO normal
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPUpdateServer.h>
#include <DNSServer.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <EEPROM.h>
#include <PubSubClient.h>
#include <time.h>

#ifndef APP_VERSION
#define APP_VERSION "0.0.0"
#endif

#ifndef APP_GITHUB_REPO
#define APP_GITHUB_REPO "MrRegata/SnapFan"
#endif

const char* FW_VERSION = APP_VERSION;
const char* FW_GITHUB_REPO = APP_GITHUB_REPO;
const char* DEVICE_HOSTNAME = "snapfan";
const char* NTP_TZ = "CET-1CEST,M3.5.0/2,M10.5.0/3";
const char* NTP_SERVER_1 = "pool.ntp.org";
const char* NTP_SERVER_2 = "time.google.com";

// ─── Pines ────────────────────────────────────────────────────────────────────
// NOTA: Verifica que estos GPIO estén disponibles en ESP32-C3 y no reservados.
#define ONE_WIRE_PIN   4   // GPIO4 (ajustado para ESP32-C3, antes 14)
#define LED_PIN        7   // GPIO7 (ajustado, antes 2)
#define MOSFET1_PIN    5   // GPIO5 salida zona 1
#define MOSFET2_PIN    6   // GPIO6 salida zona 2

// ─── EEPROM layout ────────────────────────────────────────────────────────────
#define EEPROM_SIZE      200
// Configuración fans (bytes 0–23)
#define EEPROM_MAGIC     0xC77C6794UL
#define ADDR_MAGIC        0              // uint32_t
#define ADDR_WEB_AUTO     4              // uint8_t zona 1
#define ADDR_Z1_MANUAL    5              // uint8_t zona 1 manual ON/OFF
#define ADDR_Z2_AUTO      6              // uint8_t zona 2
#define ADDR_Z2_MANUAL    7              // uint8_t zona 2 manual ON/OFF
#define ADDR_Z1_THIGH     8              // float
#define ADDR_Z1_HYST     12              // float
#define ADDR_Z2_THIGH    16              // float
#define ADDR_Z2_HYST     20              // float
// Credenciales WiFi (bytes 60–161)
#define WIFI_MAGIC       0xA5F7E3C1UL
#define ADDR_WIFI_MAGIC  60              // uint32_t
#define ADDR_WIFI_SSID   64              // char[33]
#define ADDR_WIFI_PASS   97              // char[65]
// Temperaturas máximas históricas (bytes 162–173)
#define PEAK_MAGIC       0xB3D9F1A2UL
#define ADDR_PEAK_MAGIC  162             // uint32_t
#define ADDR_PEAK_T1     166             // float
#define ADDR_PEAK_T2     170             // float
#define LANG_MAGIC       0x4C414E47UL
#define ADDR_LANG_MAGIC  174             // uint32_t
#define ADDR_LANG_CODE   178             // char[3]
// Eliminado: solo 2-pin

// ─── MQTT ─────────────────────────────────────────────────────────────────────
const char* MQTT_BROKER    = "";
const int   MQTT_PORT      = 1883;
const char* MQTT_USER      = "";
const char* MQTT_PASS_M    = "";
const char* MQTT_CLIENT_ID = "snapfan_esp12";
#define TOPIC_STATUS  "snapfan/status"
#define TOPIC_MODE    "snapfan/mode"

// ─── Estado ───────────────────────────────────────────────────────────────────
bool apMode = false;
bool timeConfigured = false;

// ─── Parámetros de control ────────────────────────────────────────────────────
bool  z1Auto = true, z2Auto = true;
bool  z1ManualOn = false, z2ManualOn = false;
float z1THigh = 45.0f, z1Hyst = 10.0f;
float z2THigh = 50.0f, z2Hyst = 10.0f;

// ─── Objetos globales ─────────────────────────────────────────────────────────
OneWire              oneWire(ONE_WIRE_PIN);
DallasTemperature    sensors(&oneWire);
WebServer            server(80);
HTTPUpdateServer     httpUpdater;
DNSServer            dnsServer;
WiFiClient           wifiClient;
PubSubClient         mqttClient(wifiClient);

DeviceAddress addr1, addr2;
bool  twoSensors = false;
float temp1 = 0.0f, temp2 = 0.0f;
float peakTemp1 = -127.0f, peakTemp2 = -127.0f;
bool  fan1Active = false, fan2Active = false;
bool  sensor1Valid = false, sensor2Valid = false;
bool  sensor1Ready = false, sensor2Ready = false;
char  currentLang[3] = "es";

// ─── Historial de temperatura (últimos 60 lecturas ≈ 3 min) ──────────────────
#define HIST_SIZE 60
float hist1[HIST_SIZE];
float hist2[HIST_SIZE];
int   histIdx   = 0;
int   histCount = 0;

unsigned long lastTempMs = 0, lastMqttMs = 0;
const unsigned long TEMP_MS = 3000UL, MQTT_MS = 5000UL;

// ─── Rampa lineal + histéresis ────────────────────────────────────────────────
// Solo ON/OFF según temperatura e histéresis
bool calcAutoFan(float t, float tHigh, float hyst, bool &fanOn) {
  if (t >= tHigh) {
    fanOn = true;
  } else if (t < tHigh - hyst) {
    fanOn = false;
  }
  return fanOn;
}

bool isBootPlaceholderReading(float temperature) {
  return temperature > 84.5f && temperature < 85.5f;
}

bool isSupportedLanguageCode(const char* code) {
  if (!code || code[0] == '\0' || code[1] == '\0' || code[2] != '\0') return false;
  return strcmp(code, "es") == 0 || strcmp(code, "en") == 0 || strcmp(code, "fr") == 0 ||
         strcmp(code, "it") == 0 || strcmp(code, "pt") == 0 || strcmp(code, "de") == 0 ||
         strcmp(code, "zh") == 0;
}

void setCurrentLanguage(const char* code) {
  if (isSupportedLanguageCode(code)) {
    currentLang[0] = code[0];
    currentLang[1] = code[1];
  } else {
    currentLang[0] = 'e';
    currentLang[1] = 's';
  }
  currentLang[2] = '\0';
}

void loadLanguagePref() {
  uint32_t magic;
  EEPROM.get(ADDR_LANG_MAGIC, magic);
  if (magic != LANG_MAGIC) {
    setCurrentLanguage("es");
    return;
  }
  char code[3] = {0};
  for (int i = 0; i < 3; ++i) code[i] = static_cast<char>(EEPROM.read(ADDR_LANG_CODE + i));
  setCurrentLanguage(code);
}

void saveLanguagePref(const char* code) {
  setCurrentLanguage(code);
  const uint32_t magic = LANG_MAGIC;
  EEPROM.put(ADDR_LANG_MAGIC, magic);
  for (int i = 0; i < 3; ++i) EEPROM.write(ADDR_LANG_CODE + i, currentLang[i]);
  EEPROM.commit();
}

// ─── EEPROM – WiFi ────────────────────────────────────────────────────────────
bool loadWifiCreds(char* ssid, char* pass) {
    uint32_t m;
    EEPROM.get(ADDR_WIFI_MAGIC, m);
    if (m != WIFI_MAGIC) return false;
    for (int i = 0; i < 33; i++) ssid[i] = EEPROM.read(ADDR_WIFI_SSID + i);
    ssid[32] = '\0';
    for (int i = 0; i < 65; i++) pass[i] = EEPROM.read(ADDR_WIFI_PASS + i);
    pass[64] = '\0';
    return (ssid[0] != '\0');
}

void saveWifiCreds(const char* ssid, const char* pass) {
    const uint32_t m = WIFI_MAGIC;
    EEPROM.put(ADDR_WIFI_MAGIC, m);
    const int sl = strlen(ssid), pl = strlen(pass);
    for (int i = 0; i < 33; i++) EEPROM.write(ADDR_WIFI_SSID + i, i < sl ? ssid[i] : 0);
    for (int i = 0; i < 65; i++) EEPROM.write(ADDR_WIFI_PASS + i, i < pl ? pass[i] : 0);
    EEPROM.commit();
}

// ─── EEPROM – temperaturas máximas ───────────────────────────────────────────
void loadPeakTemps() {
    uint32_t m;
    EEPROM.get(ADDR_PEAK_MAGIC, m);
    if (m != PEAK_MAGIC) return;
    float v1, v2;
    EEPROM.get(ADDR_PEAK_T1, v1);
    EEPROM.get(ADDR_PEAK_T2, v2);
    if (v1 > -50.0f && v1 < 150.0f) peakTemp1 = v1;
    if (v2 > -50.0f && v2 < 150.0f) peakTemp2 = v2;
}

void savePeakTemps() {
    const uint32_t m = PEAK_MAGIC;
    EEPROM.put(ADDR_PEAK_MAGIC, m);
    EEPROM.put(ADDR_PEAK_T1, peakTemp1);
    EEPROM.put(ADDR_PEAK_T2, peakTemp2);
    EEPROM.commit();
}

// ─── EEPROM – fans ────────────────────────────────────────────────────────────
void loadSettings() {
    uint32_t magic;
    EEPROM.get(ADDR_MAGIC, magic);
    if (magic != EEPROM_MAGIC) return;
    uint8_t wa;
    EEPROM.get(ADDR_WEB_AUTO, wa); z1Auto = (wa != 0);
  EEPROM.get(ADDR_Z1_MANUAL, wa); z1ManualOn = (wa != 0);
  EEPROM.get(ADDR_Z2_AUTO,  wa); z2Auto = (wa != 0);
  EEPROM.get(ADDR_Z2_MANUAL, wa); z2ManualOn = (wa != 0);
    float v;
    auto lf = [&](int a, float &d, float mn, float mx) { EEPROM.get(a, v); if (v >= mn && v <= mx) d = v; };
  lf(ADDR_Z1_THIGH, z1THigh, 20, 90);
  lf(ADDR_Z1_HYST, z1Hyst, 1, 20);
  lf(ADDR_Z2_THIGH, z2THigh, 20, 90);
  lf(ADDR_Z2_HYST, z2Hyst, 1, 20);
}

void saveSettings() {
    const uint32_t m = EEPROM_MAGIC;
    EEPROM.put(ADDR_MAGIC,    m);
    EEPROM.put(ADDR_WEB_AUTO, (uint8_t)(z1Auto ? 1 : 0));
  EEPROM.put(ADDR_Z1_MANUAL, (uint8_t)(z1ManualOn ? 1 : 0));
  EEPROM.put(ADDR_Z2_AUTO,  (uint8_t)(z2Auto ? 1 : 0));
  EEPROM.put(ADDR_Z2_MANUAL, (uint8_t)(z2ManualOn ? 1 : 0));
  EEPROM.put(ADDR_Z1_THIGH, z1THigh);
  EEPROM.put(ADDR_Z1_HYST, z1Hyst);
  EEPROM.put(ADDR_Z2_THIGH, z2THigh);
  EEPROM.put(ADDR_Z2_HYST, z2Hyst);
    EEPROM.commit();
}

  String jsonEscape(const String& value) {
    String out;
    out.reserve(value.length() + 8);
    for (size_t i = 0; i < value.length(); ++i) {
      const char c = value[i];
      switch (c) {
        case '\\': out += F("\\\\"); break;
        case '"': out += F("\\\""); break;
        case '\n': out += F("\\n"); break;
        case '\r': out += F("\\r"); break;
        case '\t': out += F("\\t"); break;
        default:
          if (static_cast<uint8_t>(c) >= 0x20) out += c;
          break;
      }
    }
    return out;
  }

// ─── JSON de estado ───────────────────────────────────────────────────────────
String buildJson() {
  String j; j.reserve(672);
    const bool wifiConnected = WiFi.status() == WL_CONNECTED;
    const int sensorCount = sensors.getDeviceCount();
    const String wifiSsid = wifiConnected ? WiFi.SSID() : String();
    const String wifiIp = wifiConnected ? WiFi.localIP().toString() : String();
    const String wifiHost = wifiConnected ? String(WiFi.getHostname()) : String(DEVICE_HOSTNAME);
    const long wifiRssi = wifiConnected ? WiFi.RSSI() : -127;
    time_t nowTs = 0;
    if (wifiConnected) {
      if (!timeConfigured) {
        configTzTime(NTP_TZ, NTP_SERVER_1, NTP_SERVER_2);
        timeConfigured = true;
      }
      nowTs = time(nullptr);
      if (nowTs < 946684800) nowTs = 0;
    }
    j += F("{\"t1\":"); j += String(temp1, 1);
    j += F(",\"t2\":"); j += String(temp2, 1);
  j += F(",\"wifi\":"); j += wifiConnected ? F("true") : F("false");
  j += F(",\"ssid\":\""); j += jsonEscape(wifiSsid); j += F("\"");
  j += F(",\"ip\":\""); j += jsonEscape(wifiIp); j += F("\"");
  j += F(",\"host\":\""); j += jsonEscape(wifiHost); j += F("\"");
  j += F(",\"rssi\":"); j += String(wifiRssi);
  j += F(",\"ntp\":"); j += nowTs > 0 ? F("true") : F("false");
  j += F(",\"epoch\":"); j += String(static_cast<unsigned long>(nowTs));
  j += F(",\"on1\":"); j += fan1Active ? F("true") : F("false");
  j += F(",\"on2\":"); j += fan2Active ? F("true") : F("false");
    j += F(",\"z1auto\":"); j += z1Auto ? F("true") : F("false");
    j += F(",\"z2auto\":"); j += z2Auto ? F("true") : F("false");
  j += F(",\"z1man\":"); j += z1ManualOn ? F("true") : F("false");
  j += F(",\"z2man\":"); j += z2ManualOn ? F("true") : F("false");
  j += F(",\"z1th\":"); j += String(z1THigh,1);
    j += F(",\"z1hy\":"); j += String(z1Hyst,1);
  j += F(",\"z2th\":"); j += String(z2THigh,1);
    j += F(",\"z2hy\":"); j += String(z2Hyst,1);
    j += F(",\"mx1\":"); j += String(peakTemp1, 1);
    j += F(",\"mx2\":"); j += String(peakTemp2, 1);
      j += F(",\"sc\":"); j += sensorCount;
      j += F(",\"s1ok\":"); j += sensor1Valid ? F("true") : F("false");
      j += F(",\"s2ok\":"); j += sensor2Valid ? F("true") : F("false");
      j += F(",\"ver\":\""); j += FW_VERSION;
      j += F("\",\"repo\":\""); j += FW_GITHUB_REPO;
      j += F("\",\"lang\":\""); j += currentLang;
      j += F("\"");
    j += F("}");
    return j;
}

// ─── MQTT ─────────────────────────────────────────────────────────────────────
void mqttCallback(char* topic, byte* payload, unsigned int len) {
    if (len == 0 || len > 8) return;
    char buf[9]; memcpy(buf, payload, len); buf[len] = '\0';
    if (strcmp(topic, TOPIC_MODE) == 0) { z1Auto = z2Auto = (strcmp(buf, "auto") == 0); saveSettings(); return; }
}

void mqttReconnect() {
    if (WiFi.status() != WL_CONNECTED || millis() - lastMqttMs < MQTT_MS) return;
    lastMqttMs = millis();
    bool ok = (MQTT_USER[0] != '\0')
        ? mqttClient.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS_M)
        : mqttClient.connect(MQTT_CLIENT_ID);
  if (ok) { mqttClient.subscribe(TOPIC_MODE); }
}

// ─── Control fans ─────────────────────────────────────────────────────────────
void controlFans() {
    if (z1Auto) {
      calcAutoFan(temp1, z1THigh, z1Hyst, fan1Active);
    } else {
      fan1Active = z1ManualOn;
    }
    if (z2Auto) {
      calcAutoFan(temp2, z2THigh, z2Hyst, fan2Active);
    } else {
      fan2Active = z2ManualOn;
    }

    digitalWrite(MOSFET1_PIN, fan1Active ? HIGH : LOW);
    digitalWrite(MOSFET2_PIN, fan2Active ? HIGH : LOW);

    static unsigned long ledMs = 0; static bool ledSt = false;
    if (z1Auto || z2Auto) {
      if (millis() - ledMs >= 1000UL) { ledMs = millis(); ledSt = !ledSt; digitalWrite(LED_PIN, ledSt ? LOW : HIGH); }
    } else { digitalWrite(LED_PIN, LOW); }
}

// ─── AP Portal HTML ───────────────────────────────────────────────────────────
const char AP_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="es"><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1.0">
<title>SnapFan - Setup WiFi</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'Segoe UI',sans-serif;background:#111827;color:#e2e8f0;display:flex;align-items:center;justify-content:center;min-height:100vh;padding:16px}
.box{background:#1e2535;padding:28px;border-radius:14px;width:100%;max-width:360px;border:1px solid #2d3a52;box-shadow:0 8px 32px #0008}
.hdr{display:flex;align-items:center;justify-content:center;gap:10px;margin-bottom:4px}
.hdr svg{width:36px;height:36px}
h1{color:#60a5fa;font-size:1.15rem}
.sub{font-size:.72rem;color:#64748b;text-align:center;margin-bottom:22px}
label{font-size:.78rem;color:#94a3b8;display:block;margin-bottom:4px;margin-top:12px}
input{width:100%;padding:9px 10px;border-radius:8px;border:1px solid #2d3a52;background:#0f172a;color:#e2e8f0;font-size:.9rem}
button{width:100%;margin-top:18px;padding:11px;background:#3b82f6;color:#fff;border:none;border-radius:8px;font-size:.95rem;cursor:pointer;font-weight:600}
button:hover{background:#2563eb}
.note{font-size:.68rem;color:#475569;text-align:center;margin-top:14px;line-height:1.4}
</style></head><body>
<div class="box">
  <div class="hdr">
    <svg viewBox="0 0 64 64" fill="none" xmlns="http://www.w3.org/2000/svg">
      <rect x="8" y="44" width="48" height="12" rx="3" fill="#1e3a5f" stroke="#60a5fa" stroke-width="2"/>
      <rect x="14" y="36" width="36" height="10" rx="2" fill="#0f2a47" stroke="#3b82f6" stroke-width="1.5"/>
      <rect x="20" y="28" width="24" height="10" rx="2" fill="#0f2a47" stroke="#3b82f6" stroke-width="1.5"/>
      <line x1="22" y1="28" x2="22" y2="10" stroke="#60a5fa" stroke-width="2" stroke-linecap="round"/>
      <line x1="32" y1="28" x2="32" y2="10" stroke="#60a5fa" stroke-width="2" stroke-linecap="round"/>
      <line x1="42" y1="28" x2="42" y2="10" stroke="#60a5fa" stroke-width="2" stroke-linecap="round"/>
      <circle cx="22" cy="10" r="3" fill="#38bdf8"/>
      <circle cx="32" cy="10" r="3" fill="#fb923c"/>
      <circle cx="42" cy="10" r="3" fill="#4ade80"/>
    </svg>
    <h1>SnapFan &ndash; Setup</h1>
  </div>
  <div class="sub">Conectado al punto de acceso de configuraci&oacute;n</div>
  <form method="POST" action="/savewifi">
    <label id="apLblSsid">Red WiFi (SSID)</label>
    <input type="text" id="apSsidInput" name="ssid" maxlength="32" required autocomplete="off" placeholder="Nombre de tu red WiFi">
    <label id="apLblPass">Contrase&ntilde;a</label>
    <input type="password" id="apPassInput" name="pass" maxlength="64" autocomplete="off" placeholder="Dejar vac&iacute;o si la red es abierta">
    <button type="submit" id="apSaveBtn">&#128274; Guardar y Conectar</button>
  </form>
  <div class="note" id="apNote">El ESP se reiniciar&aacute; e intentar&aacute; conectar a tu red.<br>Si falla, volver&aacute; a modo AP autom&aacute;ticamente.</div>
</div>
<script>
const AP_TR={es:{title:'SnapFan – Setup',sub:'Conectado al punto de acceso de configuración',ssid:'Red WiFi (SSID)',ssidPh:'Nombre de tu red WiFi',pass:'Contraseña',passPh:'Dejar vacío si la red es abierta',save:'🔒 Guardar y Conectar',note:'El ESP se reiniciará e intentará conectar a tu red.<br>Si falla, volverá a modo AP automáticamente.'},en:{title:'SnapFan – Setup',sub:'Connected to the configuration access point',ssid:'WiFi network (SSID)',ssidPh:'Your WiFi network name',pass:'Password',passPh:'Leave empty if the network is open',save:'🔒 Save and Connect',note:'The ESP will restart and try to connect to your network.<br>If it fails, it will return to AP mode automatically.'},fr:{title:'SnapFan – Configuration',sub:'Connecté au point d\'accès de configuration',ssid:'Réseau WiFi (SSID)',ssidPh:'Nom de votre réseau WiFi',pass:'Mot de passe',passPh:'Laisser vide si le réseau est ouvert',save:'🔒 Enregistrer et connecter',note:'L\'ESP redémarrera et essaiera de se connecter à votre réseau.<br>En cas d\'échec, il reviendra automatiquement en mode AP.'},it:{title:'SnapFan – Configurazione',sub:'Connesso al punto di accesso di configurazione',ssid:'Rete WiFi (SSID)',ssidPh:'Nome della tua rete WiFi',pass:'Password',passPh:'Lascia vuoto se la rete è aperta',save:'🔒 Salva e collega',note:'L\'ESP si riavvierà e proverà a collegarsi alla tua rete.<br>Se fallisce, tornerà automaticamente in modalità AP.'},pt:{title:'SnapFan – Configuração',sub:'Ligado ao ponto de acesso de configuração',ssid:'Rede WiFi (SSID)',ssidPh:'Nome da tua rede WiFi',pass:'Palavra-passe',passPh:'Deixa vazio se a rede for aberta',save:'🔒 Guardar e Ligar',note:'O ESP vai reiniciar e tentar ligar-se à tua rede.<br>Se falhar, voltará automaticamente ao modo AP.'},de:{title:'SnapFan – Einrichtung',sub:'Mit dem Konfigurationszugangspunkt verbunden',ssid:'WiFi-Netzwerk (SSID)',ssidPh:'Name Ihres WiFi-Netzwerks',pass:'Passwort',passPh:'Leer lassen, wenn das Netzwerk offen ist',save:'🔒 Speichern und Verbinden',note:'Der ESP startet neu und versucht, sich mit Ihrem Netzwerk zu verbinden.<br>Falls es fehlschlägt, kehrt er automatisch in den AP-Modus zurück.'},zh:{title:'SnapFan – 设置',sub:'已连接到配置接入点',ssid:'WiFi 网络 (SSID)',ssidPh:'你的 WiFi 网络名称',pass:'密码',passPh:'如果网络开放请留空',save:'🔒 保存并连接',note:'ESP 将重启并尝试连接你的网络。<br>如果失败，它会自动返回 AP 模式。'}};
function apSetLang(lang){const t=AP_TR[lang]||AP_TR.es;document.documentElement.lang=lang;document.querySelector('.hdr h1').textContent=t.title;document.querySelector('.sub').textContent=t.sub;document.getElementById('apLblSsid').textContent=t.ssid;document.getElementById('apSsidInput').placeholder=t.ssidPh;document.getElementById('apLblPass').textContent=t.pass;document.getElementById('apPassInput').placeholder=t.passPh;document.getElementById('apSaveBtn').textContent=t.save;document.getElementById('apNote').innerHTML=t.note;}
fetch('/status').then(r=>r.json()).then(d=>apSetLang(d.lang||'es')).catch(()=>apSetLang('es'));
</script></body></html>
)rawliteral";

// ─── OTA Page ─────────────────────────────────────────────────────────────────
const char OTA_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="es"><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1.0">
<title>SnapFan OTA</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'Segoe UI',sans-serif;background:#111827;color:#e2e8f0;min-height:100vh;padding:16px;display:flex;flex-direction:column;align-items:center}
.topbar{width:100%;max-width:520px;display:flex;align-items:center;justify-content:space-between;margin-bottom:18px}
.back-btn{display:flex;align-items:center;gap:6px;padding:7px 14px;background:#1e2535;border:1px solid #2d3a52;border-radius:8px;color:#60a5fa;text-decoration:none;font-size:.82rem;cursor:pointer}
.back-btn:hover{background:#273550}
.hdr{display:flex;align-items:center;gap:10px}
.hdr svg{width:30px;height:30px}
.htitle{color:#60a5fa;font-size:1rem;font-weight:700}
.card{background:#1e2535;border-radius:14px;padding:24px;width:100%;max-width:520px;border:1px solid #2d3a52;box-shadow:0 8px 32px #0006}
.card h2{color:#93c5fd;font-size:.85rem;text-transform:uppercase;letter-spacing:2px;margin-bottom:16px;text-align:center}
.dropzone{border:2px dashed #2d3a52;border-radius:10px;padding:32px 16px;text-align:center;cursor:pointer;transition:all .3s;background:#0f172a;margin-bottom:16px}
.dropzone.over{border-color:#3b82f6;background:#0f2044}
.dropzone svg{margin:0 auto 10px;display:block;opacity:.5}
.dropzone p{font-size:.85rem;color:#64748b}
.dropzone p span{color:#60a5fa;font-weight:600}
#fileInfo{display:none;background:#0f172a;border-radius:8px;padding:10px 14px;margin-bottom:14px;font-size:.82rem;border:1px solid #2d3a52}
#fileInfo .fname{color:#60a5fa;font-weight:600;word-break:break-all}
#fileInfo .fsize{color:#94a3b8;margin-top:2px}
.pbar-wrap{height:10px;background:#1f2937;border-radius:5px;overflow:hidden;margin-bottom:6px;display:none}
.pbar{height:100%;width:0%;background:linear-gradient(90deg,#2563eb,#38bdf8);border-radius:5px;transition:width .3s}
.pct{font-size:.75rem;color:#64748b;margin-bottom:12px;display:none}
.upbtn{width:100%;padding:11px;background:#1d4ed8;color:#fff;border:none;border-radius:8px;font-size:.95rem;cursor:pointer;font-weight:600;display:flex;align-items:center;justify-content:center;gap:8px}
.upbtn:hover:not(:disabled){background:#2563eb}
.upbtn:disabled{opacity:.4;cursor:not-allowed}
.result{margin-top:14px;padding:12px;border-radius:8px;font-size:.85rem;text-align:center;display:none}
.result.ok{background:#052e16;border:1px solid #166534;color:#4ade80}
.result.err{background:#450a0a;border:1px solid #7f1d1d;color:#f87171}
.note{font-size:.72rem;color:#475569;text-align:center;margin-top:14px;line-height:1.5}
</style></head><body>
<div class="topbar">
  <a href="/" class="back-btn" id="otaBackBtn">&#8592; Volver</a>
  <div class="hdr">
    <svg viewBox="0 0 64 64" fill="none" xmlns="http://www.w3.org/2000/svg">
      <rect x="8" y="44" width="48" height="12" rx="3" fill="#1e3a5f" stroke="#60a5fa" stroke-width="2"/>
      <rect x="14" y="36" width="36" height="10" rx="2" fill="#0f2a47" stroke="#3b82f6" stroke-width="1.5"/>
      <rect x="20" y="28" width="24" height="10" rx="2" fill="#0f2a47" stroke="#3b82f6" stroke-width="1.5"/>
      <line x1="22" y1="28" x2="22" y2="10" stroke="#60a5fa" stroke-width="2" stroke-linecap="round"/>
      <line x1="32" y1="28" x2="32" y2="10" stroke="#60a5fa" stroke-width="2" stroke-linecap="round"/>
      <line x1="42" y1="28" x2="42" y2="10" stroke="#60a5fa" stroke-width="2" stroke-linecap="round"/>
      <circle cx="22" cy="10" r="3" fill="#38bdf8"/>
      <circle cx="32" cy="10" r="3" fill="#fb923c"/>
      <circle cx="42" cy="10" r="3" fill="#4ade80"/>
    </svg>
    <span class="htitle" id="otaTitle">SnapFan &ndash; OTA Update</span>
  </div>
  <div style="width:80px"></div>
</div>
<div class="card">
  <h2 id="otaHeading">&#128230; Actualizaci&oacute;n de Firmware</h2>
  <div class="dropzone" id="dz" onclick="document.getElementById('fw').click()">
    <svg width="40" height="40" viewBox="0 0 24 24" fill="none" stroke="#60a5fa" stroke-width="1.5">
      <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/>
      <polyline points="17 8 12 3 7 8"/><line x1="12" y1="3" x2="12" y2="15"/>
    </svg>
    <p id="otaDropText">Arrastra el archivo <span>.bin</span> aqu&iacute;<br>o haz clic para seleccionar</p>
  </div>
  <input type="file" id="fw" accept=".bin" style="display:none">
  <div id="fileInfo"><div class="fname" id="fname"></div><div class="fsize" id="fsize"></div></div>
  <div class="pbar-wrap" id="pw"><div class="pbar" id="pb"></div></div>
  <div class="pct" id="pct">0%</div>
  <button class="upbtn" id="upbtn" onclick="doUpload()" disabled>
    <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
      <polyline points="16 16 12 12 8 16"/><line x1="12" y1="12" x2="12" y2="21"/>
      <path d="M20.39 18.39A5 5 0 0 0 18 9h-1.26A8 8 0 1 0 3 16.3"/>
    </svg>
    <span id="otaUploadBtnText">Subir Firmware</span>
  </button>
  <div class="result" id="res"></div>
  <div class="note" id="otaNote">&#9888;&#65039; El ESP se reiniciar&aacute; autom&aacute;ticamente al terminar.<br>No cierres esta p&aacute;gina hasta que finalice la carga.</div>
</div>
<script>
const OTA_TR={es:{back:'← Volver',title:'SnapFan – OTA Update',heading:'📦 Actualización de Firmware',drop:'Arrastra el archivo <span>.bin</span> aquí<br>o haz clic para seleccionar',upload:'Subir Firmware',note:'⚠️ El ESP se reiniciará automáticamente al terminar.<br>No cierres esta página hasta que finalice la carga.',uploaded:'✓ Firmware cargado. Reiniciando...',error:'✗ Error: ',conn:'✗ Error de conexión'},en:{back:'← Back',title:'SnapFan – OTA Update',heading:'📦 Firmware Update',drop:'Drag the <span>.bin</span> file here<br>or click to select it',upload:'Upload Firmware',note:'⚠️ The ESP will restart automatically when finished.<br>Do not close this page until the upload completes.',uploaded:'✓ Firmware uploaded. Restarting...',error:'✗ Error: ',conn:'✗ Connection error'},fr:{back:'← Retour',title:'SnapFan – Mise à jour OTA',heading:'📦 Mise à jour du firmware',drop:'Glissez le fichier <span>.bin</span> ici<br>ou cliquez pour le sélectionner',upload:'Téléverser le firmware',note:'⚠️ L\'ESP redémarrera automatiquement à la fin.<br>Ne fermez pas cette page avant la fin du transfert.',uploaded:'✓ Firmware téléversé. Redémarrage...',error:'✗ Erreur : ',conn:'✗ Erreur de connexion'},it:{back:'← Indietro',title:'SnapFan – Aggiornamento OTA',heading:'📦 Aggiornamento firmware',drop:'Trascina qui il file <span>.bin</span><br>oppure fai clic per selezionarlo',upload:'Carica firmware',note:'⚠️ L\'ESP si riavvierà automaticamente al termine.<br>Non chiudere questa pagina fino al completamento del caricamento.',uploaded:'✓ Firmware caricato. Riavvio...',error:'✗ Errore: ',conn:'✗ Errore di connessione'},pt:{back:'← Voltar',title:'SnapFan – Atualização OTA',heading:'📦 Atualização de Firmware',drop:'Arrasta o ficheiro <span>.bin</span> para aqui<br>ou clica para o selecionar',upload:'Carregar Firmware',note:'⚠️ O ESP vai reiniciar automaticamente no fim.<br>Não feches esta página até a carga terminar.',uploaded:'✓ Firmware carregado. A reiniciar...',error:'✗ Erro: ',conn:'✗ Erro de ligação'},de:{back:'← Zurück',title:'SnapFan – OTA-Update',heading:'📦 Firmware-Aktualisierung',drop:'Ziehen Sie die <span>.bin</span>-Datei hierher<br>oder klicken Sie zum Auswählen',upload:'Firmware hochladen',note:'⚠️ Der ESP startet nach Abschluss automatisch neu.<br>Schließen Sie diese Seite nicht, bevor der Upload fertig ist.',uploaded:'✓ Firmware hochgeladen. Neustart...',error:'✗ Fehler: ',conn:'✗ Verbindungsfehler'},zh:{back:'← 返回',title:'SnapFan – OTA 更新',heading:'📦 固件更新',drop:'将 <span>.bin</span> 文件拖到这里<br>或点击选择文件',upload:'上传固件',note:'⚠️ ESP 完成后会自动重启。<br>在上传结束前请不要关闭此页面。',uploaded:'✓ 固件已上传，正在重启...',error:'✗ 错误：',conn:'✗ 连接错误'}};
let OTA_LG='es';
function otaT(key){return (OTA_TR[OTA_LG]||OTA_TR.es)[key]||key;}
function otaSetLang(lang){OTA_LG=OTA_TR[lang]?lang:'es';document.documentElement.lang=OTA_LG;document.getElementById('otaBackBtn').textContent=otaT('back');document.getElementById('otaTitle').textContent=otaT('title');document.getElementById('otaHeading').innerHTML=otaT('heading');document.getElementById('otaDropText').innerHTML=otaT('drop');document.getElementById('otaUploadBtnText').textContent=otaT('upload');document.getElementById('otaNote').innerHTML=otaT('note');}
const dz=document.getElementById('dz'),fw=document.getElementById('fw');
const fi=document.getElementById('fileInfo'),fn=document.getElementById('fname'),fs=document.getElementById('fsize');
const pw=document.getElementById('pw'),pb=document.getElementById('pb'),pct=document.getElementById('pct');
const upbtn=document.getElementById('upbtn'),res=document.getElementById('res');
let file=null;
function setFile(f){
  file=f; fn.textContent=f.name; fs.textContent=(f.size/1024).toFixed(1)+' KB';
  fi.style.display='block'; upbtn.disabled=false;
  res.style.display='none';
}
fw.addEventListener('change',()=>{if(fw.files[0])setFile(fw.files[0]);});
dz.addEventListener('dragover',e=>{e.preventDefault();dz.classList.add('over');});
dz.addEventListener('dragleave',()=>dz.classList.remove('over'));
dz.addEventListener('drop',e=>{e.preventDefault();dz.classList.remove('over');if(e.dataTransfer.files[0])setFile(e.dataTransfer.files[0]);});
function doUpload(){
  if(!file)return;
  const fd=new FormData(); fd.append('firmware',file,file.name);
  const xhr=new XMLHttpRequest();
  xhr.open('POST','/update');
  xhr.upload.onprogress=e=>{
    if(e.lengthComputable){
      const p=Math.round(e.loaded/e.total*100);
      pw.style.display='block'; pct.style.display='block';
      pb.style.width=p+'%'; pct.textContent=p+'%';
    }
  };
  xhr.onload=()=>{
    res.style.display='block';
    if(xhr.status==200){res.className='result ok';res.textContent=otaT('uploaded');}
    else{res.className='result err';res.textContent=otaT('error')+xhr.responseText;}
  };
  xhr.onerror=()=>{res.style.display='block';res.className='result err';res.textContent=otaT('conn');};
  upbtn.disabled=true; xhr.send(fd);
}
fetch('/status').then(r=>r.json()).then(d=>otaSetLang(d.lang||'es')).catch(()=>otaSetLang('es'));
</script>
</body></html>
)rawliteral";

const char WIFI_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="es"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1.0">
<title>SnapFan - WiFi</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'Segoe UI',sans-serif;background:radial-gradient(circle at top,#17304f 0,#0f172a 42%,#09111e 100%);color:#e2e8f0;min-height:100vh;padding:14px}
.shell{max-width:980px;margin:0 auto}.top{display:flex;align-items:center;justify-content:space-between;gap:10px;flex-wrap:wrap;margin-bottom:14px}.back{display:inline-flex;align-items:center;gap:6px;padding:8px 14px;background:#1e293b;border:1px solid #334155;border-radius:10px;color:#93c5fd;text-decoration:none;font-size:.84rem;font-weight:600}.back:hover{background:#243244}.lang{padding:7px 10px;border-radius:10px;border:1px solid #334155;background:#1e293b;color:#e2e8f0}
.hero{background:linear-gradient(135deg,#162339,#1e293b 60%,#0f172a);border:1px solid #334155;border-radius:18px;padding:18px 18px 16px;margin-bottom:14px;box-shadow:0 14px 40px #0005}.eyebrow{font-size:.72rem;letter-spacing:2px;text-transform:uppercase;color:#60a5fa;margin-bottom:6px}.hero h1{font-size:1.35rem;color:#eff6ff;margin-bottom:6px}.hero p{font-size:.86rem;color:#94a3b8;line-height:1.5}
.statusgrid{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:10px;margin-top:14px}.statuscard{background:#0f172a;border:1px solid #253246;border-radius:12px;padding:12px;text-align:center}.statusk{font-size:.7rem;color:#94a3b8;text-transform:uppercase;letter-spacing:1px;margin-bottom:6px}.statusv{font-size:.98rem;font-weight:700;color:#e2e8f0;word-break:break-word}
.content{display:grid;grid-template-columns:1.15fr .85fr;gap:14px}.panel{background:#1e293b;border:1px solid #334155;border-radius:16px;padding:16px}.panel h2{color:#bfdbfe;font-size:.88rem;letter-spacing:2px;text-transform:uppercase;margin-bottom:12px}.scanbar{display:flex;align-items:center;justify-content:space-between;gap:8px;flex-wrap:wrap;margin-bottom:12px}.scanmsg{font-size:.8rem;color:#94a3b8}.btn{border:none;border-radius:10px;padding:10px 14px;font-size:.84rem;font-weight:700;cursor:pointer}.btn.blue{background:#2563eb;color:#eff6ff}.btn.blue:hover{background:#1d4ed8}.btn.dark{background:#0f172a;color:#93c5fd;border:1px solid #334155}.btn.dark:hover{background:#162235}
.nets{display:grid;gap:10px;max-height:460px;overflow:auto;padding-right:4px}.net{background:#0f172a;border:1px solid #253246;border-radius:12px;padding:12px;display:flex;align-items:center;justify-content:space-between;gap:10px;cursor:pointer;transition:transform .18s ease,border-color .18s ease,background .18s ease}.net:hover{transform:translateY(-1px);border-color:#60a5fa;background:#142033}.net.sel{border-color:#f59e0b;box-shadow:0 0 0 1px #f59e0b inset}.netname{font-size:.96rem;font-weight:700;color:#eff6ff;margin-bottom:4px;word-break:break-word}.netmeta{font-size:.74rem;color:#94a3b8;display:flex;gap:8px;flex-wrap:wrap}.pill{display:inline-flex;align-items:center;gap:4px;padding:4px 8px;border-radius:999px;background:#182437;border:1px solid #2a3a53;color:#cbd5e1}.sec{background:#052e16;border-color:#14532d;color:#bbf7d0}.open{background:#1f2937;border-color:#374151;color:#d1d5db}
.formgrid{display:grid;gap:12px}.field label{display:block;font-size:.76rem;color:#94a3b8;margin-bottom:5px}.field input{width:100%;padding:10px 11px;border-radius:10px;border:1px solid #334155;background:#0f172a;color:#e2e8f0;font-size:.92rem}.field input:focus{outline:none;border-color:#60a5fa}.note{font-size:.78rem;color:#94a3b8;line-height:1.5}.toast{display:none;margin-top:12px;padding:11px 12px;border-radius:12px;font-size:.84rem}.toast.show{display:block}.toast.ok{background:#052e16;border:1px solid #166534;color:#86efac}.toast.err{background:#450a0a;border:1px solid #991b1b;color:#fca5a5}
@media (max-width: 820px){.content{grid-template-columns:1fr}.nets{max-height:none}}
</style></head><body>
<div class="shell">
  <div class="top">
    <a class="back" href="/" id="backBtn">&#8592; Volver al panel</a>
    <select class="lang" id="langSel" onchange="setLang(this.value)"><option value="es">🇪🇸 ES</option><option value="en">🇬🇧 EN</option><option value="fr">🇫🇷 FR</option><option value="it">🇮🇹 IT</option><option value="pt">🇵🇹 PT</option><option value="de">🇩🇪 DE</option><option value="zh">🇨🇳 中文</option></select>
  </div>
  <section class="hero">
    <div class="eyebrow" id="heroEyebrow">Administrador WiFi</div>
    <h1 id="heroTitle">Conecta SnapFan a otra red</h1>
    <p id="heroText">Pulsa en una red detectada para rellenar el SSID, introduce la contraseña si hace falta y guarda. El equipo reiniciará para conectarse.</p>
    <div class="statusgrid">
      <div class="statuscard"><div class="statusk" id="kSsid">SSID actual</div><div class="statusv" id="curSsid">--</div></div>
      <div class="statuscard"><div class="statusk" id="kIp">IP actual</div><div class="statusv" id="curIp">--</div></div>
      <div class="statuscard"><div class="statusk" id="kSignal">Señal</div><div class="statusv" id="curSignal">--</div></div>
      <div class="statuscard"><div class="statusk" id="kHost">Host</div><div class="statusv" id="curHost">--</div></div>
      <div class="statuscard"><div class="statusk" id="kClock">Hora</div><div class="statusv" id="curClock">--</div></div>
      <div class="statuscard"><div class="statusk" id="kSync">NTP</div><div class="statusv" id="curSync">--</div></div>
    </div>
  </section>
  <div class="content">
    <section class="panel">
      <div class="scanbar">
        <h2 id="scanTitle">Redes detectadas</h2>
        <button class="btn dark" id="scanBtn" onclick="scanWifi()">Escanear de nuevo</button>
      </div>
      <div class="scanmsg" id="scanMsg">Buscando redes WiFi cercanas...</div>
      <div class="nets" id="scanList"></div>
    </section>
    <section class="panel">
      <h2 id="formTitle">Conectar a la red</h2>
      <div class="formgrid">
        <div class="field"><label id="lblSsid" for="ssidInput">SSID</label><input id="ssidInput" maxlength="32" autocomplete="off"></div>
        <div class="field"><label id="lblPass" for="passInput">Contraseña</label><input id="passInput" type="password" maxlength="64" autocomplete="off"></div>
        <button class="btn blue" id="saveBtn" onclick="saveWifi()">Guardar y reconectar</button>
        <p class="note" id="wifiNote">Si la red es abierta, deja la contraseña vacía. Al guardar, el ESP32-C3 reiniciará e intentará enlazar con esa red.</p>
        <div class="toast" id="toast"></div>
      </div>
    </section>
  </div>
</div>
<script>
const TR={es:{back:'\u2190 Volver al panel',eyebrow:'Administrador WiFi',title:'Conecta SnapFan a otra red',hero:'Pulsa en una red detectada para rellenar el SSID, introduce la contraseña si hace falta y guarda. El equipo reiniciará para conectarse.',ssidNow:'SSID actual',ipNow:'IP actual',signal:'Señal',host:'Host',clock:'Hora',sync:'NTP',scanTitle:'Redes detectadas',scanBtn:'Escanear de nuevo',scanIdle:'Selecciona una red o lanza un nuevo escaneo.',scanRun:'Buscando redes WiFi cercanas...',scanNone:'No se han encontrado redes visibles.',scanErr:'No se pudo escanear ahora mismo.',formTitle:'Conectar a la red',lblSsid:'SSID',lblPass:'Contraseña',save:'Guardar y reconectar',note:'Si la red es abierta, deja la contraseña vacía. Al guardar, el ESP32-C3 reiniciará e intentará enlazar con esa red.',selectHint:'Red seleccionada',open:'Abierta',secure:'Protegida',saved:'WiFi guardado. Reiniciando...',saveErr:'No se pudo guardar la nueva WiFi.',ssidReq:'Introduce el SSID',noWifi:'Sin WiFi',noTime:'Sin hora',synced:'Sincronizada',unsynced:'Pendiente',dayClock:'Hora local'},en:{back:'\u2190 Back to dashboard',eyebrow:'WiFi manager',title:'Connect SnapFan to another network',hero:'Tap a detected network to fill the SSID, enter the password if needed, and save. The device will reboot and reconnect.',ssidNow:'Current SSID',ipNow:'Current IP',signal:'Signal',host:'Host',clock:'Time',sync:'NTP',scanTitle:'Detected networks',scanBtn:'Scan again',scanIdle:'Select a network or start a new scan.',scanRun:'Scanning nearby WiFi networks...',scanNone:'No visible networks were found.',scanErr:'WiFi scan failed right now.',formTitle:'Connect to network',lblSsid:'SSID',lblPass:'Password',save:'Save and reconnect',note:'If the network is open, leave the password empty. After saving, the ESP32-C3 will reboot and try to join that network.',selectHint:'Selected network',open:'Open',secure:'Secured',saved:'WiFi saved. Restarting...',saveErr:'Could not save the new WiFi.',ssidReq:'Enter SSID',noWifi:'No WiFi',noTime:'No time',synced:'Synced',unsynced:'Pending',dayClock:'Local time'},fr:{back:'\u2190 Retour au tableau de bord',eyebrow:'Gestionnaire WiFi',title:'Connecter SnapFan à un autre réseau',hero:'Touchez un réseau détecté pour remplir le SSID, saisissez le mot de passe si nécessaire, puis enregistrez. L\'appareil redémarrera et se reconnectera.',ssidNow:'SSID actuel',ipNow:'IP actuelle',signal:'Signal',host:'Hôte',clock:'Heure',sync:'NTP',scanTitle:'Réseaux détectés',scanBtn:'Relancer le scan',scanIdle:'Sélectionnez un réseau ou lancez un nouveau scan.',scanRun:'Recherche des réseaux WiFi à proximité...',scanNone:'Aucun réseau visible trouvé.',scanErr:'Impossible de lancer le scan WiFi pour le moment.',formTitle:'Se connecter au réseau',lblSsid:'SSID',lblPass:'Mot de passe',save:'Enregistrer et reconnecter',note:'Si le réseau est ouvert, laissez le mot de passe vide. Après l\'enregistrement, l\'ESP32-C3 redémarrera et essaiera de rejoindre ce réseau.',selectHint:'Réseau sélectionné',open:'Ouvert',secure:'Sécurisé',saved:'WiFi enregistré. Redémarrage...',saveErr:'Impossible d\'enregistrer le nouveau WiFi.',ssidReq:'Saisissez le SSID',noWifi:'Pas de WiFi',noTime:'Pas d\'heure',synced:'Synchronisé',unsynced:'En attente',dayClock:'Heure locale'},it:{back:'\u2190 Torna al pannello',eyebrow:'Gestore WiFi',title:'Collega SnapFan a un\'altra rete',hero:'Tocca una rete rilevata per compilare l\'SSID, inserisci la password se necessaria e salva. Il dispositivo si riavvierà e si ricollegherà.',ssidNow:'SSID attuale',ipNow:'IP attuale',signal:'Segnale',host:'Host',clock:'Ora',sync:'NTP',scanTitle:'Reti rilevate',scanBtn:'Scansiona di nuovo',scanIdle:'Seleziona una rete o avvia una nuova scansione.',scanRun:'Scansione delle reti WiFi vicine...',scanNone:'Nessuna rete visibile trovata.',scanErr:'Impossibile eseguire la scansione WiFi in questo momento.',formTitle:'Connetti alla rete',lblSsid:'SSID',lblPass:'Password',save:'Salva e riconnetti',note:'Se la rete è aperta, lascia vuota la password. Dopo il salvataggio, l\'ESP32-C3 si riavvierà e proverà a collegarsi a quella rete.',selectHint:'Rete selezionata',open:'Aperta',secure:'Protetta',saved:'WiFi salvato. Riavvio...',saveErr:'Impossibile salvare il nuovo WiFi.',ssidReq:'Inserisci l\'SSID',noWifi:'Nessun WiFi',noTime:'Nessuna ora',synced:'Sincronizzato',unsynced:'In attesa',dayClock:'Ora locale'},pt:{back:'\u2190 Voltar ao painel',eyebrow:'Gestor de WiFi',title:'Ligar o SnapFan a outra rede',hero:'Toque numa rede detetada para preencher o SSID, introduza a palavra-passe se necessário e guarde. O dispositivo vai reiniciar e voltar a ligar-se.',ssidNow:'SSID atual',ipNow:'IP atual',signal:'Sinal',host:'Host',clock:'Hora',sync:'NTP',scanTitle:'Redes detetadas',scanBtn:'Pesquisar novamente',scanIdle:'Selecione uma rede ou inicie uma nova pesquisa.',scanRun:'A procurar redes WiFi próximas...',scanNone:'Não foram encontradas redes visíveis.',scanErr:'Não foi possível pesquisar WiFi agora.',formTitle:'Ligar à rede',lblSsid:'SSID',lblPass:'Palavra-passe',save:'Guardar e voltar a ligar',note:'Se a rede estiver aberta, deixe a palavra-passe vazia. Após guardar, o ESP32-C3 vai reiniciar e tentar ligar-se a essa rede.',selectHint:'Rede selecionada',open:'Aberta',secure:'Protegida',saved:'WiFi guardado. A reiniciar...',saveErr:'Não foi possível guardar o novo WiFi.',ssidReq:'Introduza o SSID',noWifi:'Sem WiFi',noTime:'Sem hora',synced:'Sincronizado',unsynced:'Pendente',dayClock:'Hora local'},de:{back:'\u2190 Zurück zum Dashboard',eyebrow:'WiFi-Verwaltung',title:'SnapFan mit einem anderen Netzwerk verbinden',hero:'Wählen Sie ein erkanntes Netzwerk aus, um die SSID zu übernehmen, geben Sie bei Bedarf das Passwort ein und speichern Sie. Das Gerät startet neu und verbindet sich erneut.',ssidNow:'Aktuelle SSID',ipNow:'Aktuelle IP',signal:'Signal',host:'Host',clock:'Uhrzeit',sync:'NTP',scanTitle:'Erkannte Netzwerke',scanBtn:'Erneut scannen',scanIdle:'Wählen Sie ein Netzwerk oder starten Sie einen neuen Scan.',scanRun:'Suche nach verfügbaren WLAN-Netzwerken...',scanNone:'Keine sichtbaren Netzwerke gefunden.',scanErr:'WLAN-Scan konnte gerade nicht ausgeführt werden.',formTitle:'Mit Netzwerk verbinden',lblSsid:'SSID',lblPass:'Passwort',save:'Speichern und neu verbinden',note:'Wenn das Netzwerk offen ist, lassen Sie das Passwort leer. Nach dem Speichern startet der ESP32-C3 neu und versucht, sich mit diesem Netzwerk zu verbinden.',selectHint:'Ausgewähltes Netzwerk',open:'Offen',secure:'Geschützt',saved:'WiFi gespeichert. Neustart...',saveErr:'Das neue WiFi konnte nicht gespeichert werden.',ssidReq:'SSID eingeben',noWifi:'Kein WiFi',noTime:'Keine Uhrzeit',synced:'Synchronisiert',unsynced:'Ausstehend',dayClock:'Ortszeit'},zh:{back:'\u2190 返回面板',eyebrow:'WiFi 管理',title:'将 SnapFan 连接到其他网络',hero:'点击已检测到的网络以填入 SSID，如有需要请输入密码并保存。设备将重启并重新连接。',ssidNow:'当前 SSID',ipNow:'当前 IP',signal:'信号',host:'主机',clock:'时间',sync:'NTP',scanTitle:'检测到的网络',scanBtn:'重新扫描',scanIdle:'请选择一个网络或重新开始扫描。',scanRun:'正在扫描附近的 WiFi 网络...',scanNone:'未找到可见网络。',scanErr:'当前无法执行 WiFi 扫描。',formTitle:'连接到网络',lblSsid:'SSID',lblPass:'密码',save:'保存并重新连接',note:'如果网络是开放的，请将密码留空。保存后，ESP32-C3 将重启并尝试连接该网络。',selectHint:'已选网络',open:'开放',secure:'受保护',saved:'WiFi 已保存，正在重启...',saveErr:'无法保存新的 WiFi。',ssidReq:'请输入 SSID',noWifi:'无 WiFi',noTime:'无时间',synced:'已同步',unsynced:'等待中',dayClock:'本地时间'}};
const LOCALE_MAP={es:'es-ES',en:'en-GB',fr:'fr-FR',it:'it-IT',pt:'pt-PT',de:'de-DE',zh:'zh-CN'};
let LG=TR[localStorage.getItem('lang')]?localStorage.getItem('lang'):'es';
let scanResults=[];
function t(k){return TR[LG][k]||k;}
function showToast(msg,kind){const el=document.getElementById('toast');el.textContent=msg;el.className='toast show '+kind;}
function clearToast(){const el=document.getElementById('toast');el.className='toast';el.textContent='';}
function formatClock(epoch){if(!epoch)return t('noTime');const loc=LOCALE_MAP[LG]||'es-ES';return new Date(epoch*1000).toLocaleString(loc,{day:'2-digit',month:'2-digit',year:'numeric',hour:'2-digit',minute:'2-digit',second:'2-digit'});}
function formatSignal(rssi){if(typeof rssi!=='number'||rssi<=-126)return '--';const quality=Math.max(0,Math.min(100,Math.round(2*(rssi+100))));return rssi+' dBm · '+quality+'%';}
function securityLabel(secure){return secure?t('secure'):t('open');}
function renderNetworks(){const host=document.getElementById('scanList');host.innerHTML='';if(!scanResults.length){document.getElementById('scanMsg').textContent=t('scanNone');return;}document.getElementById('scanMsg').textContent=t('scanIdle');const selected=document.getElementById('ssidInput').value.trim();scanResults.forEach(net=>{const row=document.createElement('button');row.type='button';row.className='net'+(selected===net.ssid?' sel':'');row.onclick=()=>selectNetwork(net);const left=document.createElement('div');const name=document.createElement('div');name.className='netname';name.textContent=net.ssid||'(hidden)';const meta=document.createElement('div');meta.className='netmeta';meta.innerHTML='<span class="pill">📶 '+formatSignal(net.rssi)+'</span><span class="pill '+(net.secure?'sec':'open')+'">'+securityLabel(net.secure)+'</span><span class="pill">CH '+net.channel+'</span>';left.appendChild(name);left.appendChild(meta);const right=document.createElement('div');right.className='pill';right.textContent=t('selectHint');row.appendChild(left);row.appendChild(right);host.appendChild(row);});}
function selectNetwork(net){document.getElementById('ssidInput').value=net.ssid||'';document.getElementById('passInput').focus();renderNetworks();clearToast();}
async function scanWifi(){clearToast();document.getElementById('scanMsg').textContent=t('scanRun');document.getElementById('scanList').innerHTML='';try{const res=await fetch('/wifi/scan');if(!res.ok)throw new Error('scan');const data=await res.json();scanResults=(data.nets||[]).filter(net=>net.ssid);renderNetworks();}catch(e){scanResults=[];renderNetworks();document.getElementById('scanMsg').textContent=t('scanErr');showToast(t('scanErr'),'err');}}
async function persistLang(lang){try{await fetch('/setlang?lang='+encodeURIComponent(lang));}catch(e){}}
async function loadStatus(){try{const d=await(await fetch('/status')).json();if(d.lang&&TR[d.lang]&&d.lang!==LG){setLang(d.lang,false,false);}document.getElementById('curSsid').textContent=d.ssid||t('noWifi');document.getElementById('curIp').textContent=d.ip||'--';document.getElementById('curSignal').textContent=formatSignal(Number(d.rssi));document.getElementById('curHost').textContent=d.host||'--';document.getElementById('curClock').textContent=formatClock(parseInt(d.epoch)||0);document.getElementById('curSync').textContent=d.ntp?t('synced'):t('unsynced');}catch(e){}}
async function saveWifi(){const ssid=document.getElementById('ssidInput').value.trim();const pass=document.getElementById('passInput').value;if(!ssid){showToast(t('ssidReq'),'err');return;}try{const res=await fetch('/savewifi',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'ssid='+encodeURIComponent(ssid)+'&pass='+encodeURIComponent(pass)+'&noreset=1'});if(!res.ok)throw new Error('save');showToast(t('saved'),'ok');setTimeout(()=>{window.location='/';},2500);}catch(e){showToast(t('saveErr'),'err');}}
function setLang(lang,persist=true,refreshStatus=true){LG=TR[lang]?lang:'es';localStorage.setItem('lang',LG);document.documentElement.lang=LG;document.getElementById('langSel').value=LG;document.getElementById('backBtn').textContent=t('back');document.getElementById('heroEyebrow').textContent=t('eyebrow');document.getElementById('heroTitle').textContent=t('title');document.getElementById('heroText').textContent=t('hero');document.getElementById('kSsid').textContent=t('ssidNow');document.getElementById('kIp').textContent=t('ipNow');document.getElementById('kSignal').textContent=t('signal');document.getElementById('kHost').textContent=t('host');document.getElementById('kClock').textContent=t('clock');document.getElementById('kSync').textContent=t('sync');document.getElementById('scanTitle').textContent=t('scanTitle');document.getElementById('scanBtn').textContent=t('scanBtn');document.getElementById('formTitle').textContent=t('formTitle');document.getElementById('lblSsid').textContent=t('lblSsid');document.getElementById('lblPass').textContent=t('lblPass');document.getElementById('saveBtn').textContent=t('save');document.getElementById('wifiNote').textContent=t('note');renderNetworks();if(refreshStatus)loadStatus();if(!scanResults.length){document.getElementById('scanMsg').textContent=t('scanRun');}if(persist)persistLang(LG);}
window.addEventListener('load',()=>{setLang(LG);loadStatus();scanWifi();});
</script></body></html>
)rawliteral";

// ─── Página web principal ─────────────────────────────────────────────────────
const char HTML_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="es"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1.0">
<title>SnapFan - Snapmaker U1</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'Segoe UI',sans-serif;background:radial-gradient(circle at top,#1e3a5f 0,#111827 35%,#0b1220 100%);color:#e2e8f0;min-height:100vh;padding:12px 16px}
.topbar{display:grid;grid-template-columns:auto minmax(260px,1fr) auto;align-items:center;margin-bottom:14px;gap:10px}.brand{display:flex;align-items:center;gap:10px}.brand svg{width:38px;height:38px;flex-shrink:0}.brand-txt h1{font-size:1.05rem;font-weight:700;color:#60a5fa;line-height:1.1}.brand-txt p{font-size:.68rem;color:#64748b}.topright{display:flex;align-items:center;gap:6px;flex-wrap:wrap;justify-content:flex-end}.topnet{display:grid;grid-template-columns:repeat(4,minmax(90px,1fr));gap:8px;min-width:0}.topnet-item{background:#1e2535;border:1px solid #2d3a52;border-radius:10px;padding:8px 10px;text-align:center;min-width:0}.topnet-k{font-size:.63rem;color:#94a3b8;letter-spacing:.4px;text-transform:uppercase;margin-bottom:3px}.topnet-v{font-size:.8rem;font-weight:700;color:#e2e8f0;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.langsel,.tbtn,input{border-radius:8px}.langsel{padding:5px 8px;border:1px solid #2d3a52;background:#1e2535;color:#e2e8f0;font-size:.78rem;cursor:pointer;outline:none}.tbtn{padding:6px 12px;border:1px solid #2d3a52;background:#1e2535;font-size:.78rem;cursor:pointer;font-weight:600;text-decoration:none;display:inline-flex;align-items:center;gap:5px;color:#e2e8f0}.tbtn:hover{background:#273550}.tbtn.blue{color:#60a5fa;border-color:#1d4ed8}.tbtn.orange{color:#fb923c}.tbtn.red{color:#f87171}.tbtn.green{color:#4ade80;border-color:#166534}.btn-ico{font-size:.95rem;line-height:1}.creator{margin-top:6px;text-align:center}.creator a,.social-links a{color:#64748b;font-size:.68rem;text-decoration:none}.creator a:hover,.social-links a:hover{color:#60a5fa}.creator strong{color:#cbd5e1}.social-links{margin-top:4px;text-align:center;display:flex;justify-content:center;gap:14px;flex-wrap:wrap}
.mbar{display:flex;align-items:center;justify-content:center;gap:8px;margin-bottom:14px;flex-wrap:wrap;padding:10px;background:#1e2535;border-radius:12px;border:1px solid #2d3a52}.mbadge{padding:4px 12px;border-radius:20px;font-weight:700;font-size:.8rem}.mauto{background:#065f46;color:#6ee7b7}.mman{background:#7c2d12;color:#fca5a5}.btnm{padding:6px 14px;border:none;border-radius:8px;font-size:.82rem;cursor:pointer;font-weight:600}.btna{background:#3b82f6;color:#fff}.btnm2{background:#dc2626;color:#fff}.bsm{padding:4px 10px!important;font-size:.76rem!important}
.sensor-alert{display:none;max-width:860px;margin:0 auto 14px;padding:12px 14px;background:#4a1d1f;color:#fee2e2;border:1px solid #7f1d1d;border-radius:12px;font-size:.82rem;line-height:1.45}.sensor-alert.show{display:block}.sensor-alert strong{color:#fecaca}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(290px,1fr));gap:14px;max-width:860px;margin:0 auto}.card{background:#1e2535;border-radius:14px;padding:18px;border:1px solid #2d3a52}.card h2{color:#93c5fd;font-size:.8rem;text-transform:uppercase;letter-spacing:2px;margin-bottom:10px;text-align:center}.zmode-bar{display:flex;align-items:center;justify-content:center;gap:8px;margin-bottom:10px}
.stats{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin:10px 0}.stat{text-align:center;background:#0f172a;border:1px solid #263349;border-radius:10px;padding:10px 8px}.sv{font-size:1.8rem;font-weight:700}.sl{font-size:.68rem;color:#94a3b8;margin-top:2px}.cool{color:#34d399}.warm{color:#fbbf24}.hot{color:#f87171}.state{color:#60a5fa}
.fan-state{display:flex;align-items:center;justify-content:center;gap:8px;margin-bottom:12px}.dot{width:11px;height:11px;border-radius:50%;flex-shrink:0;transition:all .5s}.dot-on{background:#22c55e;box-shadow:0 0 8px #22c55e88}.dot-off{background:#374151}.fan-lbl{font-size:.78rem;font-weight:700;letter-spacing:1px}.fan-on-txt{color:#22c55e}.fan-off-txt{color:#6b7280}
.chart-wrap{position:relative;background:#0f172a;border-radius:8px;margin-bottom:12px;overflow:hidden;height:80px}canvas.chart{position:absolute;top:0;left:0;width:100%;height:100%}.chart-lbl{position:absolute;bottom:2px;left:6px;font-size:7px;color:#475569;pointer-events:none}.chart-lbl-r{position:absolute;bottom:2px;right:6px;font-size:7px;color:#475569;pointer-events:none}
.sec{border-top:1px solid #2d3a52;padding-top:12px;margin-top:2px}.st{font-size:.7rem;color:#64748b;text-transform:uppercase;letter-spacing:1px;margin-bottom:8px}.r2{display:grid;grid-template-columns:1fr 1fr;gap:8px}label{font-size:.74rem;color:#94a3b8;display:block;margin-bottom:2px}input[type=number],input[type=text],input[type=password]{width:100%;padding:7px 8px;border:1px solid #2d3a52;background:#0f172a;color:#e2e8f0;font-size:.88rem;outline:none}input:focus{border-color:#3b82f6}
.sv2{width:100%;margin-top:12px;padding:8px;background:#1d4ed8;color:#fff;border:none;border-radius:8px;font-size:.88rem;cursor:pointer}.sv2:hover{background:#2563eb}.power-actions{display:grid;grid-template-columns:1fr 1fr;gap:8px}.power-btn{padding:10px;border:none;border-radius:8px;font-size:.82rem;font-weight:700;cursor:pointer;transition:opacity .2s ease,filter .2s ease}.power-btn:disabled{opacity:.28;filter:saturate(.35) brightness(.7);cursor:not-allowed}.power-on{background:#166534;color:#dcfce7}.power-off{background:#7f1d1d;color:#fee2e2}
.overlay{display:none;position:fixed;inset:0;background:#000a;z-index:100;align-items:center;justify-content:center}.overlay.show{display:flex}.modal{background:#1e2535;border-radius:14px;padding:24px;width:90%;max-width:340px;border:1px solid #2d3a52;text-align:center}.modal p{margin-bottom:16px;font-size:.9rem;color:#94a3b8}.modal-btns{display:flex;gap:10px;justify-content:center}.modal-btns button{padding:8px 20px;border:none;border-radius:8px;font-size:.88rem;cursor:pointer;font-weight:600}
footer{text-align:center;color:#475569;font-size:.66rem;margin-top:16px}
@media (max-width: 980px){.topbar{grid-template-columns:1fr;justify-items:stretch}.topnet{grid-template-columns:repeat(2,minmax(120px,1fr))}.topright{justify-content:flex-start}}
@media (max-width: 560px){.topnet{grid-template-columns:1fr 1fr}.topnet-v{font-size:.76rem}}
</style></head><body>
<div class="topbar">
  <div class="brand">
    <svg viewBox="0 0 64 64" fill="none" xmlns="http://www.w3.org/2000/svg"><rect x="6" y="46" width="52" height="13" rx="3" fill="#1e3a5f" stroke="#60a5fa" stroke-width="2"/><rect x="10" y="38" width="44" height="10" rx="2" fill="#0f2a47" stroke="#3b82f6" stroke-width="1.5"/><rect x="16" y="30" width="32" height="10" rx="2" fill="#0f2a47" stroke="#3b82f6" stroke-width="1.5"/><rect x="22" y="22" width="20" height="10" rx="2" fill="#0f2a47" stroke="#60a5fa" stroke-width="1.5"/><line x1="26" y1="22" x2="26" y2="6" stroke="#60a5fa" stroke-width="2" stroke-linecap="round"/><line x1="32" y1="22" x2="32" y2="6" stroke="#60a5fa" stroke-width="2" stroke-linecap="round"/><line x1="38" y1="22" x2="38" y2="6" stroke="#60a5fa" stroke-width="2" stroke-linecap="round"/></svg>
    <div class="brand-txt"><h1>Snapmaker U1</h1><p>Fan Control 24V · ESP32-C3</p><div style="font-size:.68rem;color:#94a3b8;margin-top:2px">Firmware <span id="fwVersion">v0.0.0</span></div></div>
  </div>
  <div class="topnet">
    <div class="topnet-item"><div class="topnet-k" id="lblNetSsid">SSID</div><div class="topnet-v" id="netSsid">--</div></div>
    <div class="topnet-item"><div class="topnet-k" id="lblNetIp">IP</div><div class="topnet-v" id="netIp">--</div></div>
    <div class="topnet-item"><div class="topnet-k" id="lblNetSignal">Señal</div><div class="topnet-v" id="netSignal">--</div></div>
    <div class="topnet-item"><div class="topnet-k" id="lblNetTime">Hora</div><div class="topnet-v" id="netTime">--:--:--</div></div>
  </div>
  <div class="topright">
    <select class="langsel" id="langSel" onchange="setLang(this.value)"><option value="es">🇪🇸 ES</option><option value="en">🇬🇧 EN</option><option value="fr">🇫🇷 FR</option><option value="it">🇮🇹 IT</option><option value="pt">🇵🇹 PT</option><option value="de">🇩🇪 DE</option><option value="zh">🇨🇳 中文</option></select>
    <button class="tbtn green" onclick="checkForUpdates(true)" id="btnUpdates"><span class="btn-ico">⬇️</span><span>Updates</span></button>
    <button class="tbtn blue" onclick="showOTA()" id="btnOTA"><span class="btn-ico">📦</span><span>OTA</span></button>
    <button class="tbtn orange" onclick="openWifiPage()" id="btnWifi"><span class="btn-ico">📶</span><span>WiFi</span></button>
    <button class="tbtn green" onclick="confirmResetPeak()" id="btnResetPeak"><span class="btn-ico">🌡️</span><span>T.max</span></button>
    <button class="tbtn red" onclick="confirmRestart()" id="btnRestart"><span class="btn-ico">🔄</span><span>Reiniciar ESP</span></button>
  </div>
</div>
<div class="mbar"><span style="font-size:.74rem;color:#94a3b8" id="lblAllZones">Todas las zonas:</span><button class="btnm btna bsm" id="btnAllAuto" onclick="setAllMode('auto')">AUTO</button><button class="btnm btnm2 bsm" id="btnAllMan" onclick="setAllMode('manual')">MANUAL</button></div>
<div class="sensor-alert" id="sensorAlert"></div>
<div class="grid">
  <div class="card">
    <h2 id="z1title">&#9881; Drivers de Motores</h2><div id="sensInfo1" style="text-align:center;font-size:.72rem;color:#64748b;margin-bottom:2px">&#127777;&#65039; --</div>
    <div class="zmode-bar"><span class="mbadge mauto" id="zmdg1">AUTO</span><button class="btnm btnm2 bsm" id="zbtn1" onclick="toggleZoneMode(1)">MANUAL</button></div>
    <div style="text-align:center;margin-bottom:4px"><div class="sv cool" id="t1">--.-&deg;C</div><div class="sl" id="lblTemp1">Temperatura</div></div>
    <div class="stats"><div class="stat"><div class="sv state" id="mode1">AUTO</div><div class="sl" id="lblMode1">Modo</div></div><div class="stat"><div class="sv warm" id="mx1" style="font-size:1.2rem">--.-&deg;C</div><div class="sl" id="lblPeakT1">T. m&aacute;x</div></div></div>
    <div class="fan-state"><div class="dot dot-off" id="dot1"></div><span class="fan-lbl fan-off-txt" id="fanLbl1">APAGADO</span></div>
    <div class="chart-wrap"><canvas id="ch1" class="chart"></canvas><span class="chart-lbl" id="xlbl1">-3min</span><span class="chart-lbl-r" id="xnow1">ahora</span></div>
    <form onsubmit="saveZone(event,1)"><div class="sec" style="border-top:none;padding-top:8px"><div class="st" id="lblAuto1">Control autom&aacute;tico</div><div class="r2"><div><label id="lblTHigh1">Umbral ON (&deg;C)</label><input type="number" id="z1th" min="20" max="90" step="0.5" value="45"></div><div><label id="lblHyst1">Hist&eacute;resis (&deg;C)</label><input type="number" id="z1hy" min="1" max="20" step="0.5" value="10"></div></div></div><div class="sec"><div class="st" id="lblManual1">Control manual</div><div class="power-actions"><button type="button" class="power-btn power-on" id="z1on" onclick="setManual(1,1)">Encender</button><button type="button" class="power-btn power-off" id="z1off" onclick="setManual(1,0)">Apagar</button></div></div><button class="sv2" type="submit" id="btnSaveZ1">Guardar Zona 1</button></form>
  </div>
  <div class="card">
    <h2 id="z2title">&#9889; Fuente de Alimentaci&oacute;n</h2><div id="sensInfo2" style="text-align:center;font-size:.72rem;color:#64748b;margin-bottom:2px">&#127777;&#65039; --</div>
    <div class="zmode-bar"><span class="mbadge mauto" id="zmdg2">AUTO</span><button class="btnm btnm2 bsm" id="zbtn2" onclick="toggleZoneMode(2)">MANUAL</button></div>
    <div style="text-align:center;margin-bottom:4px"><div class="sv cool" id="t2">--.-&deg;C</div><div class="sl" id="lblTemp2">Temperatura</div></div>
    <div class="stats"><div class="stat"><div class="sv state" id="mode2">AUTO</div><div class="sl" id="lblMode2">Modo</div></div><div class="stat"><div class="sv warm" id="mx2" style="font-size:1.2rem">--.-&deg;C</div><div class="sl" id="lblPeakT2">T. m&aacute;x</div></div></div>
    <div class="fan-state"><div class="dot dot-off" id="dot2"></div><span class="fan-lbl fan-off-txt" id="fanLbl2">APAGADO</span></div>
    <div class="chart-wrap"><canvas id="ch2" class="chart"></canvas><span class="chart-lbl" id="xlbl2">-3min</span><span class="chart-lbl-r" id="xnow2">ahora</span></div>
    <form onsubmit="saveZone(event,2)"><div class="sec" style="border-top:none;padding-top:8px"><div class="st" id="lblAuto2">Control autom&aacute;tico</div><div class="r2"><div><label id="lblTHigh2">Umbral ON (&deg;C)</label><input type="number" id="z2th" min="20" max="90" step="0.5" value="50"></div><div><label id="lblHyst2">Hist&eacute;resis (&deg;C)</label><input type="number" id="z2hy" min="1" max="20" step="0.5" value="10"></div></div></div><div class="sec"><div class="st" id="lblManual2">Control manual</div><div class="power-actions"><button type="button" class="power-btn power-on" id="z2on" onclick="setManual(2,1)">Encender</button><button type="button" class="power-btn power-off" id="z2off" onclick="setManual(2,0)">Apagar</button></div></div><button class="sv2" type="submit" id="btnSaveZ2">Guardar Zona 2</button></form>
  </div>
</div>
<div class="overlay" id="otaOverlay"><div class="modal"><p id="otaMsg">Abriendo p&aacute;gina OTA...</p><div class="modal-btns"><button id="otaGoBtn" style="background:#1d4ed8;color:#fff" onclick="window.location='/update';hideOTA()">Ir a OTA</button><button id="otaCancelBtn" style="background:#374151;color:#e2e8f0" onclick="hideOTA()">Cancelar</button></div></div></div>
<div class="overlay" id="peakOverlay"><div class="modal"><p id="peakMsg">&iquest;Borrar T. m&aacute;x registradas?</p><div class="modal-btns"><button id="peakYesBtn" style="background:#16a34a;color:#fff" onclick="doResetPeak()">S&iacute;</button><button id="peakNoBtn" style="background:#374151;color:#e2e8f0" onclick="hidePeakRst()">No</button></div></div></div>
<div class="overlay" id="rstOverlay"><div class="modal"><p id="rstMsg">&iquest;Reiniciar el ESP ahora?</p><div class="modal-btns"><button id="rstYesBtn" style="background:#dc2626;color:#fff" onclick="doRestart()">S&iacute;</button><button id="rstNoBtn" style="background:#374151;color:#e2e8f0" onclick="hideRst()">No</button></div></div></div>
<footer id="footer">ESP32-C3 · Snapmaker U1 Fan Control 24V · v0.0.0 · Auto-refresh 3 s</footer>
<div class="creator"><a href="#" onclick="return false;">Creado por <strong>Regata</strong></a></div>
<div class="social-links"><a href="https://t.me/regata3dprint" target="_blank" rel="noopener noreferrer">📨 Telegram @regata3dprint</a><a href="https://instagram.com/regata3dprint" target="_blank" rel="noopener noreferrer">📷 Instagram @regata3dprint</a></div>
<script>
const TR={es:{z1title:'&#9881; Drivers de Motores',z2title:'&#9889; Fuente de Alimentaci\u00f3n',lblAllZones:'Todas las zonas:',lblTemp:'Temperatura',lblMode:'Modo',lblAuto:'Control autom\u00e1tico',lblTHigh:'Umbral ON (\u00b0C)',lblHyst:'Hist\u00e9resis (\u00b0C)',lblPeakT:'T. m\u00e1x',lblManual:'Control manual',btnSaveZ:'Guardar Zona ',btnOTA:'OTA',btnWifi:'WiFi',btnRestart:'Reiniciar ESP',btnUpdates:'Buscar actualizaciones',btnAllAuto:'AUTO',btnAllMan:'MANUAL',btnOn:'Encender',btnOff:'Apagar',modeAuto:'AUTO',modeManual:'MANUAL',fanOn:'ENCENDIDO',fanOff:'APAGADO',otaMsg:'Abriendo p\u00e1gina OTA...',rstMsg:'\u00bfReiniciar el ESP ahora?',peakMsg:'\u00bfBorrar T. m\u00e1x registradas?',btnResetPeak:'T.max',sensNone:'Sin sensor',sens1:'Sensor',sensZ1:'Zona 1',sensZ2:'Zona 2',sensShared:'Zonas 1+2',sensAlertNone:'No se detecta ning\u00fan DS18B20. Revisa GPIO4, 3.3V, GND y la resistencia pull-up de 4.7k.',sensAlertMissing:'Hay sensores DS18B20 sin lectura v\u00e1lida. Revisa cableado, conexiones o el propio sensor.',sensAlertZ1:'La zona 1 no tiene lectura v\u00e1lida.',sensAlertZ2:'La zona 2 no tiene lectura v\u00e1lida.',xlbl:'-3min',xnow:'ahora',footer:'ESP32-C3 · Snapmaker U1 Fan Control 24V · {version} · Auto-refresh 3 s',upToDate:'Ya est\u00e1s en la \u00faltima versi\u00f3n',updateAvail:'Hay una nueva versi\u00f3n disponible: {latest}. Tu firmware actual es {current}. \u00bfQuieres abrir la descarga?',updateOpenRelease:'No se encontr\u00f3 un .bin en la release. Se abrir\u00e1 la p\u00e1gina de GitHub.',updateError:'No se pudo comprobar GitHub ahora mismo.',saveError:'Error al guardar'},en:{z1title:'&#9881; Motor Drivers',z2title:'&#9889; Power Supply',lblAllZones:'All zones:',lblTemp:'Temperature',lblMode:'Mode',lblAuto:'Automatic control',lblTHigh:'ON threshold (\u00b0C)',lblHyst:'Hysteresis (\u00b0C)',lblPeakT:'Peak T',lblManual:'Manual control',btnSaveZ:'Save Zone ',btnOTA:'OTA',btnWifi:'WiFi',btnRestart:'Restart ESP',btnUpdates:'Check updates',btnAllAuto:'AUTO',btnAllMan:'MANUAL',btnOn:'Turn on',btnOff:'Turn off',modeAuto:'AUTO',modeManual:'MANUAL',fanOn:'ON',fanOff:'OFF',otaMsg:'Opening OTA page...',rstMsg:'Restart the ESP now?',peakMsg:'Clear recorded peak temperatures?',btnResetPeak:'Peak T',sensNone:'No sensor',sens1:'Sensor',sensZ1:'Zone 1',sensZ2:'Zone 2',sensShared:'Zones 1+2',sensAlertNone:'No DS18B20 detected. Check GPIO4, 3.3V, GND, and the 4.7k pull-up resistor.',sensAlertMissing:'There are DS18B20 sensors without a valid reading. Check wiring, connections, or the sensor itself.',sensAlertZ1:'Zone 1 has no valid reading.',sensAlertZ2:'Zone 2 has no valid reading.',xlbl:'-3min',xnow:'now',footer:'ESP32-C3 · Snapmaker U1 Fan Control 24V · {version} · Auto-refresh 3 s',upToDate:'You already have the latest version',updateAvail:'A new version is available: {latest}. Your current firmware is {current}. Open the download?',updateOpenRelease:'No .bin asset was found in the release. Opening GitHub release page.',updateError:'GitHub could not be checked right now.',saveError:'Save error'},fr:{z1title:'&#9881; Pilotes moteurs',z2title:'&#9889; Alimentation',lblAllZones:'Toutes les zones :',lblTemp:'Température',lblMode:'Mode',lblAuto:'Contrôle automatique',lblTHigh:'Seuil ON (\u00b0C)',lblHyst:'Hystérésis (\u00b0C)',lblPeakT:'T max',lblManual:'Contrôle manuel',btnSaveZ:'Enregistrer zone ',btnOTA:'OTA',btnWifi:'WiFi',btnRestart:'Redémarrer ESP',btnUpdates:'Rechercher des mises à jour',btnAllAuto:'AUTO',btnAllMan:'MANUEL',btnOn:'Allumer',btnOff:'Éteindre',modeAuto:'AUTO',modeManual:'MANUEL',fanOn:'ALLUMÉ',fanOff:'ÉTEINT',otaMsg:'Ouverture de la page OTA...',rstMsg:'Redémarrer l\'ESP maintenant ?',peakMsg:'Effacer les températures maximales enregistrées ?',btnResetPeak:'T.max',sensNone:'Aucun capteur',sens1:'Capteur',sensZ1:'Zone 1',sensZ2:'Zone 2',sensShared:'Zones 1+2',sensAlertNone:'Aucun DS18B20 détecté. Vérifiez GPIO4, 3.3V, GND et la résistance pull-up de 4.7k.',sensAlertMissing:'Des capteurs DS18B20 n\'ont pas de lecture valide. Vérifiez le câblage, les connexions ou le capteur.',sensAlertZ1:'La zone 1 n\'a pas de lecture valide.',sensAlertZ2:'La zone 2 n\'a pas de lecture valide.',xlbl:'-3min',xnow:'maintenant',footer:'ESP32-C3 · Snapmaker U1 Fan Control 24V · {version} · Actualisation auto 3 s',upToDate:'Vous avez déjà la dernière version',updateAvail:'Une nouvelle version est disponible : {latest}. Votre firmware actuel est {current}. Ouvrir le téléchargement ?',updateOpenRelease:'Aucun fichier .bin trouvé dans la release. Ouverture de la page GitHub.',updateError:'Impossible de vérifier GitHub pour le moment.',saveError:'Erreur lors de l\'enregistrement'},it:{z1title:'&#9881; Driver motori',z2title:'&#9889; Alimentazione',lblAllZones:'Tutte le zone:',lblTemp:'Temperatura',lblMode:'Modalità',lblAuto:'Controllo automatico',lblTHigh:'Soglia ON (\u00b0C)',lblHyst:'Isteresi (\u00b0C)',lblPeakT:'T max',lblManual:'Controllo manuale',btnSaveZ:'Salva zona ',btnOTA:'OTA',btnWifi:'WiFi',btnRestart:'Riavvia ESP',btnUpdates:'Cerca aggiornamenti',btnAllAuto:'AUTO',btnAllMan:'MANUALE',btnOn:'Accendi',btnOff:'Spegni',modeAuto:'AUTO',modeManual:'MANUALE',fanOn:'ACCESO',fanOff:'SPENTO',otaMsg:'Apertura pagina OTA...',rstMsg:'Riavviare ora l\'ESP?',peakMsg:'Cancellare le temperature massime registrate?',btnResetPeak:'T.max',sensNone:'Nessun sensore',sens1:'Sensore',sensZ1:'Zona 1',sensZ2:'Zona 2',sensShared:'Zone 1+2',sensAlertNone:'Nessun DS18B20 rilevato. Controlla GPIO4, 3.3V, GND e la resistenza pull-up da 4.7k.',sensAlertMissing:'Ci sono sensori DS18B20 senza una lettura valida. Controlla cablaggio, connessioni o il sensore.',sensAlertZ1:'La zona 1 non ha una lettura valida.',sensAlertZ2:'La zona 2 non ha una lettura valida.',xlbl:'-3min',xnow:'ora',footer:'ESP32-C3 · Snapmaker U1 Fan Control 24V · {version} · Aggiornamento auto 3 s',upToDate:'Hai già l\'ultima versione',updateAvail:'È disponibile una nuova versione: {latest}. Il firmware attuale è {current}. Aprire il download?',updateOpenRelease:'Nessun file .bin trovato nella release. Apertura pagina GitHub.',updateError:'Impossibile controllare GitHub in questo momento.',saveError:'Errore di salvataggio'},pt:{z1title:'&#9881; Drivers dos motores',z2title:'&#9889; Fonte de alimentação',lblAllZones:'Todas as zonas:',lblTemp:'Temperatura',lblMode:'Modo',lblAuto:'Controlo automático',lblTHigh:'Limite ON (\u00b0C)',lblHyst:'Histerese (\u00b0C)',lblPeakT:'T máx',lblManual:'Controlo manual',btnSaveZ:'Guardar zona ',btnOTA:'OTA',btnWifi:'WiFi',btnRestart:'Reiniciar ESP',btnUpdates:'Procurar atualizações',btnAllAuto:'AUTO',btnAllMan:'MANUAL',btnOn:'Ligar',btnOff:'Desligar',modeAuto:'AUTO',modeManual:'MANUAL',fanOn:'LIGADO',fanOff:'DESLIGADO',otaMsg:'A abrir página OTA...',rstMsg:'Reiniciar o ESP agora?',peakMsg:'Apagar temperaturas máximas registadas?',btnResetPeak:'T.max',sensNone:'Sem sensor',sens1:'Sensor',sensZ1:'Zona 1',sensZ2:'Zona 2',sensShared:'Zonas 1+2',sensAlertNone:'Nenhum DS18B20 detetado. Verifique GPIO4, 3.3V, GND e a resistência pull-up de 4.7k.',sensAlertMissing:'Existem sensores DS18B20 sem leitura válida. Verifique a cablagem, ligações ou o próprio sensor.',sensAlertZ1:'A zona 1 não tem leitura válida.',sensAlertZ2:'A zona 2 não tem leitura válida.',xlbl:'-3min',xnow:'agora',footer:'ESP32-C3 · Snapmaker U1 Fan Control 24V · {version} · Atualização auto 3 s',upToDate:'Já tem a versão mais recente',updateAvail:'Há uma nova versão disponível: {latest}. O firmware atual é {current}. Abrir a transferência?',updateOpenRelease:'Não foi encontrado nenhum .bin na release. A abrir página do GitHub.',updateError:'Não foi possível verificar o GitHub agora.',saveError:'Erro ao guardar'},de:{z1title:'&#9881; Motortreiber',z2title:'&#9889; Netzteil',lblAllZones:'Alle Zonen:',lblTemp:'Temperatur',lblMode:'Modus',lblAuto:'Automatische Steuerung',lblTHigh:'ON-Schwelle (\u00b0C)',lblHyst:'Hysterese (\u00b0C)',lblPeakT:'Max. T',lblManual:'Manuelle Steuerung',btnSaveZ:'Zone speichern ',btnOTA:'OTA',btnWifi:'WiFi',btnRestart:'ESP neu starten',btnUpdates:'Nach Updates suchen',btnAllAuto:'AUTO',btnAllMan:'MANUELL',btnOn:'Einschalten',btnOff:'Ausschalten',modeAuto:'AUTO',modeManual:'MANUELL',fanOn:'EIN',fanOff:'AUS',otaMsg:'OTA-Seite wird geöffnet...',rstMsg:'ESP jetzt neu starten?',peakMsg:'Gespeicherte Maximaltemperaturen löschen?',btnResetPeak:'T.max',sensNone:'Kein Sensor',sens1:'Sensor',sensZ1:'Zone 1',sensZ2:'Zone 2',sensShared:'Zonen 1+2',sensAlertNone:'Kein DS18B20 erkannt. Prüfen Sie GPIO4, 3.3V, GND und den 4.7k-Pull-up-Widerstand.',sensAlertMissing:'Es gibt DS18B20-Sensoren ohne gültigen Messwert. Prüfen Sie Verkabelung, Verbindungen oder den Sensor.',sensAlertZ1:'Zone 1 hat keinen gültigen Messwert.',sensAlertZ2:'Zone 2 hat keinen gültigen Messwert.',xlbl:'-3min',xnow:'jetzt',footer:'ESP32-C3 · Snapmaker U1 Fan Control 24V · {version} · Auto-Refresh 3 s',upToDate:'Sie haben bereits die neueste Version',updateAvail:'Eine neue Version ist verfügbar: {latest}. Ihre aktuelle Firmware ist {current}. Download öffnen?',updateOpenRelease:'Kein .bin-Asset in der Release gefunden. GitHub-Release-Seite wird geöffnet.',updateError:'GitHub konnte gerade nicht geprüft werden.',saveError:'Fehler beim Speichern'},zh:{z1title:'&#9881; 电机驱动',z2title:'&#9889; 电源',lblAllZones:'所有区域：',lblTemp:'温度',lblMode:'模式',lblAuto:'自动控制',lblTHigh:'开启阈值 (\u00b0C)',lblHyst:'迟滞 (\u00b0C)',lblPeakT:'最高温',lblManual:'手动控制',btnSaveZ:'保存区域 ',btnOTA:'OTA',btnWifi:'WiFi',btnRestart:'重启 ESP',btnUpdates:'检查更新',btnAllAuto:'AUTO',btnAllMan:'手动',btnOn:'开启',btnOff:'关闭',modeAuto:'AUTO',modeManual:'手动',fanOn:'开启',fanOff:'关闭',otaMsg:'正在打开 OTA 页面...',rstMsg:'现在重启 ESP 吗？',peakMsg:'清除已记录的最高温度吗？',btnResetPeak:'T.max',sensNone:'无传感器',sens1:'传感器',sensZ1:'区域 1',sensZ2:'区域 2',sensShared:'区域 1+2',sensAlertNone:'未检测到 DS18B20。请检查 GPIO4、3.3V、GND 和 4.7k 上拉电阻。',sensAlertMissing:'有 DS18B20 传感器没有有效读数。请检查接线、连接或传感器本体。',sensAlertZ1:'区域 1 没有有效读数。',sensAlertZ2:'区域 2 没有有效读数。',xlbl:'-3分钟',xnow:'现在',footer:'ESP32-C3 · Snapmaker U1 Fan Control 24V · {version} · 自动刷新 3 秒',upToDate:'已经是最新版本',updateAvail:'发现新版本：{latest}。当前固件为 {current}。要打开下载页面吗？',updateOpenRelease:'该 release 中未找到 .bin 文件，将打开 GitHub 页面。',updateError:'当前无法检查 GitHub。',saveError:'保存错误'}};
const LOCALE_MAP={es:'es-ES',en:'en-GB',fr:'fr-FR',it:'it-IT',pt:'pt-PT',de:'de-DE',zh:'zh-CN'};
let LG=TR[localStorage.getItem('lang')]?localStorage.getItem('lang'):'es';
let FW_VER='v0.0.0';
let FW_REPO='MrRegata/SnapFan';
let updatePrompted=false;
let updateDismissedVersion=localStorage.getItem('updateDismissedVersion')||'';
async function persistLang(lang){try{await fetch('/setlang?lang='+encodeURIComponent(lang));}catch(e){}}
function netText(key){return{ssid:'SSID',ip:'IP',signal:TR[LG].signal||'Signal',time:TR[LG].clock||'Time',noWifi:TR[LG].noWifi||'No WiFi',noTime:TR[LG].noTime||'No time',noSignal:(LG==='es'?'Sin señal':LG==='fr'?'Pas de signal':LG==='it'?'Nessun segnale':LG==='pt'?'Sem sinal':LG==='de'?'Kein Signal':'无信号')}[key];}
function fmt(tpl,vars){return tpl.replace(/\{(\w+)\}/g,(_,k)=>vars[k]??'');}
function normVer(v){return String(v||'').trim().replace(/^v/i,'');}
function cmpVer(a,b){const pa=normVer(a).split('.').map(v=>parseInt(v,10)||0);const pb=normVer(b).split('.').map(v=>parseInt(v,10)||0);const len=Math.max(pa.length,pb.length);for(let i=0;i<len;i++){const av=pa[i]||0,bv=pb[i]||0;if(av>bv)return 1;if(av<bv)return -1;}return 0;}
function extraText(key){const map={otaGo:{es:'Ir a OTA',en:'Go to OTA',fr:'Aller vers OTA',it:'Vai a OTA',pt:'Ir para OTA',de:'Zu OTA',zh:'前往 OTA'},cancel:{es:'Cancelar',en:'Cancel',fr:'Annuler',it:'Annulla',pt:'Cancelar',de:'Abbrechen',zh:'取消'},yes:{es:'Sí',en:'Yes',fr:'Oui',it:'Sì',pt:'Sim',de:'Ja',zh:'是'},no:{es:'No',en:'No',fr:'Non',it:'No',pt:'Não',de:'Nein',zh:'否'}};return (map[key]&&map[key][LG])||map[key].es;}
function setLang(l,persist=true){LG=TR[l]?l:'es';localStorage.setItem('lang',LG);const t=TR[LG];document.documentElement.lang=LG;document.getElementById('langSel').value=LG;document.getElementById('z1title').innerHTML=t.z1title;document.getElementById('z2title').innerHTML=t.z2title;document.getElementById('lblAllZones').textContent=t.lblAllZones;document.getElementById('btnAllAuto').textContent=t.btnAllAuto;document.getElementById('btnAllMan').textContent=t.btnAllMan;document.getElementById('btnUpdates').innerHTML='<span class="btn-ico">⬇️</span><span>'+t.btnUpdates+'</span>';document.getElementById('btnOTA').innerHTML='<span class="btn-ico">📦</span><span>'+t.btnOTA+'</span>';document.getElementById('btnWifi').innerHTML='<span class="btn-ico">📶</span><span>'+t.btnWifi+'</span>';document.getElementById('btnRestart').innerHTML='<span class="btn-ico">🔄</span><span>'+t.btnRestart+'</span>';document.getElementById('btnResetPeak').innerHTML='<span class="btn-ico">🌡️</span><span>'+t.btnResetPeak+'</span>';document.getElementById('lblNetSsid').textContent=netText('ssid');document.getElementById('lblNetIp').textContent=netText('ip');document.getElementById('lblNetSignal').textContent=netText('signal');document.getElementById('lblNetTime').textContent=netText('time');for(const z of['1','2']){document.getElementById('lblTemp'+z).textContent=t.lblTemp;document.getElementById('lblMode'+z).textContent=t.lblMode;document.getElementById('lblAuto'+z).textContent=t.lblAuto;document.getElementById('lblTHigh'+z).textContent=t.lblTHigh;document.getElementById('lblHyst'+z).textContent=t.lblHyst;document.getElementById('lblPeakT'+z).textContent=t.lblPeakT;document.getElementById('lblManual'+z).textContent=t.lblManual;document.getElementById('btnSaveZ'+z).textContent=t.btnSaveZ+z;document.getElementById('z'+z+'on').textContent=t.btnOn;document.getElementById('z'+z+'off').textContent=t.btnOff;document.getElementById('xlbl'+z).textContent=t.xlbl;document.getElementById('xnow'+z).textContent=t.xnow;}document.getElementById('otaMsg').textContent=t.otaMsg;document.getElementById('rstMsg').textContent=t.rstMsg;document.getElementById('peakMsg').textContent=t.peakMsg;document.getElementById('otaGoBtn').textContent=extraText('otaGo');document.getElementById('otaCancelBtn').textContent=extraText('cancel');document.getElementById('peakYesBtn').textContent=extraText('yes');document.getElementById('peakNoBtn').textContent=extraText('no');document.getElementById('rstYesBtn').textContent=extraText('yes');document.getElementById('rstNoBtn').textContent=extraText('no');document.getElementById('footer').textContent=fmt(t.footer,{version:FW_VER});document.getElementById('fwVersion').textContent=FW_VER;if(persist)persistLang(LG);}
function formatNetDatePart(epoch, kind){if(!epoch)return netText('noTime');const loc=LOCALE_MAP[LG]||'es-ES';const d=new Date(epoch*1000);if(kind==='day'){const text=d.toLocaleDateString(loc,{weekday:'long'});return text.charAt(0).toUpperCase()+text.slice(1);}if(kind==='date'){return d.toLocaleDateString(loc,{day:'2-digit',month:'2-digit',year:'numeric'});}return d.toLocaleTimeString(loc,{hour:'2-digit',minute:'2-digit',second:'2-digit'});}
function formatSignal(rssi){if(typeof rssi!=='number'||rssi<=-126)return netText('noSignal');const quality=Math.max(0,Math.min(100,Math.round(2*(rssi+100))));return rssi+' dBm · '+quality+'%';}
function setSensorAlert(d){const el=document.getElementById('sensorAlert');if(!el)return;const t=TR[LG];const sc=parseInt(d.sc)||0;const s1ok=!!d.s1ok;const s2ok=!!d.s2ok;let msg='';if(sc===0){msg=t.sensAlertNone;}else if(!s1ok||!s2ok){const missing=[];if(!s1ok)missing.push(t.sensAlertZ1);if(!s2ok)missing.push(t.sensAlertZ2);msg=t.sensAlertMissing+' '+missing.join(' ');}el.innerHTML=msg?'<strong>DS18B20:</strong> '+msg:'';el.classList.toggle('show',!!msg);}
function showOTA(){document.getElementById('otaOverlay').classList.add('show');}function hideOTA(){document.getElementById('otaOverlay').classList.remove('show');}function confirmRestart(){document.getElementById('rstOverlay').classList.add('show');}function hideRst(){document.getElementById('rstOverlay').classList.remove('show');}function confirmResetPeak(){document.getElementById('peakOverlay').classList.add('show');}function hidePeakRst(){document.getElementById('peakOverlay').classList.remove('show');}
async function doResetPeak(){hidePeakRst();try{await fetch('/resetpeaks');}catch(e){}maxTemp={'1':null,'2':null};document.getElementById('mx1').textContent='--.-\u00b0C';document.getElementById('mx2').textContent='--.-\u00b0C';}
async function doRestart(){hideRst();try{await fetch('/restart');}catch(e){}setTimeout(()=>location.reload(),6000);}function openWifiPage(){window.location='/wifi';}
async function checkForUpdates(manual){const t=TR[LG];try{const res=await fetch('https://api.github.com/repos/'+FW_REPO+'/releases/latest',{headers:{'Accept':'application/vnd.github+json'}});if(!res.ok)throw new Error('github');const rel=await res.json();const latest=rel.tag_name||rel.name||'';if(cmpVer(latest,FW_VER)<=0){if(manual)alert(t.upToDate+' ('+FW_VER+')');if(updateDismissedVersion&&cmpVer(updateDismissedVersion,latest)<=0){updateDismissedVersion='';localStorage.removeItem('updateDismissedVersion');}return;}if(!manual&&updateDismissedVersion&&normVer(updateDismissedVersion)===normVer(latest)){return;}const asset=(rel.assets||[]).find(a=>String(a.name||'').toLowerCase().endsWith('.bin'));const openUrl=asset&&asset.browser_download_url?asset.browser_download_url:rel.html_url;if(!asset&&manual)alert(t.updateOpenRelease);if(confirm(fmt(t.updateAvail,{latest:latest,current:FW_VER}))){window.open(openUrl,'_blank','noopener');updateDismissedVersion='';localStorage.removeItem('updateDismissedVersion');}else if(!manual){updateDismissedVersion=latest;localStorage.setItem('updateDismissedVersion',latest);}}catch(e){if(manual)alert(t.updateError);}}
let histData={t1:[],t2:[]};let maxTemp={'1':null,'2':null};
function drawChart(id,data,col){const cv=document.getElementById(id);if(!cv||!data||data.length<2)return;const W=cv.parentElement.clientWidth||260;const H=cv.parentElement.clientHeight||80;cv.width=W;cv.height=H;const ctx=cv.getContext('2d');ctx.clearRect(0,0,W,H);const N=data.length;const loV=Math.min(...data),hiV=Math.max(...data);const lo=Math.max(0,loV-5),hi=hiV+5,rng=hi-lo||1;const pH=12,pB=12;const toX=i=>(i/(N-1))*(W-2)+1;const toY=v=>H-pB-((v-lo)/rng)*(H-pH-pB);ctx.fillStyle='#0f172a';ctx.fillRect(0,0,W,H);for(let g=1;g<4;g++){const yg=H-pB-(g/3)*(H-pH-pB);ctx.strokeStyle='#1e3050';ctx.beginPath();ctx.moveTo(0,yg);ctx.lineTo(W,yg);ctx.stroke();ctx.fillStyle='#4b5563';ctx.font='8px monospace';ctx.fillText((lo+g/3*rng).toFixed(0)+'\u00b0',3,yg-2);}const grad=ctx.createLinearGradient(0,0,0,H);grad.addColorStop(0,col+'55');grad.addColorStop(1,col+'08');ctx.beginPath();data.forEach((v,i)=>{const y=toY(v);i===0?ctx.moveTo(toX(i),y):ctx.lineTo(toX(i),y);});ctx.lineTo(toX(N-1),H);ctx.lineTo(toX(0),H);ctx.closePath();ctx.fillStyle=grad;ctx.fill();ctx.beginPath();data.forEach((v,i)=>{const y=toY(v);i===0?ctx.moveTo(toX(i),y):ctx.lineTo(toX(i),y);});ctx.strokeStyle=col;ctx.lineWidth=2;ctx.lineJoin='round';ctx.stroke();const lx=toX(N-1),ly=toY(data[N-1]);ctx.beginPath();ctx.arc(lx,ly,3,0,Math.PI*2);ctx.fillStyle=col;ctx.fill();}
async function fetchHistory(){try{const d=await(await fetch('/history')).json();histData=d;[['1','#38bdf8'],['2','#fb923c']].forEach(([z,c])=>{const arr=d['t'+z]||[];drawChart('ch'+z,arr,c);if(arr.length>0){const m=Math.max(...arr);if(maxTemp[z]===null||m>maxTemp[z])maxTemp[z]=m;const el=document.getElementById('mx'+z);if(el)el.textContent=maxTemp[z].toFixed(1)+'\u00b0C';}});}catch(e){}}
window.addEventListener('resize',()=>{drawChart('ch1',histData.t1,'#38bdf8');drawChart('ch2',histData.t2,'#fb923c');});
const D={};document.querySelectorAll('input').forEach(i=>{i.addEventListener('mousedown',()=>{D[i.id]=true;});i.addEventListener('blur',()=>{delete D[i.id];});});function tc(t){return t>=55?'hot':t>=40?'warm':'cool';}function sf(id,v){if(!D[id]&&document.activeElement.id!==id)document.getElementById(id).value=v;}
function setFanState(z,on){const dot=document.getElementById('dot'+z),lbl=document.getElementById('fanLbl'+z);const t=TR[LG];if(on){dot.className='dot dot-on';lbl.className='fan-lbl fan-on-txt';lbl.textContent=t.fanOn;}else{dot.className='dot dot-off';lbl.className='fan-lbl fan-off-txt';lbl.textContent=t.fanOff;}}
function setZoneUI(z,autoMode,manualOn){const b=document.getElementById('zmdg'+z),btn=document.getElementById('zbtn'+z),t=TR[LG],onBtn=document.getElementById('z'+z+'on'),offBtn=document.getElementById('z'+z+'off');if(autoMode){b.className='mbadge mauto';b.textContent=t.modeAuto;btn.className='btnm btnm2 bsm';btn.textContent=t.btnAllMan;onBtn.disabled=true;offBtn.disabled=true;onBtn.style.opacity='0.28';offBtn.style.opacity='0.28';}else{b.className='mbadge mman';b.textContent=t.modeManual;btn.className='btnm btna bsm';btn.textContent=t.btnAllAuto;onBtn.disabled=manualOn;offBtn.disabled=!manualOn;onBtn.style.opacity=manualOn?'0.28':'1';offBtn.style.opacity=manualOn?'1':'0.28';}document.getElementById('mode'+z).textContent=autoMode?t.modeAuto:t.modeManual;}
async function toggleZoneMode(z){const cur=document.getElementById('zmdg'+z).textContent===TR[LG].modeAuto;await fetch('/setmode?zone='+z+'&mode='+(cur?'manual':'auto'));refresh();}
async function setAllMode(m){await fetch('/setmode?mode='+m);refresh();}
async function setManual(z,on){await fetch('/setmanual?zone='+z+'&on='+on);refresh();}
async function refresh(){try{const d=await(await fetch('/status')).json();if(d.lang&&TR[d.lang]&&d.lang!==LG){setLang(d.lang,false);}if(d.ver){FW_VER='v'+normVer(d.ver);if(updateDismissedVersion&&cmpVer(updateDismissedVersion,FW_VER)<=0){updateDismissedVersion='';localStorage.removeItem('updateDismissedVersion');}}if(d.repo){FW_REPO=d.repo;}document.getElementById('netSsid').textContent=d.ssid||netText('noWifi');document.getElementById('netIp').textContent=d.ip||'--';document.getElementById('netSignal').textContent=formatSignal(Number(d.rssi));document.getElementById('netTime').textContent=formatNetDatePart(parseInt(d.epoch)||0,'time');setSensorAlert(d);for(const z of['1','2']){const ok=!!d['s'+z+'ok'];const te=document.getElementById('t'+z);if(ok){const tv=parseFloat(d['t'+z]).toFixed(1);te.textContent=tv+'\u00b0C';te.className='sv '+tc(parseFloat(tv));}else{te.textContent='--.-\u00b0C';te.className='sv state';}setFanState(z,d['on'+z]);}sf('z1th',d.z1th);sf('z1hy',d.z1hy);sf('z2th',d.z2th);sf('z2hy',d.z2hy);setZoneUI('1',d.z1auto,d.z1man);setZoneUI('2',d.z2auto,d.z2man);const sc=parseInt(d.sc)||0;const t=TR[LG];document.getElementById('fwVersion').textContent=FW_VER;document.getElementById('footer').textContent=fmt(t.footer,{version:FW_VER});for(let z=1;z<=2;z++){const el=document.getElementById('sensInfo'+z);if(!el)continue;if(sc===0){el.textContent='🌡️ '+t.sensNone;}else if(sc===1){el.innerHTML='🌡️ '+t.sens1+' &bull; '+(z===1?t.sensZ1:t.sensShared);}else{el.innerHTML='🌡️ '+t.sens1+(z===2?' #2':' #1')+' &bull; '+(z===1?t.sensZ1:t.sensZ2);}}for(const z of['1','2']){const pk=parseFloat(d['mx'+z]);if(!isNaN(pk)&&pk>-100){if(maxTemp[z]===null||pk>maxTemp[z])maxTemp[z]=pk;const el=document.getElementById('mx'+z);if(el)el.textContent=maxTemp[z].toFixed(1)+'\u00b0C';}}if(!updatePrompted&&FW_REPO){updatePrompted=true;setTimeout(()=>checkForUpdates(false),1200);}}catch(e){}}
async function saveZone(e,z){e.preventDefault();const p=new URLSearchParams({zone:z,thigh:document.getElementById('z'+z+'th').value,hyst:document.getElementById('z'+z+'hy').value});const r=await fetch('/set?'+p);if(!r.ok){alert(TR[LG].saveError||'Error');}else{delete D['z'+z+'th'];delete D['z'+z+'hy'];refresh();}}
window.addEventListener('load',()=>{setLang(LG);fetchHistory();refresh();setInterval(refresh,3000);setInterval(fetchHistory,15000);});
</script></body></html>
)rawliteral";

// ─── Manejadores web ──────────────────────────────────────────────────────────
void handleRoot()   { server.send_P(200, "text/html", HTML_PAGE); }
void handleStatus() { server.send(200, F("application/json"), buildJson()); }
void handleWifiPage() { server.send_P(200, "text/html", WIFI_PAGE); }

void handleWifiScan() {
  const int n = WiFi.scanNetworks(false, true);
  String j;
  j.reserve(64 + (n > 0 ? n * 96 : 0));
  j += F("{\"nets\":[");
  bool first = true;
  for (int i = 0; i < n; ++i) {
    const String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;
    if (!first) j += ',';
    first = false;
    j += F("{\"ssid\":\""); j += jsonEscape(ssid); j += F("\"");
    j += F(",\"rssi\":"); j += String(WiFi.RSSI(i));
    j += F(",\"secure\":"); j += WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? F("false") : F("true");
    j += F(",\"channel\":"); j += String(WiFi.channel(i));
    j += F("}");
  }
  j += F("]}");
  WiFi.scanDelete();
  server.send(200, F("application/json"), j);
}

void handleHistory() {
    String j; j.reserve(700);
    j += F("{\"t1\":[");
    const int start = (histCount < HIST_SIZE) ? 0 : histIdx;
    for (int i = 0; i < histCount; i++) { if (i > 0) j += ','; j += String(hist1[(start + i) % HIST_SIZE], 1); }
    j += F("],\"t2\":[");
    for (int i = 0; i < histCount; i++) { if (i > 0) j += ','; j += String(hist2[(start + i) % HIST_SIZE], 1); }
    j += F("]}");
    server.send(200, F("application/json"), j);
}

void handleSet() {
  if (!server.hasArg("zone") || !server.hasArg("thigh") || !server.hasArg("hyst")) {
        server.send(400, "text/plain", "Parametros faltantes"); return;
    }
    const int   zone  = server.arg("zone").toInt();
    const float thigh = server.arg("thigh").toFloat();
    const float hyst  = server.arg("hyst").toFloat();
    if (zone < 1 || zone > 2)          { server.send(400, "text/plain", "Zona invalida"); return; }
    if (thigh < 20 || thigh > 90)       { server.send(400, "text/plain", "thigh[20-90]"); return; }
    if (hyst  < 1  || hyst  > 20)       { server.send(400, "text/plain", "hyst[1-20]"); return; }
  if (zone == 1) { z1THigh = thigh; z1Hyst = hyst; }
  else           { z2THigh = thigh; z2Hyst = hyst; }
    saveSettings();
    server.send(200, "text/plain", "OK");
}

void handleSetMode() {
    if (!server.hasArg("mode")) { server.send(400, "text/plain", "Falta mode"); return; }
    const String mode = server.arg("mode");
    if (mode != "auto" && mode != "manual") { server.send(400, "text/plain", "Modo invalido"); return; }
    const bool a = (mode == "auto");
    const int zone = server.hasArg("zone") ? server.arg("zone").toInt() : 0;
    if (zone == 1) z1Auto = a; else if (zone == 2) z2Auto = a; else z1Auto = z2Auto = a;
    saveSettings();
    server.send(200, "text/plain", "OK");
}

void handleSetLang() {
    if (!server.hasArg("lang")) {
      server.send(400, "text/plain", "Falta lang"); return;
    }
    const String lang = server.arg("lang");
    if (lang.length() != 2 || !isSupportedLanguageCode(lang.c_str())) {
      server.send(400, "text/plain", "Idioma invalido"); return;
    }
    saveLanguagePref(lang.c_str());
    server.send(200, "text/plain", "OK");
}

  void handleSetManual() {
    if (!server.hasArg("zone") || !server.hasArg("on")) {
      server.send(400, "text/plain", "Faltan parametros"); return;
    }
    const int zone = server.arg("zone").toInt();
    const int on = server.arg("on").toInt();
    if (zone < 1 || zone > 2 || (on != 0 && on != 1)) {
      server.send(400, "text/plain", "Parametros invalidos"); return;
    }
    if (zone == 1) z1ManualOn = (on == 1);
    else           z2ManualOn = (on == 1);
    saveSettings();
    controlFans();
    server.send(200, "text/plain", "OK");
  }

void handleOtaPage() { server.send_P(200, "text/html", OTA_PAGE); }

void handleSaveWifi() {
    if (!server.hasArg("ssid") || server.arg("ssid").length() == 0) {
        server.send(400, "text/plain", "SSID requerido"); return;
    }
    const String ssid = server.arg("ssid");
    const String pass = server.hasArg("pass") ? server.arg("pass") : "";
    // noreset=1 → llamado desde web principal; guardar sin mensaje de recarga AP
    (void)(server.hasArg("noreset") && server.arg("noreset") == "1"); // noreset=1: reservado
    saveWifiCreds(ssid.c_str(), pass.c_str());
    server.send(200, "text/html",
        F("<!DOCTYPE html><html><head><meta charset='UTF-8'>"
          "<style>body{font-family:sans-serif;background:#111827;color:#e2e8f0;text-align:center;padding:40px}</style></head>"
          "<body><h2 style='color:#60a5fa'>&#128274; Guardado</h2>"
          "<p style='margin-top:12px'>Reiniciando y conectando a la nueva red...</p>"
          "</body></html>"));
    delay(1500);
    ESP.restart();
}

void handleWifiReset() {
    uint32_t zero = 0;
    EEPROM.put(ADDR_WIFI_MAGIC, zero);
    EEPROM.commit();
    server.send(200, "text/plain", "WiFi borrado. Reiniciando en modo AP...");
    delay(1000);
    ESP.restart();
}

void handleResetPeaks() {
    peakTemp1 = -127.0f;
    peakTemp2 = -127.0f;
    savePeakTemps();
    server.send(200, "text/plain", "OK");
}

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(100);
    Serial.println(F("\n=== Snapmaker U1 Fan Controller 24V · ESP32-C3 ==="));
  Serial.printf("Firmware version: v%s\n", FW_VERSION);
    Serial.print(F("MAC ESP32: "));
    Serial.println(WiFi.macAddress());

    // Hardware
    pinMode(MOSFET1_PIN, OUTPUT); digitalWrite(MOSFET1_PIN, LOW);
    pinMode(MOSFET2_PIN, OUTPUT); digitalWrite(MOSFET2_PIN, LOW);
    pinMode(LED_PIN, OUTPUT); digitalWrite(LED_PIN, HIGH);

    // EEPROM
    EEPROM.begin(EEPROM_SIZE);
    loadLanguagePref();
    loadSettings();
    loadPeakTemps();
    Serial.printf("Z1: %s  Z2: %s\n", z1Auto ? "AUTO" : "MANUAL", z2Auto ? "AUTO" : "MANUAL");

    // DS18B20
    sensors.begin();
    const int found = sensors.getDeviceCount();
    Serial.printf("DS18B20: %d sensor(es)\n", found);
    sensor1Valid = (found >= 1);
    sensor2Valid = (found >= 1);
    if (found >= 1) { sensors.getAddress(addr1, 0); sensors.setResolution(addr1, 11); }
    if (found >= 2) { sensors.getAddress(addr2, 1); sensors.setResolution(addr2, 11); twoSensors = true; sensor2Valid = true; }
    else Serial.println(F("AVISO: zona 2 usara la misma lectura que zona 1"));

    // Credenciales WiFi
    char wifiSSID[33] = {0}, wifiPass[65] = {0};
    const bool hasWifi = loadWifiCreds(wifiSSID, wifiPass);

    if (!hasWifi) {
      // ── MODO AP ──────────────────────────────────────────────────────────
      apMode = true;
      Serial.println(F("Sin credenciales WiFi → modo AP 'SnapFan-Setup'"));
      WiFi.mode(WIFI_AP_STA);
      WiFi.softAP("SnapFan-Setup");
      delay(1000); // Esperar a que la IP esté lista
      Serial.print(F("IP AP: ")); Serial.println(WiFi.softAPIP());
      dnsServer.start(53, "*", WiFi.softAPIP());
      server.on("/", HTTP_GET,  []() { server.send_P(200, "text/html", AP_PAGE); });
      server.on("/wifi", HTTP_GET, handleWifiPage);
      server.on("/wifi/scan", HTTP_GET, handleWifiScan);
      server.on("/status", HTTP_GET, handleStatus);
      server.on("/setlang", HTTP_GET, handleSetLang);
      server.on("/savewifi", HTTP_POST, handleSaveWifi);
      server.onNotFound([]() {
        server.sendHeader("Location", "http://192.168.4.1/", true);
        server.send(302, "text/plain", "");
      });
      server.begin();
      Serial.println(F("Portal AP listo → http://192.168.4.1"));
    } else {
      // ── MODO NORMAL ──────────────────────────────────────────────────────
      Serial.printf("Conectando a '%s'\n", wifiSSID);
      WiFi.mode(WIFI_STA);
      WiFi.setHostname(DEVICE_HOSTNAME);
      WiFi.begin(wifiSSID, wifiPass);
      const unsigned long wStart = millis();
      while (WiFi.status() != WL_CONNECTED && (millis() - wStart) < 15000UL) {
        delay(500); Serial.print('.');
      }
      Serial.println();
      if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("Conectado! IP: %s\n", WiFi.localIP().toString().c_str());
      } else {
        Serial.println(F("WiFi no disponible. Reintentando en loop..."));
      }
      server.on("/",         HTTP_GET, handleRoot);
      server.on("/wifi",     HTTP_GET, handleWifiPage);
      server.on("/wifi/scan", HTTP_GET, handleWifiScan);
      server.on("/status",   HTTP_GET, handleStatus);
      server.on("/setlang",  HTTP_GET, handleSetLang);
      server.on("/history",  HTTP_GET, handleHistory);
      server.on("/set",      HTTP_GET, handleSet);
      server.on("/setmode",  HTTP_GET, handleSetMode);
      server.on("/setmanual", HTTP_GET, handleSetManual);
      server.on("/wifireset",  HTTP_GET, handleWifiReset);
      server.on("/resetpeaks",  HTTP_GET, handleResetPeaks);
      server.on("/savewifi", HTTP_POST, handleSaveWifi);
      server.on("/restart",  HTTP_GET, []() {
        server.send(200, "text/plain", "Reiniciando..."); delay(200); ESP.restart();
      });
      // GET /update → página visual; POST /update → esptool handler
      server.on("/update",   HTTP_GET,  handleOtaPage);
      httpUpdater.setup(&server, "/update");
      server.begin();
      Serial.println(F("Servidor web listo"));
      if (MQTT_BROKER[0] != '\0') {
        mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
        mqttClient.setCallback(mqttCallback);
        mqttReconnect();
      } else Serial.println(F("MQTT desactivado"));
    }
}

  void updateTemperatures() {
    if (millis() - lastTempMs < TEMP_MS) return;
    lastTempMs = millis();

    sensors.requestTemperatures();
    const float r1 = twoSensors ? sensors.getTempC(addr1) : sensors.getTempCByIndex(0);
    sensor1Valid = (r1 != DEVICE_DISCONNECTED_C) && (sensor1Ready || !isBootPlaceholderReading(r1));
    if (sensor1Valid) {
      temp1 = r1;
      sensor1Ready = true;
    }

    const float r2 = twoSensors ? sensors.getTempC(addr2) : temp1;
    sensor2Valid = twoSensors
      ? ((r2 != DEVICE_DISCONNECTED_C) && (sensor2Ready || !isBootPlaceholderReading(r2)))
      : sensor1Valid;
    if (sensor2Valid) {
      temp2 = r2;
      sensor2Ready = twoSensors ? true : sensor1Ready;
    }

    hist1[histIdx] = temp1;
    hist2[histIdx] = temp2;
    histIdx = (histIdx + 1) % HIST_SIZE;
    if (histCount < HIST_SIZE) histCount++;

    bool pkChanged = false;
    if (temp1 > peakTemp1) { peakTemp1 = temp1; pkChanged = true; }
    if (temp2 > peakTemp2) { peakTemp2 = temp2; pkChanged = true; }
    if (pkChanged) savePeakTemps();

    Serial.printf("Temperaturas -> Z1: %.2f C | Z2: %.2f C\n", temp1, temp2);
    controlFans();
  }

// ─── Loop ─────────────────────────────────────────────────────────────────────
void loop() {
  updateTemperatures();

    if (apMode) {
        dnsServer.processNextRequest();
        server.handleClient();
        // Parpadeo rápido LED → indica modo AP
        static unsigned long lb = 0;
        if (millis() - lb > 200) { lb = millis(); digitalWrite(LED_PIN, !digitalRead(LED_PIN)); }
        return;
    }

    // Watchdog WiFi
    if (WiFi.status() != WL_CONNECTED) {
        static unsigned long lastW = 0;
        if (millis() - lastW >= 10000UL) {
            lastW = millis();
            char ssid[33] = {0}, pass[65] = {0};
            loadWifiCreds(ssid, pass);
            WiFi.disconnect(); WiFi.begin(ssid, pass);
        }
        return;
    }

    static bool announced = false;
    if (!announced) {
        announced = true;
        Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
    }

    server.handleClient();

    if (MQTT_BROKER[0] != '\0') {
        if (!mqttClient.connected()) mqttReconnect();
        else {
            mqttClient.loop();
            static unsigned long lastPub = 0;
            if (millis() - lastPub >= 10000UL) {
                lastPub = millis();
                mqttClient.publish(TOPIC_STATUS, buildJson().c_str());
            }
        }
    }
}
