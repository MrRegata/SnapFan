# SnapFan

Control de ventiladores 24V para Snapmaker U1 con ESP32-C3, interfaz web y actualizaciones OTA.

## Qué hace

- Controla 2 zonas de ventiladores de 2 pines
- Puede leer el estado de Klipper a traves de Moonraker y reflejarlo en una tira WS2812
- Modo AUTO con umbral e histéresis
- Modo MANUAL desde la web
- Configuración web de sensores DS18B20 por ROM ID
- Configuración web de la salida física de ventilador entre IO5 e IO6 por zona
- Exploración WiFi con selección rápida de SSID y entrada manual
- Descubrimiento en red por mDNS, NetBIOS y SSDP
- OTA desde navegador
- Publicación automática de firmware en GitHub Releases
- Detección de nuevas versiones desde la web del ESP

## Release actual

- Versión objetivo actual: v0.5.1.
- Binario OTA esperado al compilar: `snapfan-esp32c3-v0.5.1.bin`.
- Repo de releases OTA: `MrRegata/SnapFan`.

## Conexiones actuales

- Alimentación de la placa: 24V de la impresora hacia un reductor MP1584EN y salida a 3.3V para el ESP32-C3.
- GPIO4: bus OneWire para los sensores DS18B20.
- GPIO5: salida al MOSFET de la zona 1.
- GPIO6: salida al MOSFET de la zona 2.
- GPIO7: LED de estado activo en LOW.
- GPIO3: salida de datos para una tira WS2812/NeoPixel de estado de impresora.

## Integracion con Klipper y tira LED

- El firmware puede consultar Moonraker del host Klipper para leer el estado de impresion.
- Estados contemplados en esta ampliacion: calentando, imprimiendo, pausada, finalizada, error, inactiva y offline.
- Colores por defecto:
	- calentando: naranja parpadeante
	- imprimiendo: verde fijo
	- pausada: amarillo parpadeante
	- finalizada: azul fijo
	- error: rojo parpadeante
	- inactiva: azul tenue
	- offline: morado tenue
- Parametros a ajustar en [src/main.cpp](src/main.cpp): KLIPPER_HOST, KLIPPER_PORT, WLED_PIN y WLED_COUNT.
- Conexion recomendada para la tira: DIN a GPIO3 con resistencia serie de 330 ohm, GND comun y alimentacion adecuada para la tira.

## Sensores y zonas

- El firmware soporta 1 o 2 DS18B20 en el mismo bus OneWire.
- Si detecta 1 sensor, ese sensor se usa como referencia general.
- Si detecta 2 sensores, el primero se asigna a la zona 1 y el segundo a la zona 2.
- Zona 1: Drivers de motores.
- Zona 2: Fuente de alimentación.

## Etapa de potencia

- Los ventiladores son de 24V y 2 pines, así que el control es todo o nada.
- Cada zona conmuta su línea de alimentación mediante MOSFET.
- Nivel lógico HIGH en GPIO5 o GPIO6 implica ventilador activo en su zona.

## WiFi y mantenimiento

- Primer arranque sin credenciales: el equipo crea el AP abierto SnapFan-Setup.
- Portal de configuración: http://192.168.4.1.
- Cambio posterior de red: desde el botón WiFi de la web o la ruta /wifireset.
- OTA web disponible en la ruta /update.

## Estado de esta carpeta

- Esta carpeta local ha sido restaurada desde el repositorio de GitHub.
- El remoto configurado es MrRegata/SnapFan.
- El entorno principal de PlatformIO es esp32c3 para placa esp32-c3-devkitm-1.

## Uso rápido en VS Code

### Publicar todo con un clic

Pulsa `Ctrl+Shift+B`.

La tarea por defecto `Publicar y subir al ESP` hace esto:

1. incrementa automáticamente la versión `0.x.x`
2. guarda esa versión en `platformio.ini`
3. hace commit y push del código al repo
4. compila el firmware
5. publica o actualiza la release en GitHub
6. sube el firmware por cable al ESP usando el puerto configurado

### Publicar con versión manual

Usa la tarea `Publicar todo a GitHub` si quieres escribir la versión manualmente.

### Publicar solo OTA a GitHub

Usa la tarea `Publicar OTA a GitHub` si quieres:

1. subir código al repo
2. crear o actualizar la release OTA
3. incrementar la versión automáticamente

pero sin cargar el firmware por cable al ESP.

## Archivos importantes

- `platformio.ini`: configuración de PlatformIO y versión actual
- `src/main.cpp`: firmware principal
- `docs/flash_esp32.md`: guía para cargar el firmware por cable al ESP32-C3
- `tools/versioning.py`: crea el `.bin` versionado
- `tools/publish_repo.ps1`: sube el código fuente al repo
- `tools/publish_release.ps1`: publica la release del firmware
- `tools/publish_all.ps1`: flujo completo de publicación

## OTA

Los binarios publicados se adjuntan a las releases de GitHub con este formato:

- `snapfan-esp32c3-vX.Y.Z.bin`
- `snapfan-esp32c3-v0.5.1.bin` para esta entrega

La web del ESP consulta el repo:

- `MrRegata/SnapFan`

## Repositorio

- https://github.com/MrRegata/SnapFan
