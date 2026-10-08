param(
    [string]$Save = 'start1',
    [int]$FireSeconds = 5,
    [int]$WarmupSeconds = 3,
    [string]$Basedir = ''
)

$ErrorActionPreference = 'Stop'

if (-not $Basedir) {
    $Basedir = (Resolve-Path (Join-Path $PSScriptRoot '..\..\build\Debug')).Path
}

$exe = Join-Path $Basedir 'quakeray.exe'
$gameDir = Join-Path $Basedir 'ad'
$cfg = Join-Path $gameDir 'config.cfg'
$cfgBak = Join-Path $gameDir 'config.pointsabbak.cfg'
$console = Join-Path $Basedir 'qconsole.log'
$bench = Join-Path $gameDir 'benchmark.log'
$stderrFile = Join-Path $Basedir 'qr_points_ab_stderr.txt'
$runCfg = Join-Path $gameDir 'qr_points_ab.cfg'

if (-not (Test-Path $exe)) { throw "engine not found: $exe" }

$savePath = $null
foreach ($candidate in @((Join-Path $gameDir ($Save + '.sav')), (Join-Path $gameDir $Save), $Save)) {
    if (Test-Path $candidate) { $savePath = (Resolve-Path $candidate).Path; break }
}
if (-not $savePath) { throw "save not found: $Save (expected $gameDir\$Save.sav)" }
$saveName = [System.IO.Path]::GetFileNameWithoutExtension($savePath)

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class PointsAbWin32 {
    [DllImport("user32.dll")] public static extern IntPtr PostMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
    public static void Key(IntPtr h, int vk, int sc, bool up) {
        int lp = 1 | (sc << 16);
        if (up) lp |= unchecked((int)0xC0000000);
        PostMessage(h, up ? 0x101u : 0x100u, (IntPtr)vk, (IntPtr)lp);
    }
}
"@

$keys = @{ 'F1' = @(0x70, 0x3B); 'F2' = @(0x71, 0x3C); 'F4' = @(0x73, 0x3E); 'F5' = @(0x74, 0x3F); 'F6' = @(0x75, 0x40); 'F7' = @(0x76, 0x41); 'F9' = @(0x78, 0x43) }

function Send-Key([IntPtr]$hwnd, [string]$name) {
    $vk, $sc = $keys[$name]
    [PointsAbWin32]::Key($hwnd, $vk, $sc, $false)
    Start-Sleep -Milliseconds 60
    [PointsAbWin32]::Key($hwnd, $vk, $sc, $true)
    Start-Sleep -Milliseconds 250
}

function Send-Raw([IntPtr]$hwnd, [int]$vk, [int]$sc) {
    [PointsAbWin32]::Key($hwnd, $vk, $sc, $false)
    Start-Sleep -Milliseconds 60
    [PointsAbWin32]::Key($hwnd, $vk, $sc, $true)
    Start-Sleep -Milliseconds 250
}

function Count-Text([string]$path, [string]$text) {
    try {
        $m = Select-String -Path $path -Pattern $text -SimpleMatch -ErrorAction SilentlyContinue
        if ($null -eq $m) { return 0 }
        return ($m | Measure-Object).Count
    } catch { return 0 }
}

function Wait-Text($process, [int]$seconds, [string]$text, [int]$from) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) {
        $process.Refresh()
        if ($process.HasExited) { return -1 }
        $n = Count-Text $console $text
        if ($n -gt $from) { return $n }
        Start-Sleep -Seconds 1
    }
    return -2
}

function Snapshot-Dump([string]$target) {
    $dump = Get-ChildItem (Join-Path $gameDir 'stats-*.dump') -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1
    if (-not $dump) { return $false }
    Copy-Item $dump.FullName (Join-Path $gameDir $target) -Force
    return $true
}

$cfgText = @"
vid_vsync 0
host_maxfps 0
r_particles 2
r_particle_lighting 1
r_smoke 1
r_fteparticles 1
rt_stats 3
rt_stats_interval 0.2
load $saveName
echo ==== SAVE_LOADED
bind F1 "r_particles_points 1; echo ==== POINTS_ON"
bind F2 "r_particles_points 0; echo ==== POINTS_OFF"
bind F4 "rt_stats_dump_start; rt_bench start; echo ==== ARM_START"
bind F5 "give 7 1; give 6 1; give r 50; impulse 7; echo ==== RL_SELECTED"
bind F6 "+attack; echo ==== FIRE"
bind F7 "-attack; rt_bench stop; rt_stats_dump_end; echo ==== ARM_DONE"
bind F9 "quit"
"@
Set-Content -Path $runCfg -Value $cfgText

