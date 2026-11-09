# findgame.ps1 - finds the Jurassic Park: Operation Genesis folder (the one with SimJP.exe).
# Order: the -Game argument, game_path.txt from an earlier install, folders next to / above this
# package, then asks.
function Find-Game([string]$Game) {
    $here = $PSScriptRoot
    $cands = @()
    if ($Game) { $cands += $Game }
    $saved = Join-Path $here "game_path.txt"
    if (Test-Path $saved) { $cands += (Get-Content $saved -TotalCount 1) }
    $d = $here
    for ($i = 0; $i -lt 6 -and $d; $i++) {
        $cands += $d
        $cands += (Join-Path $d "jurassic park")
        $d = Split-Path $d -Parent
    }
    foreach ($c in $cands) {
        if ($c -and (Test-Path (Join-Path $c "SimJP.exe"))) { return (Resolve-Path $c).Path }
    }
    while ($true) {
        $c = Read-Host "Where is the game? Paste the folder that contains SimJP.exe"
        $c = $c.Trim('"', ' ')
        if ($c -and (Test-Path (Join-Path $c "SimJP.exe"))) { return (Resolve-Path $c).Path }
        Write-Host "SimJP.exe is not in that folder - try again."
    }
}
