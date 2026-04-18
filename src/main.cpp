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

#ifndef APP_VERSION
#define APP_VERSION "0.0.0"
#endif

#ifndef APP_GITHUB_REPO
#define APP_GITHUB_REPO "MrRegata/SnapFan"
#endif

const char* FW_VERSION = APP_VERSION;
const char* FW_GITHUB_REPO = APP_GITHUB_REPO;

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

// ─── JSON de estado ───────────────────────────────────────────────────────────
String buildJson() {
  String j; j.reserve(448);
    j += F("{\"t1\":"); j += String(temp1, 1);
    j += F(",\"t2\":"); j += String(temp2, 1);
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
    j += F(",\"sc\":"); j += sensors.getDeviceCount();
      j += F(",\"ver\":\""); j += FW_VERSION;
      j += F("\",\"repo\":\""); j += FW_GITHUB_REPO;
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
    <label>Red WiFi (SSID)</label>
    <input type="text" name="ssid" maxlength="32" required autocomplete="off" placeholder="Nombre de tu red WiFi">
    <label>Contrase&ntilde;a</label>
    <input type="password" name="pass" maxlength="64" autocomplete="off" placeholder="Dejar vac&iacute;o si la red es abierta">
    <button type="submit">&#128274; Guardar y Conectar</button>
  </form>
  <div class="note">El ESP se reiniciar&aacute; e intentar&aacute; conectar a tu red.<br>Si falla, volver&aacute; a modo AP autom&aacute;ticamente.</div>
</div></body></html>
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
  <a href="/" class="back-btn">&#8592; Volver</a>
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
    <span class="htitle">SnapFan &ndash; OTA Update</span>
  </div>
  <div style="width:80px"></div>
</div>
<div class="card">
  <h2>&#128230; Actualizaci&oacute;n de Firmware</h2>
  <div class="dropzone" id="dz" onclick="document.getElementById('fw').click()">
    <svg width="40" height="40" viewBox="0 0 24 24" fill="none" stroke="#60a5fa" stroke-width="1.5">
      <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/>
      <polyline points="17 8 12 3 7 8"/><line x1="12" y1="3" x2="12" y2="15"/>
    </svg>
    <p>Arrastra el archivo <span>.bin</span> aqu&iacute;<br>o haz clic para seleccionar</p>
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
    Subir Firmware
  </button>
  <div class="result" id="res"></div>
  <div class="note">&#9888;&#65039; El ESP se reiniciar&aacute; autom&aacute;ticamente al terminar.<br>No cierres esta p&aacute;gina hasta que finalice la carga.</div>
</div>
<script>
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
    if(xhr.status==200){res.className='result ok';res.textContent='✓ Firmware cargado. Reiniciando...';}
    else{res.className='result err';res.textContent='✗ Error: '+xhr.responseText;}
  };
  xhr.onerror=()=>{res.style.display='block';res.className='result err';res.textContent='✗ Error de conexi\u00f3n';};
  upbtn.disabled=true; xhr.send(fd);
}
</script>
</body></html>
)rawliteral";

