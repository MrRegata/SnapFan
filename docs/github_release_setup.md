Publicar en GitHub desde este proyecto

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

3. Cambiar versión.

Edita:

- platformio.ini

Clave:

- custom_firmware_version = 0.0.0

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