# SnapFan

Control de ventiladores 24V para Snapmaker U1 con ESP32-C3, interfaz web y actualizaciones OTA.

## Qué hace

- Controla 2 zonas de ventiladores de 2 pines
- Modo AUTO con umbral e histéresis
- Modo MANUAL desde la web
- OTA desde navegador
- Publicación automática de firmware en GitHub Releases
- Detección de nuevas versiones desde la web del ESP

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
- `tools/versioning.py`: crea el `.bin` versionado
- `tools/publish_repo.ps1`: sube el código fuente al repo
- `tools/publish_release.ps1`: publica la release del firmware
- `tools/publish_all.ps1`: flujo completo de publicación

## OTA

Los binarios publicados se adjuntan a las releases de GitHub con este formato:

- `snapfan-esp32c3-vX.Y.Z.bin`

La web del ESP consulta el repo:

- `MrRegata/SnapFan`

## Repositorio

- https://github.com/MrRegata/SnapFan
