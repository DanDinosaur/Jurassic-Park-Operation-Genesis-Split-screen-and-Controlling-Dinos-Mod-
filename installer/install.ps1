# install.ps1 - JPOG Co-op v2 installer (called by install.bat). Re-runnable (idempotent).
# Copies the mod files and applies the data tweaks as TEXT PATCHES to your own game files
# (no game files are shipped). Each changed original is backed up once as *.prejpogmod.bak.
param([string]$Game = "", [switch]$EasyStart, [switch]$NormalStart)
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
. (Join-Path $here "findgame.ps1")
$Game = Find-Game $Game
Write-Host "Game folder: $Game"
Set-Content (Join-Path $here "game_path.txt") $Game
$bin = Join-Path $here "files"

function Backup($path) {
    $bak = "$path.prejpogmod.bak"
    if ((Test-Path $path) -and -not (Test-Path $bak)) { Copy-Item $path $bak; Write-Host "  backup -> $(Split-Path $bak -Leaf)" }
}
# True if the file is one of OUR binaries (never treat those as the "original").
function Is-Ours($path) {
    if (-not (Test-Path $path)) { return $false }
    $b = [IO.File]::ReadAllBytes($path)
    $a = [Text.Encoding]::ASCII.GetString($b); $u = [Text.Encoding]::Unicode.GetString($b)
    $u2 = if ($b.Length -gt 1) { [Text.Encoding]::Unicode.GetString($b, 1, $b.Length - 1) } else { "" }
    foreach ($t in $a, $u, $u2) { if ($t.Contains("jpogmod") -or $t.Contains("JPOGMouseFix")) { return $true } }
    return $false
}
function Read-Text($path)  { [IO.File]::ReadAllText($path, [Text.Encoding]::Default) }
function Write-Text($path, $s) { [IO.File]::WriteAllText($path, $s, [Text.Encoding]::Default) }
# Set "Key = value;" (first match). Keeps indentation; leaves the rest of the line's comment alone.
function Set-Key([ref]$s, $key, $value) {
    $rx = "(?m)^([ \t]*" + [regex]::Escape($key) + "[ \t]*=[ \t]*)[^;\r\n]*;"
    if (-not [regex]::IsMatch($s.Value, $rx)) { Write-Warning "  key not found: $key"; return }
    $s.Value = ([regex]$rx).Replace($s.Value, '${1}' + $value + ';', 1)
}
# Settings files: install the default if missing; otherwise ADD keys/sections that are new in this
# version (with their comments) and keep every value the player already has.
function Merge-Ini($template, $target) {
    if (-not (Test-Path $target)) { Copy-Item $template $target; Write-Host "installed $(Split-Path $target -Leaf) (default settings)"; return }
    $have = @{}; $sec = ""
    foreach ($l in [IO.File]::ReadAllLines($target)) {
        if ($l -match '^\s*\[([^\]]+)\]') { $sec = $matches[1].ToLower(); $have["$sec|"] = $true }
        elseif ($l -match '^\s*([^;=\s][^=]*?)\s*=') { $have["$sec|" + $matches[1].ToLower()] = $true }
    }
    $lines = [Collections.Generic.List[string]]::new([IO.File]::ReadAllLines($target))
    $sec = ""; $pending = @(); $added = 0; $newSecs = [ordered]@{}
    foreach ($l in [IO.File]::ReadAllLines($template)) {
        if ($l -match '^\s*\[([^\]]+)\]') { $sec = $matches[1].ToLower(); $pending = @(); continue }
        if ($l -match '^\s*;' -or $l.Trim() -eq "") { $pending += $l; continue }
        if ($l -match '^\s*([^;=\s][^=]*?)\s*=') {
            $k = $matches[1].ToLower()
            if (-not $have.ContainsKey("$sec|$k")) {
                $block = @($pending | Where-Object { $_.Trim() -ne "" }) + $l
                if ($have.ContainsKey("$sec|")) {
                    # insert at the end of that section in the target
                    $s2 = ""; $end = $lines.Count
                    for ($i = 0; $i -lt $lines.Count; $i++) {
                        if ($lines[$i] -match '^\s*\[([^\]]+)\]') {
                            if ($s2 -eq $sec) { $end = $i; break }
                            $s2 = $matches[1].ToLower()
                        }
                    }
                    while ($end -gt 0 -and $lines[$end - 1].Trim() -eq "") { $end-- }
                    $lines.InsertRange($end, [string[]]$block)
                } else {
                    if (-not $newSecs.Contains($sec)) { $newSecs[$sec] = [Collections.Generic.List[string]]::new() }
                    $newSecs[$sec].AddRange([string[]]$block)
                }
                $have["$sec|$k"] = $true; $added++
            }
            $pending = @()
        }
    }
    foreach ($s in $newSecs.Keys) { $lines.Add(""); $lines.Add("[$s]"); $lines.AddRange($newSecs[$s]) }
    if ($added -gt 0) {
        Backup $target
        [IO.File]::WriteAllLines($target, $lines)
        Write-Host "updated $(Split-Path $target -Leaf): $added new setting(s) added, your values kept"
    } else { Write-Host "$(Split-Path $target -Leaf) is up to date (your values kept)" }
}