// ─── Página web principal ─────────────────────────────────────────────────────
const char HTML_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="es"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1.0">
<title>SnapFan - Snapmaker U1</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'Segoe UI',sans-serif;background:radial-gradient(circle at top,#1e3a5f 0,#111827 35%,#0b1220 100%);color:#e2e8f0;min-height:100vh;padding:12px 16px}
.topbar{display:flex;align-items:center;justify-content:space-between;margin-bottom:14px;flex-wrap:wrap;gap:8px}.brand{display:flex;align-items:center;gap:10px}.brand svg{width:38px;height:38px;flex-shrink:0}.brand-txt h1{font-size:1.05rem;font-weight:700;color:#60a5fa;line-height:1.1}.brand-txt p{font-size:.68rem;color:#64748b}.topright{display:flex;align-items:center;gap:6px;flex-wrap:wrap;justify-content:flex-end}
.langsel,.tbtn,input{border-radius:8px}.langsel{padding:5px 8px;border:1px solid #2d3a52;background:#1e2535;color:#e2e8f0;font-size:.78rem;cursor:pointer;outline:none}.tbtn{padding:6px 12px;border:1px solid #2d3a52;background:#1e2535;font-size:.78rem;cursor:pointer;font-weight:600;text-decoration:none;display:inline-flex;align-items:center;gap:5px;color:#e2e8f0}.tbtn:hover{background:#273550}.tbtn.blue{color:#60a5fa;border-color:#1d4ed8}.tbtn.orange{color:#fb923c}.tbtn.red{color:#f87171}.tbtn.green{color:#4ade80;border-color:#166534}.btn-ico{font-size:.95rem;line-height:1}.creator{margin-top:6px;text-align:center}.creator a,.social-links a{color:#64748b;font-size:.68rem;text-decoration:none}.creator a:hover,.social-links a:hover{color:#60a5fa}.creator strong{color:#cbd5e1}.social-links{margin-top:4px;text-align:center;display:flex;justify-content:center;gap:14px;flex-wrap:wrap}
.mbar{display:flex;align-items:center;justify-content:center;gap:8px;margin-bottom:14px;flex-wrap:wrap;padding:10px;background:#1e2535;border-radius:12px;border:1px solid #2d3a52}.mbadge{padding:4px 12px;border-radius:20px;font-weight:700;font-size:.8rem}.mauto{background:#065f46;color:#6ee7b7}.mman{background:#7c2d12;color:#fca5a5}.btnm{padding:6px 14px;border:none;border-radius:8px;font-size:.82rem;cursor:pointer;font-weight:600}.btna{background:#3b82f6;color:#fff}.btnm2{background:#dc2626;color:#fff}.bsm{padding:4px 10px!important;font-size:.76rem!important}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(290px,1fr));gap:14px;max-width:860px;margin:0 auto}.card{background:#1e2535;border-radius:14px;padding:18px;border:1px solid #2d3a52}.card h2{color:#93c5fd;font-size:.8rem;text-transform:uppercase;letter-spacing:2px;margin-bottom:10px;text-align:center}.zmode-bar{display:flex;align-items:center;justify-content:center;gap:8px;margin-bottom:10px}
.stats{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin:10px 0}.stat{text-align:center;background:#0f172a;border:1px solid #263349;border-radius:10px;padding:10px 8px}.sv{font-size:1.8rem;font-weight:700}.sl{font-size:.68rem;color:#94a3b8;margin-top:2px}.cool{color:#34d399}.warm{color:#fbbf24}.hot{color:#f87171}.state{color:#60a5fa}
.fan-state{display:flex;align-items:center;justify-content:center;gap:8px;margin-bottom:12px}.dot{width:11px;height:11px;border-radius:50%;flex-shrink:0;transition:all .5s}.dot-on{background:#22c55e;box-shadow:0 0 8px #22c55e88}.dot-off{background:#374151}.fan-lbl{font-size:.78rem;font-weight:700;letter-spacing:1px}.fan-on-txt{color:#22c55e}.fan-off-txt{color:#6b7280}
.chart-wrap{position:relative;background:#0f172a;border-radius:8px;margin-bottom:12px;overflow:hidden;height:80px}canvas.chart{position:absolute;top:0;left:0;width:100%;height:100%}.chart-lbl{position:absolute;bottom:2px;left:6px;font-size:7px;color:#475569;pointer-events:none}.chart-lbl-r{position:absolute;bottom:2px;right:6px;font-size:7px;color:#475569;pointer-events:none}
.sec{border-top:1px solid #2d3a52;padding-top:12px;margin-top:2px}.st{font-size:.7rem;color:#64748b;text-transform:uppercase;letter-spacing:1px;margin-bottom:8px}.r2{display:grid;grid-template-columns:1fr 1fr;gap:8px}label{font-size:.74rem;color:#94a3b8;display:block;margin-bottom:2px}input[type=number],input[type=text],input[type=password]{width:100%;padding:7px 8px;border:1px solid #2d3a52;background:#0f172a;color:#e2e8f0;font-size:.88rem;outline:none}input:focus{border-color:#3b82f6}
.sv2{width:100%;margin-top:12px;padding:8px;background:#1d4ed8;color:#fff;border:none;border-radius:8px;font-size:.88rem;cursor:pointer}.sv2:hover{background:#2563eb}.power-actions{display:grid;grid-template-columns:1fr 1fr;gap:8px}.power-btn{padding:10px;border:none;border-radius:8px;font-size:.82rem;font-weight:700;cursor:pointer;transition:opacity .2s ease,filter .2s ease}.power-btn:disabled{opacity:.28;filter:saturate(.35) brightness(.7);cursor:not-allowed}.power-on{background:#166534;color:#dcfce7}.power-off{background:#7f1d1d;color:#fee2e2}
.wifi-panel{display:none;max-width:860px;margin:14px auto 0}.wifi-card{background:#1e2535;border-radius:14px;padding:18px;border:1px solid #2d3a52}.wifi-card h2{color:#93c5fd;font-size:.8rem;text-transform:uppercase;letter-spacing:2px;margin-bottom:12px;text-align:center}.wifi-r2{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-bottom:8px}
.overlay{display:none;position:fixed;inset:0;background:#000a;z-index:100;align-items:center;justify-content:center}.overlay.show{display:flex}.modal{background:#1e2535;border-radius:14px;padding:24px;width:90%;max-width:340px;border:1px solid #2d3a52;text-align:center}.modal p{margin-bottom:16px;font-size:.9rem;color:#94a3b8}.modal-btns{display:flex;gap:10px;justify-content:center}.modal-btns button{padding:8px 20px;border:none;border-radius:8px;font-size:.88rem;cursor:pointer;font-weight:600}
footer{text-align:center;color:#475569;font-size:.66rem;margin-top:16px}
</style></head><body>
<div class="topbar">
  <div class="brand">
    <svg viewBox="0 0 64 64" fill="none" xmlns="http://www.w3.org/2000/svg"><rect x="6" y="46" width="52" height="13" rx="3" fill="#1e3a5f" stroke="#60a5fa" stroke-width="2"/><rect x="10" y="38" width="44" height="10" rx="2" fill="#0f2a47" stroke="#3b82f6" stroke-width="1.5"/><rect x="16" y="30" width="32" height="10" rx="2" fill="#0f2a47" stroke="#3b82f6" stroke-width="1.5"/><rect x="22" y="22" width="20" height="10" rx="2" fill="#0f2a47" stroke="#60a5fa" stroke-width="1.5"/><line x1="26" y1="22" x2="26" y2="6" stroke="#60a5fa" stroke-width="2" stroke-linecap="round"/><line x1="32" y1="22" x2="32" y2="6" stroke="#60a5fa" stroke-width="2" stroke-linecap="round"/><line x1="38" y1="22" x2="38" y2="6" stroke="#60a5fa" stroke-width="2" stroke-linecap="round"/></svg>
    <div class="brand-txt"><h1>Snapmaker U1</h1><p>Fan Control 24V · ESP32-C3</p><div style="font-size:.68rem;color:#94a3b8;margin-top:2px">Firmware <span id="fwVersion">v0.0.0</span></div></div>
  </div>
  <div class="topright">
    <select class="langsel" id="langSel" onchange="setLang(this.value)"><option value="es">🇪🇸 ES</option><option value="en">🇬🇧 EN</option></select>
    <button class="tbtn green" onclick="checkForUpdates(true)" id="btnUpdates"><span class="btn-ico">⬇️</span><span>Updates</span></button>
    <button class="tbtn blue" onclick="showOTA()" id="btnOTA"><span class="btn-ico">📦</span><span>OTA</span></button>
    <button class="tbtn orange" onclick="toggleWifiPanel()" id="btnWifi"><span class="btn-ico">📶</span><span>WiFi</span></button>
    <button class="tbtn green" onclick="confirmResetPeak()" id="btnResetPeak"><span class="btn-ico">🌡️</span><span>T.max</span></button>
    <button class="tbtn red" onclick="confirmRestart()" id="btnRestart"><span class="btn-ico">🔄</span><span>Reiniciar ESP</span></button>
  </div>
</div>
<div class="mbar"><span style="font-size:.74rem;color:#94a3b8" id="lblAllZones">Todas las zonas:</span><button class="btnm btna bsm" id="btnAllAuto" onclick="setAllMode('auto')">AUTO</button><button class="btnm btnm2 bsm" id="btnAllMan" onclick="setAllMode('manual')">MANUAL</button></div>
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
<div class="wifi-panel" id="wifiPanel"><div class="wifi-card"><h2 id="wifiTitle">Cambiar WiFi</h2><div class="wifi-r2"><div><label id="lblNewSSID">SSID</label><input type="text" id="newSSID" maxlength="32" autocomplete="off"></div><div><label id="lblNewPass">Contrase&ntilde;a</label><input type="password" id="newPass" maxlength="64" autocomplete="off"></div></div><button class="sv2" onclick="saveWifi()" id="btnSaveWifi">Guardar y Reconectar</button><p style="font-size:.7rem;color:#475569;margin-top:8px;text-align:center" id="wifiNote">El ESP se reconectar&aacute; a la nueva red y reiniciar&aacute;.</p></div></div>
<div class="overlay" id="otaOverlay"><div class="modal"><p id="otaMsg">Abriendo p&aacute;gina OTA...</p><div class="modal-btns"><button style="background:#1d4ed8;color:#fff" onclick="window.location='/update';hideOTA()">Ir a OTA</button><button style="background:#374151;color:#e2e8f0" onclick="hideOTA()">Cancelar</button></div></div></div>
<div class="overlay" id="peakOverlay"><div class="modal"><p id="peakMsg">&iquest;Borrar T. m&aacute;x registradas?</p><div class="modal-btns"><button style="background:#16a34a;color:#fff" onclick="doResetPeak()">S&iacute;</button><button style="background:#374151;color:#e2e8f0" onclick="hidePeakRst()">No</button></div></div></div>
<div class="overlay" id="rstOverlay"><div class="modal"><p id="rstMsg">&iquest;Reiniciar el ESP ahora?</p><div class="modal-btns"><button style="background:#dc2626;color:#fff" onclick="doRestart()">S&iacute;</button><button style="background:#374151;color:#e2e8f0" onclick="hideRst()">No</button></div></div></div>
<footer id="footer">ESP32-C3 · Snapmaker U1 Fan Control 24V · v0.0.0 · Auto-refresh 3 s</footer>
<div class="creator"><a href="#" onclick="return false;">Creado por <strong>Regata</strong></a></div>
<div class="social-links"><a href="https://t.me/regata3dprint" target="_blank" rel="noopener noreferrer">📨 Telegram @regata3dprint</a><a href="https://instagram.com/regata3dprint" target="_blank" rel="noopener noreferrer">📷 Instagram @regata3dprint</a></div>
<script>
const TR={es:{z1title:'&#9881; Drivers de Motores',z2title:'&#9889; Fuente de Alimentaci\u00f3n',lblAllZones:'Todas las zonas:',lblTemp:'Temperatura',lblMode:'Modo',lblAuto:'Control autom\u00e1tico',lblTHigh:'Umbral ON (\u00b0C)',lblHyst:'Hist\u00e9resis (\u00b0C)',lblPeakT:'T. m\u00e1x',lblManual:'Control manual',btnSaveZ:'Guardar Zona ',btnOTA:'OTA',btnWifi:'WiFi',btnRestart:'Reiniciar ESP',btnUpdates:'Buscar actualizaciones',btnAllAuto:'AUTO',btnAllMan:'MANUAL',btnOn:'Encender',btnOff:'Apagar',modeAuto:'AUTO',modeManual:'MANUAL',fanOn:'ENCENDIDO',fanOff:'APAGADO',wifiTitle:'Cambiar Red WiFi',lblNewSSID:'SSID',lblNewPass:'Contrase\u00f1a',btnSaveWifi:'Guardar y Reconectar',wifiNote:'El ESP se reconectar\u00e1 a la nueva red y reiniciar\u00e1.',otaMsg:'Abriendo p\u00e1gina OTA...',rstMsg:'\u00bfReiniciar el ESP ahora?',peakMsg:'\u00bfBorrar T. m\u00e1x registradas?',btnResetPeak:'T.max',sensNone:'Sin sensor',sens1:'Sensor',sensZ1:'Zona 1',sensZ2:'Zona 2',sensShared:'Zonas 1+2',xlbl:'-3min',xnow:'ahora',footer:'ESP32-C3 · Snapmaker U1 Fan Control 24V · {version} · Auto-refresh 3 s',upToDate:'Ya est\u00e1s en la \u00faltima versi\u00f3n',updateAvail:'Hay una nueva versi\u00f3n disponible: {latest}. Tu firmware actual es {current}. \u00bfQuieres abrir la descarga?',updateOpenRelease:'No se encontr\u00f3 un .bin en la release. Se abrir\u00e1 la p\u00e1gina de GitHub.',updateError:'No se pudo comprobar GitHub ahora mismo.'},en:{z1title:'&#9881; Motor Drivers',z2title:'&#9889; Power Supply',lblAllZones:'All zones:',lblTemp:'Temperature',lblMode:'Mode',lblAuto:'Automatic control',lblTHigh:'ON threshold (\u00b0C)',lblHyst:'Hysteresis (\u00b0C)',lblPeakT:'Peak T',lblManual:'Manual control',btnSaveZ:'Save Zone ',btnOTA:'OTA',btnWifi:'WiFi',btnRestart:'Restart ESP',btnUpdates:'Check updates',btnAllAuto:'AUTO',btnAllMan:'MANUAL',btnOn:'Turn on',btnOff:'Turn off',modeAuto:'AUTO',modeManual:'MANUAL',fanOn:'ON',fanOff:'OFF',wifiTitle:'Change WiFi Network',lblNewSSID:'SSID',lblNewPass:'Password',btnSaveWifi:'Save & Reconnect',wifiNote:'The ESP will reconnect to the new network and restart.',otaMsg:'Opening OTA page...',rstMsg:'Restart the ESP now?',peakMsg:'Clear recorded peak temperatures?',btnResetPeak:'Peak T',sensNone:'No sensor',sens1:'Sensor',sensZ1:'Zone 1',sensZ2:'Zone 2',sensShared:'Zones 1+2',xlbl:'-3min',xnow:'now',footer:'ESP32-C3 · Snapmaker U1 Fan Control 24V · {version} · Auto-refresh 3 s',upToDate:'You already have the latest version',updateAvail:'A new version is available: {latest}. Your current firmware is {current}. Open the download?',updateOpenRelease:'No .bin asset was found in the release. Opening GitHub release page.',updateError:'GitHub could not be checked right now.'}};
let LG=localStorage.getItem('lang')||'es';
let FW_VER='v0.0.0';
let FW_REPO='MrRegata/SnapFan';
let updatePrompted=false;
let updateDismissedVersion=localStorage.getItem('updateDismissedVersion')||'';
function fmt(tpl,vars){return tpl.replace(/\{(\w+)\}/g,(_,k)=>vars[k]??'');}
function normVer(v){return String(v||'').trim().replace(/^v/i,'');}
function cmpVer(a,b){const pa=normVer(a).split('.').map(v=>parseInt(v,10)||0);const pb=normVer(b).split('.').map(v=>parseInt(v,10)||0);const len=Math.max(pa.length,pb.length);for(let i=0;i<len;i++){const av=pa[i]||0,bv=pb[i]||0;if(av>bv)return 1;if(av<bv)return -1;}return 0;}
function setLang(l){LG=l;localStorage.setItem('lang',l);const t=TR[l];document.getElementById('langSel').value=l;document.getElementById('z1title').innerHTML=t.z1title;document.getElementById('z2title').innerHTML=t.z2title;document.getElementById('lblAllZones').textContent=t.lblAllZones;document.getElementById('btnAllAuto').textContent=t.btnAllAuto;document.getElementById('btnAllMan').textContent=t.btnAllMan;document.getElementById('btnUpdates').innerHTML='<span class="btn-ico">⬇️</span><span>'+t.btnUpdates+'</span>';document.getElementById('btnOTA').innerHTML='<span class="btn-ico">📦</span><span>'+t.btnOTA+'</span>';document.getElementById('btnWifi').innerHTML='<span class="btn-ico">📶</span><span>'+t.btnWifi+'</span>';document.getElementById('btnRestart').innerHTML='<span class="btn-ico">🔄</span><span>'+t.btnRestart+'</span>';document.getElementById('btnResetPeak').innerHTML='<span class="btn-ico">🌡️</span><span>'+t.btnResetPeak+'</span>';for(const z of['1','2']){document.getElementById('lblTemp'+z).textContent=t.lblTemp;document.getElementById('lblMode'+z).textContent=t.lblMode;document.getElementById('lblAuto'+z).textContent=t.lblAuto;document.getElementById('lblTHigh'+z).textContent=t.lblTHigh;document.getElementById('lblHyst'+z).textContent=t.lblHyst;document.getElementById('lblPeakT'+z).textContent=t.lblPeakT;document.getElementById('lblManual'+z).textContent=t.lblManual;document.getElementById('btnSaveZ'+z).textContent=t.btnSaveZ+z;document.getElementById('z'+z+'on').textContent=t.btnOn;document.getElementById('z'+z+'off').textContent=t.btnOff;document.getElementById('xlbl'+z).textContent=t.xlbl;document.getElementById('xnow'+z).textContent=t.xnow;}document.getElementById('wifiTitle').textContent=t.wifiTitle;document.getElementById('lblNewSSID').textContent=t.lblNewSSID;document.getElementById('lblNewPass').textContent=t.lblNewPass;document.getElementById('btnSaveWifi').textContent=t.btnSaveWifi;document.getElementById('wifiNote').textContent=t.wifiNote;document.getElementById('otaMsg').textContent=t.otaMsg;document.getElementById('rstMsg').textContent=t.rstMsg;document.getElementById('peakMsg').textContent=t.peakMsg;document.getElementById('footer').textContent=fmt(t.footer,{version:FW_VER});document.getElementById('fwVersion').textContent=FW_VER;}
function showOTA(){document.getElementById('otaOverlay').classList.add('show');}function hideOTA(){document.getElementById('otaOverlay').classList.remove('show');}function confirmRestart(){document.getElementById('rstOverlay').classList.add('show');}function hideRst(){document.getElementById('rstOverlay').classList.remove('show');}function confirmResetPeak(){document.getElementById('peakOverlay').classList.add('show');}function hidePeakRst(){document.getElementById('peakOverlay').classList.remove('show');}
async function doResetPeak(){hidePeakRst();try{await fetch('/resetpeaks');}catch(e){}maxTemp={'1':null,'2':null};document.getElementById('mx1').textContent='--.-\u00b0C';document.getElementById('mx2').textContent='--.-\u00b0C';}
async function doRestart(){hideRst();try{await fetch('/restart');}catch(e){}setTimeout(()=>location.reload(),6000);}function toggleWifiPanel(){const p=document.getElementById('wifiPanel');p.style.display=p.style.display==='block'?'none':'block';}
async function checkForUpdates(manual){const t=TR[LG];try{const res=await fetch('https://api.github.com/repos/'+FW_REPO+'/releases/latest',{headers:{'Accept':'application/vnd.github+json'}});if(!res.ok)throw new Error('github');const rel=await res.json();const latest=rel.tag_name||rel.name||'';if(cmpVer(latest,FW_VER)<=0){if(manual)alert(t.upToDate+' ('+FW_VER+')');if(updateDismissedVersion&&cmpVer(updateDismissedVersion,latest)<=0){updateDismissedVersion='';localStorage.removeItem('updateDismissedVersion');}return;}if(!manual&&updateDismissedVersion&&normVer(updateDismissedVersion)===normVer(latest)){return;}const asset=(rel.assets||[]).find(a=>String(a.name||'').toLowerCase().endsWith('.bin'));const openUrl=asset&&asset.browser_download_url?asset.browser_download_url:rel.html_url;if(!asset&&manual)alert(t.updateOpenRelease);if(confirm(fmt(t.updateAvail,{latest:latest,current:FW_VER}))){window.open(openUrl,'_blank','noopener');updateDismissedVersion='';localStorage.removeItem('updateDismissedVersion');}else if(!manual){updateDismissedVersion=latest;localStorage.setItem('updateDismissedVersion',latest);}}catch(e){if(manual)alert(t.updateError);}}
async function saveWifi(){const s=document.getElementById('newSSID').value.trim();const p=document.getElementById('newPass').value;if(!s){alert(LG==='es'?'Introduce el SSID':'Enter SSID');return;}const r=await fetch('/savewifi',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(p)+'&noreset=1'});if(r.ok)alert(LG==='es'?'WiFi guardado. Reiniciando...':'WiFi saved. Restarting...');else alert('Error');}
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
async function refresh(){try{const d=await(await fetch('/status')).json();if(d.ver){FW_VER='v'+normVer(d.ver);if(updateDismissedVersion&&cmpVer(updateDismissedVersion,FW_VER)<=0){updateDismissedVersion='';localStorage.removeItem('updateDismissedVersion');}}if(d.repo){FW_REPO=d.repo;}for(const z of['1','2']){const tv=parseFloat(d['t'+z]).toFixed(1);const te=document.getElementById('t'+z);te.textContent=tv+'\u00b0C';te.className='sv '+tc(parseFloat(tv));setFanState(z,d['on'+z]);}sf('z1th',d.z1th);sf('z1hy',d.z1hy);sf('z2th',d.z2th);sf('z2hy',d.z2hy);setZoneUI('1',d.z1auto,d.z1man);setZoneUI('2',d.z2auto,d.z2man);const sc=parseInt(d.sc)||0;const t=TR[LG];document.getElementById('fwVersion').textContent=FW_VER;document.getElementById('footer').textContent=fmt(t.footer,{version:FW_VER});for(let z=1;z<=2;z++){const el=document.getElementById('sensInfo'+z);if(!el)continue;if(sc===0){el.textContent='🌡️ '+t.sensNone;}else if(sc===1){el.innerHTML='🌡️ '+t.sens1+' &bull; '+(z===1?t.sensZ1:t.sensShared);}else{el.innerHTML='🌡️ '+t.sens1+(z===2?' #2':' #1')+' &bull; '+(z===1?t.sensZ1:t.sensZ2);}}for(const z of['1','2']){const pk=parseFloat(d['mx'+z]);if(!isNaN(pk)&&pk>-100){if(maxTemp[z]===null||pk>maxTemp[z])maxTemp[z]=pk;const el=document.getElementById('mx'+z);if(el)el.textContent=maxTemp[z].toFixed(1)+'\u00b0C';}}if(!updatePrompted&&FW_REPO){updatePrompted=true;setTimeout(()=>checkForUpdates(false),1200);}}catch(e){}}
async function saveZone(e,z){e.preventDefault();const p=new URLSearchParams({zone:z,thigh:document.getElementById('z'+z+'th').value,hyst:document.getElementById('z'+z+'hy').value});const r=await fetch('/set?'+p);if(!r.ok){alert('Error');}else{delete D['z'+z+'th'];delete D['z'+z+'hy'];refresh();}}
window.addEventListener('load',()=>{setLang(LG);fetchHistory();refresh();setInterval(refresh,3000);setInterval(fetchHistory,15000);});
</script></body></html>
)rawliteral";

// ─── Manejadores web ──────────────────────────────────────────────────────────
void handleRoot()   { server.send_P(200, "text/html", HTML_PAGE); }
void handleStatus() { server.send(200, F("application/json"), buildJson()); }

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
    loadSettings();
    loadPeakTemps();
    Serial.printf("Z1: %s  Z2: %s\n", z1Auto ? "AUTO" : "MANUAL", z2Auto ? "AUTO" : "MANUAL");

    // DS18B20
    sensors.begin();
    const int found = sensors.getDeviceCount();
    Serial.printf("DS18B20: %d sensor(es)\n", found);
    if (found >= 1) { sensors.getAddress(addr1, 0); sensors.setResolution(addr1, 11); }
    if (found >= 2) { sensors.getAddress(addr2, 1); sensors.setResolution(addr2, 11); twoSensors = true; }
    else Serial.println(F("AVISO: zona 2 usara la misma lectura que zona 1"));

    // Credenciales WiFi
    char wifiSSID[33] = {0}, wifiPass[65] = {0};
    const bool hasWifi = loadWifiCreds(wifiSSID, wifiPass);

    if (!hasWifi) {
      // ── MODO AP ──────────────────────────────────────────────────────────
      apMode = true;
      Serial.println(F("Sin credenciales WiFi → modo AP 'SnapFan-Setup'"));
      WiFi.mode(WIFI_AP);
      WiFi.softAP("SnapFan-Setup");
      delay(1000); // Esperar a que la IP esté lista
      Serial.print(F("IP AP: ")); Serial.println(WiFi.softAPIP());
      dnsServer.start(53, "*", WiFi.softAPIP());
      server.on("/", HTTP_GET,  []() { server.send_P(200, "text/html", AP_PAGE); });
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
      server.on("/status",   HTTP_GET, handleStatus);
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

// ─── Loop ─────────────────────────────────────────────────────────────────────
void loop() {
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

    if (millis() - lastTempMs >= TEMP_MS) {
        lastTempMs = millis();
        sensors.requestTemperatures();
        const float r1 = twoSensors ? sensors.getTempC(addr1) : sensors.getTempCByIndex(0);
        if (r1 != DEVICE_DISCONNECTED_C) temp1 = r1;
        const float r2 = twoSensors ? sensors.getTempC(addr2) : temp1;
        if (r2 != DEVICE_DISCONNECTED_C) temp2 = r2;
        // Actualizar historial circular
        hist1[histIdx] = temp1; hist2[histIdx] = temp2;
        histIdx = (histIdx + 1) % HIST_SIZE;
        if (histCount < HIST_SIZE) histCount++;
        // Actualizar temperaturas máximas persistentes
        bool pkChanged = false;
        if (temp1 > peakTemp1) { peakTemp1 = temp1; pkChanged = true; }
        if (temp2 > peakTemp2) { peakTemp2 = temp2; pkChanged = true; }
        if (pkChanged) savePeakTemps();
        controlFans();
    }

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
