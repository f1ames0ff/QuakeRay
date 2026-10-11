param(
    [string[]]$Demos = @('ad_particle_heavy', 'ad_particle_idle'),
    [string[]]$Arms = @('points_on', 'points_off'),
    [string[]]$ExtraCvars = @(),
    [string]$Basedir = '',
    [int]$TimeoutSeconds = 170
)

$ErrorActionPreference = 'Stop'

if (-not $Basedir) {
    $Basedir = (Resolve-Path (Join-Path $PSScriptRoot '..\..\build\Debug')).Path
}

$exe = Join-Path $Basedir 'quakeray.exe'
$gameDir = Join-Path $Basedir 'ad'
$bench = Join-Path $gameDir 'benchmark.log'
$stderrFile = Join-Path $Basedir 'qr_points_demo_stderr.txt'

if (-not (Test-Path $exe)) { throw "engine not found: $exe" }

$running = Get-Process quakeray -ErrorAction SilentlyContinue
if ($running) { throw ("another instance is running: pid " + ($running.Id -join ', ')) }

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public class PointsDemoWin32 {
    [DllImport("user32.dll")] public static extern IntPtr PostMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
    public static void Key(IntPtr h, int vk, int sc, bool up) {
        int lp = 1 | (sc << 16);
        if (up) lp |= unchecked((int)0xC0000000);
        PostMessage(h, up ? 0x101u : 0x100u, (IntPtr)vk, (IntPtr)lp);
    }
}
"@

$env:SDL_VIDEO_MINIMIZE_ON_FOCUS_LOSS = '0'

if (-not $Arms) { throw 'no arm selected' }

$benchFrom = if (Test-Path $bench) { @(Get-Content $bench).Count } else { 0 }

foreach ($demo in $Demos) {
    foreach ($armName in @('points_on', 'points_off')) {
        if ($Arms -notcontains $armName) { continue }
        $armValue = if ($armName -eq 'points_on') { 1 } else { 0 }
        $cfgName = "qr_gate_$armName.cfg"
        $cfgPath = Join-Path $gameDir $cfgName
        $cfgText = @"
r_particles 2
r_particle_lighting 1
r_fteparticles 1
r_smoke 1
rt_particle_resolve_cache 1
r_particles_points $armValue
$($ExtraCvars -join "`n")
bind F9 "menu_main"
rt_bench $demo quit
"@
        Set-Content -Path $cfgPath -Value $cfgText
        if (Test-Path $stderrFile) { Remove-Item $stderrFile -Force }

        Write-Host ("=== {0} / {1} ===" -f $demo, $armName)
        $p = Start-Process -FilePath $exe -ArgumentList '-basedir', $Basedir, '-game', 'ad', '+exec', $cfgName -WorkingDirectory $Basedir -RedirectStandardError $stderrFile -PassThru
        Write-Host ("started pid {0}" -f $p.Id)

        $started = Get-Date
        try {
            $hwnd = [IntPtr]::Zero
            $deadline = (Get-Date).AddSeconds(60)
            while ((Get-Date) -lt $deadline) {
                $p.Refresh()
                if ($p.HasExited) { throw 'game exited during startup' }
                if ($p.MainWindowHandle -ne 0) { $hwnd = $p.MainWindowHandle; break }
                Start-Sleep -Seconds 1
            }
            if ($hwnd -eq [IntPtr]::Zero) { throw 'no game window' }

            [PointsDemoWin32]::ShowWindow($hwnd, 5) | Out-Null
            [PointsDemoWin32]::SetForegroundWindow($hwnd) | Out-Null
            Start-Sleep -Seconds 2
            [PointsDemoWin32]::Key($hwnd, 0x78, 0x43, $false)
            Start-Sleep -Milliseconds 60
            [PointsDemoWin32]::Key($hwnd, 0x78, 0x43, $true)
            Start-Sleep -Milliseconds 400
            [PointsDemoWin32]::Key($hwnd, 0x1B, 0x01, $false)
            Start-Sleep -Milliseconds 60
            [PointsDemoWin32]::Key($hwnd, 0x1B, 0x01, $true)

            $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
            while ((Get-Date) -lt $deadline) {
                $p.Refresh()
                if ($p.HasExited) { break }
                Start-Sleep -Seconds 2
            }
            $p.Refresh()
            if (-not $p.HasExited) {
                Write-Warning ("{0} / {1}: timed out after {2}s, killing" -f $demo, $armName, $TimeoutSeconds)
                Stop-Process -Id $p.Id -Force
            }
        } finally {
            $p.Refresh()
            if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
        }

        $elapsed = [Math]::Round(((Get-Date) - $started).TotalSeconds, 1)
        Write-Host ("{0} / {1}: finished in {2}s" -f $demo, $armName, $elapsed)
        Start-Sleep -Seconds 3
    }
}

Write-Host ''
Write-Host 'new benchmark blocks:'
$lines = Get-Content $bench
$cut = [Math]::Min($benchFrom, $lines.Count)
if ($cut -lt $lines.Count) {
    foreach ($line in $lines[$cut..($lines.Count - 1)]) {
        if ($line -match '^#\s*rt_bench' -or
            $line -match '^cpu\.slot\s+(frame|particles sim|particles resolve|particles fill|particles upload|fte convert)\s' -or
            $line -match '^cpu\.main') {
            Write-Host $line
        }
    }
}

Write-Host 'points demo A/B run finished'