# --- 1. binaries -------------------------------------------------------------------------
if (Get-Process -Name SimJP -ErrorAction SilentlyContinue) { throw "The game is running - close it and run install.bat again." }
$marker = Join-Path $Game "jpogmod.installed"
$first = -not (Test-Path $marker)
# Easy start (all dig sites, 5-star start, 10M money, all research): asked on the first install.
$easy = $true
if ($NormalStart) { $easy = $false }
elseif (-not $EasyStart) {
    $saved = Join-Path $Game "jpogmod.installed"
    $prev = if (Test-Path $saved) { Get-Content $saved -Raw } else { "" }
    if ($prev -match "easy=0") { $easy = $false }
    elseif ($prev -notmatch "easy=1") {
        $a = Read-Host "Easy start? All research + dig sites, parks start at 5 stars, money never runs out. [Y/n]"
        $easy = -not ($a.Trim().ToLower().StartsWith("n"))
    }
}
foreach ($f in "dinput8.dll","JPOGMouseFix.asi") {
    $src = Join-Path $bin $f
    if (-not (Test-Path $src)) {
        if ($f -eq "dinput8.dll") { throw "missing $src" }
        if (Test-Path (Join-Path $Game $f)) { Write-Host "kept your own $f (widescreen / HUD plugin)" }
        else { Write-Warning "$f (JPOG-Modernize widescreen / HUD plugin) not included - the mod works without it; see README" }
        continue
    }
    $dst = Join-Path $Game $f
    if ($first -and -not (Is-Ours $dst)) { Backup $dst }
    Copy-Item $src $dst -Force; Write-Host "installed $f"
}
Merge-Ini (Join-Path $bin "jpogmod.ini") (Join-Path $Game "jpogmod.ini")
if (Test-Path (Join-Path $bin "JPOGMouseFix.ini")) { Merge-Ini (Join-Path $bin "JPOGMouseFix.ini") (Join-Path $Game "JPOGMouseFix.ini") }
if (-not $easy) {   # normal game: the mod's always-on unlocks off
    $p = Join-Path $Game "jpogmod.ini"; $s = [IO.File]::ReadAllText($p)
    foreach ($k in "site_b","all_research","all_dig_sites","max_stars","infinite_money") {
        $s = [regex]::Replace($s, "(?m)^([ \t]*" + $k + "[ \t]*=[ \t]*)\d+", '${1}0')
    }
    [IO.File]::WriteAllText($p, $s); Write-Host "normal start: unlocks / infinite money off in jpogmod.ini"
}
Set-Content $marker "JPOG Co-op v2 installed $(Get-Date -Format s) easy=$([int]$easy)"
$old = Join-Path $Game "dinput8_real.dll"; if (Test-Path $old) { Remove-Item $old; Write-Host "removed obsolete dinput8_real.dll" }

