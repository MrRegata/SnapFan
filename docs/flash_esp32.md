# Cargar SnapFan por cable en el ESP32-C3

Esta guía sirve para grabar el firmware SnapFan en la placa por USB usando PlatformIO.

## Requisitos

- VS Code
- Extensión PlatformIO IDE instalada en VS Code
- Python y dependencias que instala PlatformIO automáticamente
- Cable USB de datos conectado al ESP32-C3
- Puerto serie correcto disponible, en este proyecto: COM6

## Configuración actual del proyecto

- Entorno PlatformIO: esp32c3
- Placa: esp32-c3-devkitm-1
- Framework: arduino
- Puerto de subida: COM6
- Puerto de monitor: COM6
- Velocidad de monitor: 115200
- Versión actual del firmware: 0.5.0

## Librerías necesarias

No hace falta instalarlas a mano si compilas con PlatformIO dentro de este proyecto. PlatformIO las descargará e instalará automáticamente desde platformio.ini.

Dependencias declaradas:

- paulstoffregen/OneWire @ ^2.3.8
- milesburton/DallasTemperature @ ^3.11.0
- knolleary/PubSubClient @ ^2.8.0

## Preparación del hardware

- Conecta el ESP32-C3 por USB al PC.
- Asegúrate de que Windows lo detecta como COM6, o ajusta platformio.ini si cambia de puerto.
- Cierra cualquier monitor serie o programa que esté usando COM6 antes de subir el firmware.

## Cargar el firmware desde VS Code

1. Abre esta carpeta del proyecto en VS Code.
2. Espera a que PlatformIO termine de indexar el proyecto.
3. Abre una terminal en la raíz del proyecto.
4. Ejecuta este comando:

```powershell
platformio run -e esp32c3 --target upload --upload-port COM6 -j 1
```

5. Espera al mensaje final SUCCESS.

## Cargar el firmware desde la interfaz de PlatformIO

1. Abre la barra lateral de PlatformIO.
2. En el entorno esp32c3, pulsa Upload.
3. Si falla por puerto ocupado, cierra primero el monitor serie y repite.

## Verificar el arranque

Después de grabar, puedes abrir el monitor serie con:

```powershell
platformio device monitor --port COM6 --baud 115200 --filter time
```

Mensajes útiles esperados:

- Firmware version: v0.5.0
- DS18B20: X sensor(es)
- Temperaturas -> Z1: ... C | Z2: ... C

## Problemas habituales

### COM6 ocupado

Síntoma:

- Acceso denegado al abrir COM6

Solución:

- Cierra el monitor serie de PlatformIO u otra aplicación que esté usando el puerto.

### No detecta sensores DS18B20

Comprueba:

- DATA del DS18B20 al GPIO4
- 3.3V y GND correctos
- resistencia pull-up de 4.7k entre DATA y 3.3V

Incidencia real ya observada en este proyecto:

- conectar por error el sensor a GND en lugar de GPIO4

## Binario OTA generado al compilar

Cuando la compilación termina correctamente, el script de versionado crea además este archivo:

```text
.pio/build/esp32c3/snapfan-esp32c3-v0.5.0.bin
```

Ese es el archivo que se publica en GitHub Releases para actualización OTA.