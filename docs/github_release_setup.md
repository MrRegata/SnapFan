Publicar en GitHub desde este proyecto

Release preparada actualmente

- versión base: 0.5.0
- tag esperado en GitHub: v0.5.0
- binario OTA esperado: .pio/build/esp32c3/snapfan-esp32c3-v0.5.0.bin

Botón único en VS Code

1. Pulsa Ctrl+Shift+B.
2. La tarea por defecto Publicar y subir al ESP hará esto sola:

- incrementa automáticamente la versión 0.x.x
- actualiza custom_firmware_version en platformio.ini
- hace commit y push del código fuente
- compila el firmware
- publica o actualiza la release con el binario OTA
- genera notas automáticas de release con versión, archivo, commit y fecha
- sube el firmware al ESP por cable usando PlatformIO

3. Si quieres elegir la versión manualmente, usa la tarea Publicar todo a GitHub.
4. Si quieres publicar una OTA nueva pero sin subir por cable al ESP, usa la tarea Publicar OTA a GitHub.


1. Primera vez: sube el código fuente al repo.

Comando:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\publish_repo.ps1
```

Esto hace:
- inicializa Git si hace falta
- crea o actualiza la rama main
- conecta el remoto a MrRegata/SnapFan
- sube el código fuente al repositorio

2. Publicar una release de firmware OTA.

Comando:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\publish_release.ps1 -Build
```

Esto hace:
- lee la versión desde platformio.ini
- compila si hace falta
- usa el archivo .pio/build/esp32c3/snapfan-esp32c3-vX.Y.Z.bin
- crea la release vX.Y.Z si no existe
- o reemplaza el .bin en la release si ya existe

Tarea recomendada para OTA sin cable:

- Publicar OTA a GitHub

Esto hace:
- incrementa automáticamente la versión 0.x.x
- hace commit y push del proyecto
- compila el firmware
- publica la release OTA
- no sube el firmware al ESP por cable

Comando de flujo completo:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\publish_all.ps1 -Version 0.0.1
```

Ejemplo actual para esta release:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\publish_all.ps1 -Version 0.5.0
```

Esto hace:
- actualiza la versión en platformio.ini
- hace commit/push del proyecto con mensaje Release vX.Y.Z
- compila y publica la release en GitHub
- genera notas automáticas

3. Cambiar versión.

Edita:

- platformio.ini

Clave:

- custom_firmware_version = 0.0.0

Valor actual recomendado para esta publicación:

- custom_firmware_version = 0.5.0

Mientras estés en fase de pruebas puedes seguir usando versiones 0.x.x.

4. Flujo recomendado.

- Cambias código
- Subes la versión en platformio.ini
- Ejecutas publish_release.ps1 -Build
- GitHub publica el .bin para OTA
- La web del ESP detecta la nueva release

Notas

- GitHub CLI debe estar autenticado.
- Si el repo está vacío, primero ejecuta publish_repo.ps1.
- Si quieres cambiar de repo en el futuro, ambos scripts aceptan -Repo owner/nombre.