if ($easy) {
# --- 2. Data\FslHunt.ini : all 9 dig sites, no star requirement --------------------------
$p = Join-Path $Game "Data\FslHunt.ini"; Backup $p; $s = Read-Text $p
$s = $s -replace '(?m)^([ \t]*maxchosen[ \t]*=[ \t]*)5;', '${1}9;'
$s = [regex]::Replace($s, '(?s)(#else.*?)(#endif)', {
    param($m)
    $blk = $m.Groups[1].Value -replace '(starrating\[\d\][ \t]*=[ \t]*)\d+;', '${1}0;'
    $nl = if ($blk.Contains("`r`n")) { "`r`n" } else { "`n" }
    for ($i = 0; $i -lt 9; $i++) { if ($blk -notmatch "starrating\[$i\]") { $blk += "`tstarrating[$i] = 0;$nl" } }
    $blk + $m.Groups[2].Value }, 1)
Write-Text $p $s; Write-Host "patched Data\FslHunt.ini (9 dig sites, no star requirement)"

# --- 3. Data\Constant.ini : 5-star start, money, fossil teams ----------------------------
$p = Join-Path $Game "Data\Constant.ini"; Backup $p; $s = Read-Text $p
Set-Key ([ref]$s) "ParkStartEsat" ".96"
Set-Key ([ref]$s) "ParkStartPlayerMoney" "10000000"
Set-Key ([ref]$s) "FossilHuntStartNumTeams" "9"
Set-Key ([ref]$s) "FossilHuntMaxNumTeams" "9"
Write-Text $p $s; Write-Host "patched Data\Constant.ini (5-star start, 10M money, 9 fossil teams)"
} else { Write-Host "normal start: Data\FslHunt.ini / Data\Constant.ini left unchanged" }

# --- 4. Options.ini : native resolution (widescreen) -------------------------------------
$p = Join-Path $Game "Options.ini"
if (Test-Path $p) {
    $vc = $null; try { $vc = Get-CimInstance Win32_VideoController | Where-Object { $_.CurrentHorizontalResolution } | Select-Object -First 1 } catch {}
    $w = if ($vc) { $vc.CurrentHorizontalResolution } else { 1920 }
    $h = if ($vc) { $vc.CurrentVerticalResolution } else { 1080 }
    Backup $p; $s = Read-Text $p
    Set-Key ([ref]$s) "ScreenWidth" "$w"
    Set-Key ([ref]$s) "ScreenHeight" "$h"
    Set-Key ([ref]$s) "ScreenDepth" "32u"
    Set-Key ([ref]$s) "ScreenResolution" "$w"
    Write-Text $p $s; Write-Host "patched Options.ini ($w x $h, 32-bit)"
} else { Write-Warning "Options.ini not found - start the game once, then run install.bat again." }

