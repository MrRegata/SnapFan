# ============================================================
#  generar_zip.ps1 — Empaqueta el proyecto SnapFan 24V en ZIP
#  Ejecutar desde la raíz del proyecto o directamente.
#  Crea: SnapFan_U1_Firmware_YYYYMMDD.zip en la carpeta docs/
# ============================================================

$projectDir = Split-Path $PSScriptRoot -Parent
$date       = Get-Date -Format "yyyyMMdd"
$outZip     = Join-Path $PSScriptRoot "SnapFan_U1_Firmware_$date.zip"

# Archivos a incluir (rutas relativas al proyecto)
$files = @(
    (Join-Path $projectDir "src\main.cpp"),
    (Join-Path $projectDir "platformio.ini"),
    (Join-Path $PSScriptRoot "SnapFan_Documentacion.html")
)

# Firmare binario compilado (puede no existir si no se ha compilado antes)
$firmware = Join-Path $projectDir ".pio\build\esp12e\firmware.bin"
if (Test-Path $firmware) {
    $files += $firmware
    Write-Host "Firmware .bin encontrado: incluido en el ZIP."
} else {
    Write-Warning "No se encontró .pio\build\esp12e\firmware.bin."
    Write-Warning "Ejecuta primero: pio run   (en la raíz del proyecto)"
    Write-Warning "El ZIP se creará SIN el binario compilado."
}

# Verificar que existan los archivos obligatorios
$missing = $files | Where-Object { -not (Test-Path $_) }
if ($missing.Count -gt 0) {
    Write-Error "Archivos no encontrados:`n$($missing -join "`n")"
    exit 1
}

# Crear ZIP (fuerza sobreescritura si ya existe)
if (Test-Path $outZip) { Remove-Item $outZip -Force }

# Empaquetar preservando nombres sin estructura de directorios extra
$tmpDir = Join-Path $env:TEMP "SnapFanZip_$date"
if (Test-Path $tmpDir) { Remove-Item $tmpDir -Recurse -Force }
New-Item -ItemType Directory -Path $tmpDir | Out-Null

foreach ($f in $files) {
    Copy-Item $f -Destination $tmpDir
}

Compress-Archive -Path (Join-Path $tmpDir "*") -DestinationPath $outZip -Force
Remove-Item $tmpDir -Recurse -Force

Write-Host ""
Write-Host "======================================================"
Write-Host "  ZIP generado correctamente:"
Write-Host "  $outZip"
Write-Host "  Tamaño: $([math]::Round((Get-Item $outZip).Length/1KB,1)) KB"
Write-Host "  Archivos incluidos:"
$files | ForEach-Object { Write-Host "    - $(Split-Path $_ -Leaf)" }
Write-Host "======================================================"
