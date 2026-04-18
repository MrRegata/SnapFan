param(
    [string]$Repo = "MrRegata/SnapFan",
    [string]$Branch = "main",
    [string]$Message = "Initial SnapFan source import",
    [string]$GitName = "",
    [string]$GitEmail = "",
    [switch]$SkipPush
)

$ErrorActionPreference = "Stop"

function Require-Command($command, $hint) {
    if (-not (Get-Command $command -ErrorAction SilentlyContinue)) {
        throw "$command no está disponible. $hint"
    }
}

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

Require-Command git "Instala Git para Windows y vuelve a intentarlo."

$projectDir = Split-Path $PSScriptRoot -Parent
Set-Location $projectDir

$gh = "C:\Program Files\GitHub CLI\gh.exe"
if (-not (Test-Path $gh)) {
    throw "No se encontró GitHub CLI en $gh"
}

Invoke-Native -FilePath $gh -Arguments @("auth", "status")

$gitDir = Join-Path $projectDir ".git"
if (-not (Test-Path $gitDir)) {
    Invoke-Native -FilePath "git" -Arguments @("init")
}

$existingRemote = ""
$existingRemote = (& git remote get-url origin 2>$null)
if ($LASTEXITCODE -ne 0) {
    $existingRemote = ""
}

if ($GitName) {
    Invoke-Native -FilePath "git" -Arguments @("config", "user.name", $GitName)
}
if ($GitEmail) {
    Invoke-Native -FilePath "git" -Arguments @("config", "user.email", $GitEmail)
}

$configuredName = (& git config --get user.name)
$configuredEmail = (& git config --get user.email)
if (-not $configuredName -or -not $configuredEmail) {
    throw "Git necesita user.name y user.email. Ejecuta el script con -GitName y -GitEmail."
}

$remoteUrl = "https://github.com/$Repo.git"
if (-not $existingRemote) {
    Invoke-Native -FilePath "git" -Arguments @("remote", "add", "origin", $remoteUrl)
} elseif ($existingRemote -ne $remoteUrl) {
    Invoke-Native -FilePath "git" -Arguments @("remote", "set-url", "origin", $remoteUrl)
}

Invoke-Native -FilePath "git" -Arguments @("checkout", "-B", $Branch)
Invoke-Native -FilePath "git" -Arguments @("add", ".")

& git diff --cached --quiet
$hasChanges = ($LASTEXITCODE -ne 0)

if ($hasChanges) {
    Invoke-Native -FilePath "git" -Arguments @("commit", "-m", $Message)
}

if (-not $SkipPush) {
    Invoke-Native -FilePath "git" -Arguments @("push", "-u", "origin", $Branch)
}

Write-Host ""
if ($SkipPush) {
    Write-Host "Commit local preparado correctamente en $Branch"
} else {
    Write-Host "Repositorio publicado correctamente en https://github.com/$Repo"
}
