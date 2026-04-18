param(
    [string]$Repo = "MrRegata/SnapFan",
    [switch]$Build,
    [string]$Notes = "Firmware OTA SnapFan"
)

$ErrorActionPreference = "Stop"

function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,
        [string[]]$Arguments = @(),
        [switch]$AllowNonZero
    )

    & $FilePath @Arguments
    if (-not $AllowNonZero -and $LASTEXITCODE -ne 0) {
        throw "Falló el comando: $FilePath $($Arguments -join ' ')"
    }
}

function Get-IniValue {
    param(
        [string]$Path,
        [string]$Key
    )

    $line = Get-Content $Path | Where-Object { $_ -match "^\s*$([regex]::Escape($Key))\s*=" } | Select-Object -First 1
    if (-not $line) {
        throw "No se encontró la clave '$Key' en $Path"
    }
    return ($line -split "=", 2)[1].Trim()
}

$projectDir = Split-Path $PSScriptRoot -Parent
Set-Location $projectDir

$gh = "C:\Program Files\GitHub CLI\gh.exe"
if (-not (Test-Path $gh)) {
    throw "No se encontró GitHub CLI en $gh"
}

Invoke-Native -FilePath $gh -Arguments @("auth", "status")

$repoInfo = & $gh api "repos/$Repo"
$repoJson = $repoInfo | ConvertFrom-Json
if ($repoJson.size -eq 0) {
    throw "El repositorio $Repo está vacío. Sube primero el código fuente con tools/publish_repo.ps1"
}

$platformio = Join-Path $projectDir "platformio.ini"
$version = Get-IniValue -Path $platformio -Key "custom_firmware_version"
$tag = "v$version"
$asset = Join-Path $projectDir ".pio\build\esp32c3\snapfan-esp32c3-v$version.bin"

if ($Build -or -not (Test-Path $asset)) {
    Invoke-Native -FilePath "pio" -Arguments @("run")
}

if (-not (Test-Path $asset)) {
    throw "No se encontró el binario versionado: $asset"
}

$releaseExists = $true
$null = (& $gh release view $tag --repo $Repo 2>$null)
if ($LASTEXITCODE -ne 0) {
    $releaseExists = $false
}

if ($releaseExists) {
    Invoke-Native -FilePath $gh -Arguments @("release", "upload", $tag, $asset, "--repo", $Repo, "--clobber")
    Write-Host "Release $tag actualizada con $asset"
} else {
    Invoke-Native -FilePath $gh -Arguments @("release", "create", $tag, $asset, "--repo", $Repo, "--title", $tag, "--notes", "$Notes`n`nArchivo: $(Split-Path $asset -Leaf)")
    Write-Host "Release $tag creada con $asset"
}
