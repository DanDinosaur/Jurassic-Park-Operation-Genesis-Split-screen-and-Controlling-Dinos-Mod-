# uninstall.ps1 - removes JPOG Co-op v2 and restores every original from its *.prejpogmod.bak.
param([string]$Game = "")
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "findgame.ps1")
$Game = Find-Game $Game
if (Get-Process -Name SimJP -ErrorAction SilentlyContinue) { throw "The game is running - close it and run uninstall.bat again." }
Write-Host "Game folder: $Game"
foreach ($rel in "Data\FslHunt.ini","Data\Constant.ini","Options.ini","Data\GUI\main.gui","dinput8.dll","JPOGMouseFix.asi") {
    $f = Join-Path $Game $rel; $bak = "$f.prejpogmod.bak"
    if (Test-Path $bak) { Copy-Item $bak $f -Force; Remove-Item $bak -Force; Write-Host "restored $rel" }
    elseif ($rel -like "*.dll") { if (Test-Path $f) { Remove-Item $f -Force; Write-Host "removed $rel" } }
    elseif ($rel -like "*.asi" -and (Test-Path (Join-Path $PSScriptRoot "files\$rel"))) { if (Test-Path $f) { Remove-Item $f -Force; Write-Host "removed $rel" } }
}
foreach ($rel in "jpogmod.ini.prejpogmod.bak","JPOGMouseFix.ini.prejpogmod.bak","jpogmod.installed") {
    $f = Join-Path $Game $rel; if (Test-Path $f) { Remove-Item $f -Force }
}
Write-Host ""
Write-Host "Mod removed. (Your settings files jpogmod.ini / JPOGMouseFix.ini were left in place.)"
