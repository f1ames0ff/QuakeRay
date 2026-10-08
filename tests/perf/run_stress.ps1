param(
    [ValidateSet('qr_fuma_start', 'qr_ad_start', 'qr_gpu_heavy')][string[]]$Saves = @('qr_fuma_start', 'qr_ad_start', 'qr_gpu_heavy'),
    [ValidateSet('balanced', 'quality')][string[]]$Presets = @('balanced', 'quality'),
    [ValidateRange(2, 25)][int]$Seconds = 10,
    [ValidateRange(2, 30)][int]$Warmup = 5,
    [ValidateRange(1, 5)][int]$Repeats = 1,
    [string]$Basedir = '',
    [string]$Tag = 'baseline',
    [string[]]$Overrides = @(),
    [ValidateRange(0, 3)][int]$StatsLevel = 0,
    [switch]$NoSound,
    [switch]$Screenshot
)

$ErrorActionPreference = 'Stop'
if (-not $Basedir) { $Basedir = Join-Path $PSScriptRoot '..\..\build\Debug' }
$Basedir = (Resolve-Path $Basedir).Path
$gameDir = Join-Path $Basedir 'ad'
$exe = Join-Path $Basedir 'quakeray.exe'
$console = Join-Path $Basedir 'qconsole.log'
$config = Join-Path $gameDir 'config.cfg'
$expectedMaps = @{ qr_fuma_start = 'ad_tfuma'; qr_ad_start = 'start'; qr_gpu_heavy = 'ad_swampy' }
$runTag = ($Tag -replace '[^a-zA-Z0-9_-]', '_') + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' +
    [Guid]::NewGuid().ToString('N').Substring(0, 6)
$output = (New-Item -ItemType Directory -Path (Join-Path $Basedir "audit-$runTag")).FullName

if (-not (Test-Path $exe)) { throw "Missing Debug runtime: $exe" }
foreach ($save in $Saves) {
    if (-not (Test-Path (Join-Path $gameDir "$save.sav"))) { throw "Missing save: $save" }
}

$manifest = [ordered]@{
    Timestamp = (Get-Date).ToString('o')
    Revision = (git -C (Join-Path $PSScriptRoot '..\..') rev-parse HEAD)
    WorkingChanges = @(git -C (Join-Path $PSScriptRoot '..\..') status --short)
    Executable = $exe
    ExecutableSha256 = (Get-FileHash $exe -Algorithm SHA256).Hash
    EngineAssetsSha256 = (Get-FileHash (Join-Path $Basedir 'id1\qray.pkz') -Algorithm SHA256).Hash
    Saves = $Saves
    Presets = $Presets
    Seconds = $Seconds
    Warmup = $Warmup
    Repeats = $Repeats
    StatsLevel = $StatsLevel
    NoSound = [bool]$NoSound
    Overrides = $Overrides
    LoaderLayers = $env:VK_INSTANCE_LAYERS
    LoaderLayersDisabled = $env:VK_LOADER_LAYERS_DISABLE
    Assets = @()
}
foreach ($asset in @('pak0.pak', 'pak1.pak', 'pak2.pak', 'qray.materials.yaml', 'qray.lights.yaml') +
    @($Saves | ForEach-Object { "$_.sav" })) {
    $path = Join-Path $gameDir $asset
    if (Test-Path $path) {
        $manifest.Assets += [ordered]@{ File = $asset; Sha256 = (Get-FileHash $path -Algorithm SHA256).Hash }
    }
}
$manifest | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $output 'manifest.json') -Encoding UTF8

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class StressWin32 {
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr window, uint message, IntPtr key, IntPtr data);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    public static bool OwnsFocus(int process) {
        uint owner;
        GetWindowThreadProcessId(GetForegroundWindow(), out owner);
        return owner == process;
    }
    public static void Key(IntPtr window, int key, int scan, bool up) {
        int data = 1 | (scan << 16);
        if (up) data |= unchecked((int)0xc0000000);
        PostMessage(window, up ? 0x101u : 0x100u, (IntPtr)key, (IntPtr)data);
    }
}
"@

function Wait-Idle {
    $deadline = (Get-Date).AddMinutes(30)
    $pattern = 'run_' + '(menu|place|stress|audit)'
    $ancestors = @($PID)
    $ancestor = Get-CimInstance Win32_Process -Filter "ProcessId=$PID"
    while ($ancestor -and $ancestor.ParentProcessId -gt 0 -and $ancestor.ParentProcessId -notin $ancestors) {
        $ancestors += $ancestor.ParentProcessId
        $ancestor = Get-CimInstance Win32_Process -Filter "ProcessId=$($ancestor.ParentProcessId)"
    }
    $reported = $false
    while ($true) {
        $busy = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -match '^(quakeray|qray_|cmake$|ninja$|cl$|link$)' })
        $runners = @(Get-CimInstance Win32_Process -Filter "Name='powershell.exe' OR Name='pwsh.exe'" |
            Where-Object { $_.ProcessId -notin $ancestors -and $_.CommandLine -match $pattern })
        if ($busy.Count -eq 0 -and $runners.Count -eq 0) { return }
        if (-not $reported) {
            Write-Host "Waiting for GPU/build owners: $($busy.Id -join ', '); runners: $($runners.ProcessId -join ', ')"
            $reported = $true
        }
        if ((Get-Date) -ge $deadline) {
            throw "Another agent still owns the machine after 30 minutes: $($busy.Id -join ', '); runners $($runners.ProcessId -join ', ')."
        }
        Start-Sleep -Seconds 10
    }
}

