$ErrorActionPreference="Stop"
if (-not $env:VITASDK) { throw "VITASDK n'est pas défini. Installe VitaSDK puis ouvre un terminal où 'arm-vita-eabi-gcc' est disponible." }
Set-Location "$PSScriptRoot\VitaSync"
make clean
make
Write-Host "Build terminé: VitaSync.skprx"
