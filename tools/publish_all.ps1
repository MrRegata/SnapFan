param(
    [string]$Repo = "MrRegata/SnapFan",
    [string]$Version = "",
    [string]$GitName = "",
    [string]$GitEmail = "",
    [string]$Branch = "main",
    [switch]$AutoVersion,
    [switch]$Upload
)

$ErrorActionPreference = "Stop"

function Get-IniValue {
    param(
        [string]$Path,
        [string]$Key
    )

    $line = Get-Content $Path | Where-Object { $_ -match "^\s*$([regex]::Escape($Key))\s*=" } | Select-Object -First 1
    if (-not $line) {
        throw "No se encontró la clave '$Key' en $Path"
    }
    return ($line -split '=', 2)[1].Trim()
}

function Set-IniValue {
    param(
        [string]$Path,
        [string]$Key,
        [string]$Value
    )

    $content = Get-Content $Path -Raw
    $pattern = "(?m)^\s*$([regex]::Escape($Key))\s*=\s*.*$"
    if ($content -notmatch $pattern) {
        throw "No se encontró la clave '$Key' en $Path"
    }
    $updated = [regex]::Replace($content, $pattern, "$Key = $Value", 1)
    $normalized = $updated.TrimEnd("`r", "`n") + "`r`n"
    [System.IO.File]::WriteAllText($Path, $normalized, [System.Text.Encoding]::ASCII)
}

function Get-NextVersion {
    param([string]$CurrentVersion)

    $parts = $CurrentVersion.Split('.')
    if ($parts.Count -ne 3) {
        throw "La versión actual '$CurrentVersion' no tiene formato X.Y.Z"
    }

    $major = [int]$parts[0]
    $minor = [int]$parts[1]
    $patch = [int]$parts[2] + 1
    return "$major.$minor.$patch"
}

$projectDir = Split-Path $PSScriptRoot -Parent
Set-Location $projectDir

$platformio = Join-Path $projectDir "platformio.ini"
$currentVersion = Get-IniValue -Path $platformio -Key "custom_firmware_version"
if ($AutoVersion -or -not $Version) {
    $Version = Get-NextVersion -CurrentVersion $currentVersion
}

if ($Version -ne $currentVersion) {
    Set-IniValue -Path $platformio -Key "custom_firmware_version" -Value $Version
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

if ($Upload) {
    pio run -t upload
    if ($LASTEXITCODE -ne 0) {
        throw "Falló la subida del firmware al ESP"
    }
}

Write-Host ""
if ($Upload) {
    Write-Host "Flujo completo terminado: código + release v$Version publicados en https://github.com/$Repo y firmware subido al ESP"
} else {
    Write-Host "Flujo completo terminado: código + release v$Version publicados en https://github.com/$Repo"
}