function Send-Key([IntPtr]$Window, [int]$Key, [int]$Scan) {
    [StressWin32]::Key($Window, $Key, $Scan, $false)
    Start-Sleep -Milliseconds 60
    [StressWin32]::Key($Window, $Key, $Scan, $true)
    Start-Sleep -Milliseconds 250
}

function Wait-Marker($Process, [string]$Marker) {
    $deadline = (Get-Date).AddMinutes(4)
    while ((Get-Date) -lt $deadline) {
        $Process.Refresh()
        if ($Process.HasExited) { throw "Runtime exited with code $($Process.ExitCode) before $Marker" }
        if ($script:CurrentStderr -and (Test-Path $script:CurrentStderr)) {
            $failure = Get-Content $script:CurrentStderr -Raw
            if ($failure -match 'Vulkan call failed|FATAL|Sys_Error') { throw "Runtime startup error: $failure" }
        }
        if ((Test-Path $console) -and (Get-Content $console -Raw) -match [regex]::Escape($Marker)) { return }
        Start-Sleep -Milliseconds 250
    }
    throw "Timed out waiting for $Marker"
}

$mutex = New-Object System.Threading.Mutex($false, 'Local\QuakeRayPerformanceRun')
$locked = $false
$results = @()
try {
    try { $locked = $mutex.WaitOne([TimeSpan]::FromMinutes(30)) }
    catch [System.Threading.AbandonedMutexException] { $locked = $true }
    if (-not $locked) { throw 'Performance-run mutex was not available within 30 minutes.' }
    foreach ($save in $Saves) {
        foreach ($preset in $Presets) {
            for ($repeat = 1; $repeat -le $Repeats; $repeat++) {
                Wait-Idle
                $id = [Guid]::NewGuid().ToString('N').Substring(0, 8)
                $fixture = Join-Path $gameDir "qs_$id.cfg"
                $label = "$save-$preset-$repeat"
                $savedConfig = if (Test-Path $config) { [System.IO.File]::ReadAllBytes($config) } else { $null }
                $process = $null
                $oldDumps = @(Get-ChildItem $gameDir -Filter 'stats-*.dump' | ForEach-Object FullName)
                $oldScreenshots = @(Get-ChildItem $gameDir -Filter 'screenshot*.png' | ForEach-Object FullName)
                $savedMinimize = $env:SDL_VIDEO_MINIMIZE_ON_FOCUS_LOSS
                $bench = Join-Path $gameDir 'benchmark.log'
                $oldBench = if (Test-Path $bench) { @(Get-Content $bench).Count } else { 0 }
                $fsr = if ($preset -eq 'balanced') { 3 } else { 2 }
                $startDump = if ($StatsLevel -gt 0) { 'rt_stats_dump_start;' } else { '' }
                $endDump = if ($StatsLevel -gt 0) { 'rt_stats_dump_end;' } else { '' }
                $fixtureText = (Get-Content (Join-Path $PSScriptRoot 'qr_audit_max.cfg') -Raw) + "`n" +
                    "rt_upscale_fsr31 $fsr`n" + ($Overrides -join "`n") + "`n" + @"
vid_unlock
vid_fullscreen 0
vid_width 3840
vid_height 2160
vid_restart
rt_stats $StatsLevel
bind F4 "rt_bench start; $startDump echo QR_START_$id"
bind F5 "rt_bench stop; $endDump echo QR_STOP_$id"
bind F6 "screenshot"
bind F7 "mapname; vid_describecurrentmode; echo QR_EFFECTIVE_$id"
bind F11 "toggleconsole; quit"
load $save
echo QR_LOADED_$id
"@
                $fixtureText | Set-Content $fixture -Encoding Ascii
                Copy-Item $fixture (Join-Path $output "$label.cfg")
                try {
                    $arguments = @('-basedir', '.', '-game', 'ad', '-window', '-width', 1280, '-height', 720,
                        '-condebug', '+exec', "qs_$id.cfg")
                    if ($NoSound) { $arguments += '-nosound' }
                    $env:SDL_VIDEO_MINIMIZE_ON_FOCUS_LOSS = '0'
                    $launch = @{
                        FilePath = $exe
                        ArgumentList = $arguments
                        WorkingDirectory = $Basedir
                        PassThru = $true
                        RedirectStandardOutput = Join-Path $output "$label.stdout.log"
                        RedirectStandardError = Join-Path $output "$label.stderr.log"
                    }
                    $script:CurrentStderr = $launch.RedirectStandardError
                    $process = Start-Process @launch
                    $null = $process.Handle
                    Wait-Marker $process "QR_LOADED_$id"
                    $process.Refresh()
                    $window = $process.MainWindowHandle
                    if ($window -eq [IntPtr]::Zero) { throw 'No runtime window' }
                    [StressWin32]::SetForegroundWindow($window) | Out-Null
                    Start-Sleep -Seconds $Warmup
                    Send-Key $window 0x76 0x41
                    Wait-Marker $process "QR_EFFECTIVE_$id"
                    if (-not [StressWin32]::OwnsFocus($process.Id)) { throw 'Runtime lacks foreground focus; refusing a contaminated measurement.' }
                    Send-Key $window 0x73 0x3e
                    Wait-Marker $process "QR_START_$id"
                    $deadline = (Get-Date).AddSeconds($Seconds)
                    while ((Get-Date) -lt $deadline) {
                        if (-not [StressWin32]::OwnsFocus($process.Id)) { throw 'Focus was lost during capture.' }
                        Start-Sleep -Milliseconds 100
                    }
                    Send-Key $window 0x74 0x3f
                    Wait-Marker $process "QR_STOP_$id"
                    Start-Sleep -Seconds 1
                    if ($StatsLevel -gt 0) {
                        $dump = Get-ChildItem $gameDir -Filter 'stats-*.dump' | Where-Object FullName -notin $oldDumps |
                            Sort-Object LastWriteTime | Select-Object -Last 1
                        if (-not $dump) { throw 'No stats dump was generated' }
                        Copy-Item $dump.FullName (Join-Path $output "$label.dump")
                    }
                    $blocks = @(Get-Content $bench)
                    $newBlock = $blocks[$oldBench..($blocks.Count - 1)]
                    $newBlock | Set-Content (Join-Path $output "$label.bench.log")
                    $settings = $newBlock | Where-Object { $_ -match '^settings ' } | Select-Object -Last 1
                    if ($settings -notmatch 'vid=3840x2160@' -or $settings -notmatch "rt_upscale_fsr31=$fsr(?: |$)" -or
                        $settings -notmatch 'rt_sky_clouds_quality=0(?: |$)') { throw 'Effective resolution/FSR/cloud quality does not match the requested target.' }
                    if (($newBlock -join "`n") -notmatch "demo=maps/$($expectedMaps[$save])\.bsp") { throw 'Unexpected map in benchmark capture' }
                    $sampleRecord = $newBlock | Where-Object { $_ -match '^frame_samples file=' } | Select-Object -Last 1
                    if ($sampleRecord -notmatch '^frame_samples file=(\S+) samples=(\d+) dropped=0$') {
                        throw 'Per-frame benchmark samples are missing or truncated; rebuild the audit instrumentation.'
                    }
                    $samplePath = Join-Path $gameDir $Matches[1]
                    $framePath = Join-Path $output "$label.frames.csv"
                    Copy-Item $samplePath $framePath
                    python (Join-Path $PSScriptRoot 'analyze_stress.py') $framePath --json (Join-Path $output "$label.summary.json")
                    if ($LASTEXITCODE -ne 0) { throw 'Capture validation failed; see the frame samples.' }
                    if ($Screenshot) {
                        Send-Key $window 0x75 0x40
                        Start-Sleep -Seconds 2
                        foreach ($image in Get-ChildItem $gameDir -Filter 'screenshot*.png' | Where-Object FullName -notin $oldScreenshots) {
                            Copy-Item $image.FullName (Join-Path $output "$label-$($image.Name)")
                        }
                    }
                    Send-Key $window 0x7a 0x57
                    if (-not $process.WaitForExit(15000)) { throw 'Runtime did not exit cleanly' }
                    if ($process.ExitCode -ne 0) { throw "Runtime exit code $($process.ExitCode)" }
                    Copy-Item $console (Join-Path $output "$label.console.log")
                    $results += [PSCustomObject]@{ Save = $save; Preset = $preset; Repeat = $repeat; Output = $output }
                    Write-Host "Captured $label"
                } finally {
                    if ($process -and -not $process.HasExited) { Stop-Process -Id $process.Id -Force }
                    if (Test-Path $console) { Copy-Item $console (Join-Path $output "$label.console.log") }
                    $env:SDL_VIDEO_MINIMIZE_ON_FOCUS_LOSS = $savedMinimize
                    if ($null -ne $savedConfig) { [System.IO.File]::WriteAllBytes($config, $savedConfig) }
                    elseif (Test-Path $config) { Remove-Item $config }
                    Remove-Item $fixture
                }
            }
        }
    }
} finally {
    if ($locked) { $mutex.ReleaseMutex() }
    $mutex.Dispose()
}
$results | Format-Table -AutoSize
