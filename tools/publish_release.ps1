param(
    [string]$Repo = "MrRegata/SnapFan",
    [switch]$Build,
    [string]$Notes = "",
    [string]$Version = ""
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

function Get-ReleaseNotes {
    param(
        [string]$Tag,
        [string]$AssetPath,
        [string]$FallbackNotes
    )

    if ($FallbackNotes) {
        return $FallbackNotes
    }

    $commitSha = (& git rev-parse --short HEAD 2>$null)
    if ($LASTEXITCODE -ne 0) {
        $commitSha = "sin-commit"
    }
    $commitTitle = (& git log -1 --pretty=%s 2>$null)
    if ($LASTEXITCODE -ne 0) {
        $commitTitle = "Actualización de firmware"
    }
    $builtAt = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
    $assetName = Split-Path $AssetPath -Leaf

    return @(
        "Firmware OTA SnapFan",
        "",
        "Version: $Tag",
        "Archivo: $assetName",
        "Commit: $commitSha",
        "Resumen: $commitTitle",
        "Compilado: $builtAt"
    ) -join "`n"
}

$projectDir = Split-Path $PSScriptRoot -Parent
Set-Location $projectDir

$gh = "C:\Program Files\GitHub CLI\gh.exe"
if (-not (Test-Path $gh)) {
    throw "No se encontró GitHub CLI en $gh"
}

Invoke-Native -FilePath $gh -Arguments @("auth", "status")

$branchesJson = & $gh api "repos/$Repo/branches"
$branches = $branchesJson | ConvertFrom-Json
if (-not $branches -or $branches.Count -eq 0) {
    throw "El repositorio $Repo no tiene ramas remotas aún. Sube primero el código fuente con tools/publish_repo.ps1"
}

$platformio = Join-Path $projectDir "platformio.ini"
if ($Version) {
    Set-IniValue -Path $platformio -Key "custom_firmware_version" -Value $Version
}

$version = Get-IniValue -Path $platformio -Key "custom_firmware_version"
$tag = "v$version"
$asset = Join-Path $projectDir ".pio\build\esp32c3\snapfan-esp32c3-v$version.bin"

if ($Build -or -not (Test-Path $asset)) {
    Invoke-Native -FilePath "pio" -Arguments @("run")
}

if (-not (Test-Path $asset)) {
    throw "No se encontró el binario versionado: $asset"
}

$resolvedNotes = Get-ReleaseNotes -Tag $tag -AssetPath $asset -FallbackNotes $Notes

$releaseExists = $true
try {
    $null = (& $gh release view $tag --repo $Repo *> $null)
} catch {
}
if ($LASTEXITCODE -ne 0) {
    $releaseExists = $false
}

if ($releaseExists) {
    Invoke-Native -FilePath $gh -Arguments @("release", "upload", $tag, $asset, "--repo", $Repo, "--clobber")
    Invoke-Native -FilePath $gh -Arguments @("release", "edit", $tag, "--repo", $Repo, "--notes", $resolvedNotes)
    Write-Host "Release $tag actualizada con $asset"
} else {
    Invoke-Native -FilePath $gh -Arguments @("release", "create", $tag, $asset, "--repo", $Repo, "--title", $tag, "--notes", $resolvedNotes)
    Write-Host "Release $tag creada con $asset"
}
