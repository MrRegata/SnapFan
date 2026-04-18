param(
    [string]$Repo = "MrRegata/SnapFan",
    [string]$Version = "",
    [string]$GitName = "",
    [string]$GitEmail = "",
    [string]$Branch = "main"
)

$ErrorActionPreference = "Stop"

$projectDir = Split-Path $PSScriptRoot -Parent
Set-Location $projectDir

$platformio = Join-Path $projectDir "platformio.ini"
if (-not $Version) {
    $line = Get-Content $platformio | Where-Object { $_ -match '^\s*custom_firmware_version\s*=' } | Select-Object -First 1
    if (-not $line) {
        throw "No se encontró custom_firmware_version en platformio.ini"
    }
    $Version = ($line -split '=', 2)[1].Trim()
}

$commitMessage = "Release v$Version"

$repoArgs = @(
    "-ExecutionPolicy", "Bypass",
    "-File", (Join-Path $projectDir "tools\publish_repo.ps1"),
    "-Repo", $Repo,
    "-Branch", $Branch,
    "-Message", $commitMessage
)
if ($GitName) {
    $repoArgs += @("-GitName", $GitName)
}
if ($GitEmail) {
    $repoArgs += @("-GitEmail", $GitEmail)
}

& powershell @repoArgs
if ($LASTEXITCODE -ne 0) {
    throw "Falló la publicación del código fuente"
}

$releaseArgs = @(
    "-ExecutionPolicy", "Bypass",
    "-File", (Join-Path $projectDir "tools\publish_release.ps1"),
    "-Repo", $Repo,
    "-Build",
    "-Version", $Version
)

& powershell @releaseArgs
if ($LASTEXITCODE -ne 0) {
    throw "Falló la publicación de la release del firmware"
}

Write-Host ""
Write-Host "Flujo completo terminado: código + release v$Version publicados en https://github.com/$Repo"