if (Test-Path $cfg) { Copy-Item $cfg $cfgBak -Force }
if (Test-Path $stderrFile) { Remove-Item $stderrFile -Force }
if (Test-Path $console) { Remove-Item $console -Force }
foreach ($arm in @('stage2-points-on.dump', 'stage2-points-off.dump')) { Remove-Item (Join-Path $gameDir $arm) -Force -ErrorAction SilentlyContinue }

$benchFrom = if (Test-Path $bench) { @(Get-Content $bench).Count } else { 0 }

$env:SDL_VIDEO_MINIMIZE_ON_FOCUS_LOSS = '0'
$p = Start-Process -FilePath $exe -ArgumentList '-basedir', $Basedir, '-game', 'ad', '-condebug', '+exec', 'qr_points_ab.cfg' -WorkingDirectory $Basedir -RedirectStandardError $stderrFile -PassThru
Write-Host ("started pid {0}" -f $p.Id)

try {
    $hwnd = [IntPtr]::Zero
    $deadline = (Get-Date).AddSeconds(150)
    while ((Get-Date) -lt $deadline) {
        $p.Refresh()
        if ($p.HasExited) { throw 'game exited during startup' }
        if ($p.MainWindowHandle -ne 0) { $hwnd = $p.MainWindowHandle; break }
        Start-Sleep -Seconds 2
    }
    if ($hwnd -eq [IntPtr]::Zero) { throw 'no game window' }

    $r = Wait-Text $p 150 '==== SAVE_LOADED' 0
    if ($r -le 0) { throw "save not loaded (code $r)" }
    Write-Host 'save loaded'

    Send-Raw $hwnd 0x1B 0x01

    $r = Wait-Text $p 150 'In the beginning' 0
    if ($r -le 0) { throw "world did not spawn (code $r)" }
    Write-Host 'world spawned'
    Start-Sleep -Seconds $WarmupSeconds

    foreach ($arm in @(@{ Switch = 'F1'; Target = 'stage2-points-on.dump';  Name = 'points_on' },
                       @{ Switch = 'F2'; Target = 'stage2-points-off.dump'; Name = 'points_off' })) {
        $before = Count-Text $console '==== ARM_DONE'
        Send-Key $hwnd $arm.Switch
        Start-Sleep -Seconds 1
        Send-Key $hwnd 'F5'
        Start-Sleep -Seconds 1
        $armStartFrom = Count-Text $console '==== ARM_START'
        Send-Key $hwnd 'F4'
        $r = Wait-Text $p 60 '==== ARM_START' $armStartFrom
        if ($r -le 0) { throw "arm $($arm.Name) did not start (code $r)" }
        Send-Key $hwnd 'F6'
        Start-Sleep -Seconds $FireSeconds
        Send-Key $hwnd 'F7'
        $r = Wait-Text $p 120 '==== ARM_DONE' $before
        if ($r -le 0) { throw "arm $($arm.Name) did not finish (code $r)" }
        Start-Sleep -Seconds 2
        if (-not (Snapshot-Dump $arm.Target)) { throw "arm $($arm.Name) wrote no dump" }
        Write-Host ("arm {0}: dump saved as {1}" -f $arm.Name, $arm.Target)
    }

    Send-Key $hwnd 'F9'
    Start-Sleep -Seconds 4
    $p.Refresh()
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
} finally {
    $p.Refresh()
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
    Start-Sleep -Seconds 2
    if (Test-Path $cfgBak) { Copy-Item $cfgBak $cfg -Force; Remove-Item $cfgBak -Force }
}

if (Test-Path $bench) {
    $lines = Get-Content $bench
    $cut = [Math]::Min($benchFrom, $lines.Count)
    if ($cut -lt $lines.Count) {
        Write-Host ''
        Write-Host 'benchmark blocks:'
        foreach ($line in $lines[$cut..($lines.Count - 1)]) {
            if ($line -match '^#\s*rt_bench' -or
                $line -match '^settings' -or
                $line -match '^cpu\.slot\s+(frame|particles sim|particles resolve|particles fill|particles upload|fte convert)\s') {
                Write-Host $line
            }
        }
    }
}

$attribution = Join-Path $PSScriptRoot 'particle_attribution.ps1'
$dumpOn = Join-Path $gameDir 'stage2-points-on.dump'
$dumpOff = Join-Path $gameDir 'stage2-points-off.dump'
if ((Test-Path $attribution) -and (Test-Path $dumpOn) -and (Test-Path $dumpOff)) {
    Write-Host ''
    Write-Host 'attribution (points_on baseline vs points_off):'
    & $attribution -Baseline points_on -Arms @{ points_on = $dumpOn; points_off = $dumpOff }
}

Write-Host 'points A/B run finished'
