param(
    [Parameter(Mandatory = $true)][string]$Save,
    [ValidateSet('gpumax', 'cpumax', 'both')][string]$Mode = 'both',
    [int]$Seconds = 10,
    [int]$Warmup = 3,
    [string]$Tag = '',
    [string]$Basedir = ''
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'machine_guard.ps1')
$machineGuard = Enter-QuakeRayMachine
try {
$configBackedUpHere = $false

if (-not $Basedir) {
    $Basedir = (Resolve-Path (Join-Path $PSScriptRoot '..\..\build\Debug')).Path
}

$exe = Join-Path $Basedir 'quakeray.exe'
$gameDir = Join-Path $Basedir 'ad'
$cfg = Join-Path $gameDir 'config.cfg'
$cfgBak = Join-Path $gameDir 'config.perfbak.cfg'
$console = Join-Path $Basedir 'qconsole.log'
$bench = Join-Path $gameDir 'benchmark.log'
$stderrFile = Join-Path $Basedir 'qr_perf_stderr.txt'

if (-not (Test-Path $exe)) { throw "engine not found: $exe" }

$savePath = $null
foreach ($candidate in @((Join-Path $gameDir ($Save + '.sav')), (Join-Path $gameDir $Save), $Save)) {
    if (Test-Path $candidate) { $savePath = (Resolve-Path $candidate).Path; break }
}
if (-not $savePath) { throw "save not found: $Save (expected $gameDir\$Save.sav)" }
$saveName = [System.IO.Path]::GetFileNameWithoutExtension($savePath)

$modes = if ($Mode -eq 'both') { @('gpumax', 'cpumax') } else { @($Mode) }

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class PerfWin32 {
    [DllImport("user32.dll")] public static extern IntPtr PostMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
    public static void Key(IntPtr h, int vk, int sc, bool up) {
        int lp = 1 | (sc << 16);
        if (up) lp |= unchecked((int)0xC0000000);
        PostMessage(h, up ? 0x101u : 0x100u, (IntPtr)vk, (IntPtr)lp);
    }
}
"@

$keys = @{ 'F4' = @(0x73, 0x3E); 'F5' = @(0x74, 0x3F); 'F6' = @(0x75, 0x40); 'F7' = @(0x76, 0x41); 'F8' = @(0x77, 0x42); 'F9' = @(0x78, 0x43); 'F10' = @(0x79, 0x44) }

function Send-Key([IntPtr]$hwnd, [string]$name) {
    $vk, $sc = $keys[$name]
    [PerfWin32]::Key($hwnd, $vk, $sc, $false)
    Start-Sleep -Milliseconds 60
    [PerfWin32]::Key($hwnd, $vk, $sc, $true)
    Start-Sleep -Milliseconds 250
}

function Send-Raw([IntPtr]$hwnd, [int]$vk, [int]$sc) {
    [PerfWin32]::Key($hwnd, $vk, $sc, $false)
    Start-Sleep -Milliseconds 60
    [PerfWin32]::Key($hwnd, $vk, $sc, $true)
    Start-Sleep -Milliseconds 250
}

function Count-Text([string]$path, [string]$text) {
    try {
        $m = Select-String -Path $path -Pattern $text -SimpleMatch -ErrorAction SilentlyContinue
        if ($null -eq $m) { return 0 }
        return ($m | Measure-Object).Count
    } catch { return 0 }
}

function Show-Blocks([int]$fromLine, [string]$printTag) {
    if (-not (Test-Path $bench)) { Write-Host 'no benchmark.log'; return }
    $lines = Get-Content $bench
    $cut = [Math]::Min($fromLine, $lines.Count)
    if ($cut -ge $lines.Count) { Write-Host 'no new bench blocks'; return }
    $new = $lines[$cut..($lines.Count - 1)]
    if ($new.Count -eq 0) { Write-Host 'no new bench blocks'; return }

    if ($Tag -or $printTag) {
        $out = Join-Path $gameDir ("perf-{0}.log" -f ($printTag -replace '[^a-zA-Z0-9_.-]', '_'))
        Set-Content -Path $out -Value $new
        Write-Host "blocks copied to $out"
    }

    foreach ($line in $new) {
        if ($line -match '^#\s*rt_bench\s+(.*?)\s+demo=(\S+)\s+frames=(\d+)\s+seconds=([0-9.]+)\s+fps=([0-9.]+)\s+interrupted=(\d+)') {
            Write-Host ""
            Write-Host ("== demo={0} frames={1} seconds={2} fps={3} interrupted={4}" -f $Matches[2], $Matches[3], $Matches[4], $Matches[5], $Matches[6])
            continue
        }
        if ($line -match '^settings\s+.*?rt_cluster_sampling=(\d+)') {
            Write-Host ("   sampling={0}" -f $Matches[1])
            continue
        }
        if ($line -match '^cpu\.slot\s+(.+?)\s+avg_ms=([0-9.]+)\s+max_ms=([0-9.]+)') {
            $slotName = $Matches[1].Trim()
            if (@('frame', 'clusters', 'clust lists', 'clust resolve', 'clust vis', 'clust topup', 'clust fill', 'clust tail', 'clust upload') -contains $slotName) {
                Write-Host ("   {0,-12} avg={1,8} max={2,8}" -f $slotName, $Matches[2], $Matches[3])
            }
            continue
        }
        if ($line -match '^cpu\.cluster\s+lists\s+hits=(\d+)\s+misses=(\d+)\s+set=(\d+)\s+move=(\d+)\s+other=(\d+)') {
            Write-Host ("   hits={0} misses={1} set={2} move={3} other={4}" -f $Matches[1], $Matches[2], $Matches[3], $Matches[4], $Matches[5])
            continue
        }
    }
}

if (Test-Path -LiteralPath $cfgBak) { throw 'RECOVERY_REQUIRED: an existing performance backup needs manual review.' }
if (Test-Path $cfg) { Copy-Item $cfg $cfgBak -Force; $configBackedUpHere = $true }

foreach ($m in $modes) {
    $template = Join-Path $PSScriptRoot ("qr_perf_{0}.cfg" -f $m)
    if (-not (Test-Path $template)) { throw "mode config not found: $template" }

    $benchFrom = if (Test-Path $bench) { @(Get-Content $bench).Count } else { 0 }

    $runCfg = (Get-Content $template -Raw) + "`nload $saveName`necho ==== SAVE_LOADED`n" +
              "bind F4 `"rt_cluster_sampling 1; load $saveName; echo ==== RELOAD_S1`"`n" +
              "bind F5 `"rt_cluster_sampling 0; load $saveName; echo ==== RELOAD_S0`"`n"
    Set-Content -Path (Join-Path $gameDir ("qr_perf_{0}.cfg" -f $m)) -Value $runCfg
    Copy-Item (Join-Path $PSScriptRoot 'qr_perf_common.cfg') (Join-Path $gameDir 'qr_perf_common.cfg') -Force

    if (Test-Path $stderrFile) { Remove-Item $stderrFile -Force }
    if (Test-Path $console) { Remove-Item $console -Force }

    $env:SDL_VIDEO_MINIMIZE_ON_FOCUS_LOSS = '0'
    $p = Start-Process -FilePath $exe -ArgumentList '-basedir', $Basedir, '-game', 'ad', '-condebug', '+exec', ("qr_perf_{0}.cfg" -f $m) -WorkingDirectory $Basedir -RedirectStandardError $stderrFile -PassThru
    Write-Host ("[{0}] started pid {1}" -f $m, $p.Id)

    $hwnd = [IntPtr]::Zero
    $deadline = (Get-Date).AddSeconds(150)
    while ((Get-Date) -lt $deadline) {
        $p.Refresh()
        if ($p.HasExited) { throw '[{0}] game exited during startup' -f $m }
        if ($p.MainWindowHandle -ne 0) { $hwnd = $p.MainWindowHandle; break }
        Start-Sleep -Seconds 2
    }
    if ($hwnd -eq [IntPtr]::Zero) { Stop-Process -Id $p.Id -Force; throw 'no game window' }

    $loaded = $false
    $deadline = (Get-Date).AddSeconds(150)
    while ((Get-Date) -lt $deadline) {
        $p.Refresh()
        if ($p.HasExited) { throw '[{0}] game exited while loading the save' -f $m }
        if ((Count-Text $console '==== SAVE_LOADED') -gt 0) { $loaded = $true; break }
        Start-Sleep -Seconds 2
    }
    Write-Host ("[{0}] save loaded: {1}" -f $m, $loaded)
    Start-Sleep -Seconds 2

    Send-Raw $hwnd 0x1B 0x01
    Start-Sleep -Seconds $Warmup

    foreach ($arm in @('F4', 'F5')) {
        $before = Count-Text $console 'In the beginning'
        Send-Key $hwnd $arm

        $deadline = (Get-Date).AddSeconds(150)
        while ((Get-Date) -lt $deadline) {
            $p.Refresh()
            if ($p.HasExited) { throw '[{0}] game exited while loading the save' -f $m }
            if ((Count-Text $console 'In the beginning') -gt $before) { break }
            Start-Sleep -Seconds 1
        }

        Start-Sleep -Seconds $Warmup
        Send-Key $hwnd 'F7'
        Start-Sleep -Seconds $Seconds
        Send-Key $hwnd 'F8'
        Start-Sleep -Seconds 1
    }

    Send-Key $hwnd 'F10'
    Start-Sleep -Seconds 4
    $p.Refresh()
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
    Start-Sleep -Seconds 2

    $label = if ($Tag) { $Tag } else { $saveName }
    Show-Blocks $benchFrom ("{0}-{1}" -f $label, $m)
}

if (Test-Path $cfgBak) { Copy-Item $cfgBak $cfg -Force; Remove-Item $cfgBak -Force }

Write-Host 'perf run finished'
} finally {
    if ($configBackedUpHere -and $cfgBak -and (Test-Path -LiteralPath $cfgBak)) {
        Copy-Item -LiteralPath $cfgBak -Destination $cfg -Force
        Remove-Item -LiteralPath $cfgBak
    }
    Exit-QuakeRayMachine $machineGuard
}