# --- 5. Data\GUI\main.gui : "Split: Horizontal / Vertical" toggle in the Options popup -------
$p = Join-Path $Game "Data\GUI\main.gui"
if (Test-Path $p) {
    $enc = [Text.Encoding]::GetEncoding(28591)   # latin-1: byte-for-byte
    $s = [IO.File]::ReadAllText($p, $enc)
    if ($s.Contains("JPOGModSplit")) { Write-Host "Data\GUI\main.gui already has the split toggle" }
    else {
        Backup $p
        $a = $s.IndexOf("name = AWindowsOptionsDialog;"); $b = $s.IndexOf("[dialog]", $a)
        if ($a -lt 0 -or $b -lt 0) { Write-Warning "  Options dialog not found in main.gui - toggle skipped" }
        else {
            $blk = $s.Substring($a, $b - $a)
            $c = $blk.IndexOf("`t`tname = Credits;"); $w = $blk.LastIndexOf("`twidget {", $c)
            $nl = "`r`n"
            $widget = "`twidget {$nl`t`ttype = PGUIToggleButton;$nl`t`tplacemode = managed;$nl`t`tsizemode_horiz = fixed;$nl" +
                      "`t`tsizemode_vert = preferred;$nl`t`ttest = true;$nl`t`tname = JPOGModSplit;$nl`t`twidth = 230;$nl" +
                      "`t`tstyle = tFrontEndToggleSlideOnOff;$nl`t`tEnabled = true;$nl`t`tFocused = false;$nl`t`tVisible = true;$nl" +
                      "`t`tRollover = false;$nl`t`tSelected = false;$nl`t`tPressed = false;$nl`t`tToggled = false;$nl" +
                      "`t`tLABEL_OFF = `"Split: Horizontal`";$nl`t`tLABEL_ON = `"Split: Vertical`";$nl`t}$nl"
            $blk = $blk.Insert($w, $widget)
            $i = $blk.IndexOf("height = 300;"); if ($i -ge 0) { $blk = $blk.Remove($i, 13).Insert($i, "height = 350;") }
            $blk = $blk.Replace("`tControls[South] = Credits;", "`tControls[South] = JPOGModSplit;")
            $blk = $blk.Replace("`tCredits[North] = Controls;", "`tCredits[North] = JPOGModSplit;$nl`tJPOGModSplit[North] = Controls;$nl`tJPOGModSplit[South] = Credits;")
            $s = $s.Substring(0, $a) + $blk + $s.Substring($b)
            [IO.File]::WriteAllText($p, $s, $enc); Write-Host "patched Data\GUI\main.gui (Options: Split Horizontal/Vertical toggle)"
        }
    }
}

# --- 6. Data\GUI\main.gui : Load Game screen usable with the controller (JPOG33) --------------
# The PC Load Game dialog (LoadSaveGame_WIN) had no default focus and an empty focus map, so arrow keys /
# the pad could not reach the save list. Give it the same default focus as the console version + a focus map.
$p = Join-Path $Game "Data\GUI\main.gui"
if (Test-Path $p) {
    $enc = [Text.Encoding]::GetEncoding(28591)
    $s = [IO.File]::ReadAllText($p, $enc)
    $a = $s.IndexOf("name = LoadSaveGame_WIN;"); $b = if ($a -ge 0) { $s.IndexOf("[dialog]", $a) } else { -1 }
    if ($a -lt 0 -or $b -lt 0) { Write-Warning "  Load Game dialog not found in main.gui - controller fix skipped" }
    else {
        $blk = $s.Substring($a, $b - $a)
        if ($blk.Contains("GameList[South] = SelectButton;")) { Write-Host "Data\GUI\main.gui already has the Load Game controller fix" }
        else {
            $nl = "`r`n"; $old = "}$nl" + "focusmap {$nl}$nl"; $i = $blk.LastIndexOf($old)
            if ($i -lt 0) { Write-Warning "  Load Game focus map not in the expected form - controller fix skipped" }
            else {
                Backup $p
                $new = "}$nl" + "default_focus = GameList;$nl" + "focusmap {$nl" +
                       "`tGameList[South] = SelectButton;$nl`tGameList[East] = SelectButton;$nl" +
                       "`tSelectButton[North] = GameList;$nl`tSelectButton[West] = GameList;$nl`tSelectButton[East] = DeleteButton;$nl" +
                       "`tDeleteButton[North] = GameList;$nl`tDeleteButton[West] = SelectButton;$nl}$nl"
                $blk = $blk.Substring(0, $i) + $new + $blk.Substring($i + $old.Length)
                $s = $s.Substring(0, $a) + $blk + $s.Substring($b)
                [IO.File]::WriteAllText($p, $s, $enc); Write-Host "patched Data\GUI\main.gui (Load Game screen: controller focus)"
            }
        }
    }
}

Write-Host ""
Write-Host "DONE. Play with: Launch JPOG.bat   Remove with: uninstall.bat"
