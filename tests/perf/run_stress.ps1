param(
    [ValidateSet('qr_fuma_start', 'qr_ad_start', 'qr_gpu_heavy')][string[]]$Saves = @('qr_fuma_start'),
    [ValidateSet('balanced', 'quality')][string[]]$Presets = @('balanced'),
    [ValidateRange(2, 25)][int]$Seconds = 10,
    [ValidateRange(2, 30)][int]$Warmup = 5,
    [ValidateRange(1, 2)][int]$Repeats = 1,
    [ValidateRange(30, 300)][int]$MaxRunSeconds = 300,
    [string]$Basedir = '',
    [string]$Tag = 'baseline',
    [string[]]$Overrides = @(),
    [ValidateRange(0, 3)][int]$StatsLevel = 0,
    [switch]$NoSound,
    [switch]$Screenshot
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'stress_budget.ps1')
. (Join-Path $PSScriptRoot 'machine_guard.ps1')
if ($Saves.Count -ne 1 -or $Presets.Count -ne 1) {
    throw 'Run one save and one preset per invocation; release the GPU between short runs.'
}
$script:RunDeadline = $null
$shutdownReserve = 15
$minimumCaptureSeconds = $Warmup + $Seconds + $shutdownReserve
if ($minimumCaptureSeconds -gt $MaxRunSeconds) {
    throw 'Warmup, capture and shutdown reserve cannot fit within MaxRunSeconds.'
}
$mutex = Enter-QuakeRayMachine
try {
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
    MaxRunSeconds = $MaxRunSeconds
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
$inventoryPython = Join-Path $PSScriptRoot '..\..\tools\quakeray_mcp\.venv\Scripts\python.exe'
if (Test-Path -LiteralPath $inventoryPython) {
    $inventoryJson = & $inventoryPython -B -m quakeray_mcp.asset_inventory $Basedir
    if ($LASTEXITCODE -ne 0) { throw 'Asset inventory failed; refusing an unverified run.' }
    $inventoryPath = Join-Path $output 'assets.json'
    [IO.File]::WriteAllText($inventoryPath, ($inventoryJson -join "`n"), [Text.UTF8Encoding]::new($false))
    $manifest.AssetInventorySha256 = (Get-FileHash $inventoryPath -Algorithm SHA256).Hash
    $manifest.AssetInventoryPolicy = 1
    $manifest | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $output 'manifest.json') -Encoding UTF8
}

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
    $deadline = Get-StressDeadline -Deadline $script:RunDeadline -MaximumSeconds 300
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
            Where-Object { $_.ProcessId -notin $ancestors -and (Test-StressExternalRunnerOwner $_.CommandLine) })
        if ($busy.Count -eq 0 -and $runners.Count -eq 0) { return }
        if (-not $reported) {
            Write-Host "Waiting for GPU/build owners: $($busy.Id -join ', '); runners: $($runners.ProcessId -join ', ')"
            $reported = $true
        }
        if ((Get-Date) -ge $deadline) {
            throw "Another agent still owns the machine after 5 minutes or the shared deadline expired; defer this run: $($busy.Id -join ', '); runners $($runners.ProcessId -join ', ')."
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
    $deadline = $script:RunDeadline
    while ((Get-Date) -lt $deadline) {
        if (Test-QuakeRayStopRequested) { throw 'CANCELLED: stop requested while awaiting runtime readiness.' }
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

$results = @()
    foreach ($save in $Saves) {
        foreach ($preset in $Presets) {
            for ($repeat = 1; $repeat -le $Repeats; $repeat++) {
                if (-not (Test-StressBudget -Deadline $script:RunDeadline -RequiredSeconds $minimumCaptureSeconds)) {
                    Write-Host 'Runtime budget reached; remaining repeats are deferred.'
                    break
                }
                Wait-Idle
                if (-not (Test-StressBudget -Deadline $script:RunDeadline -RequiredSeconds $minimumCaptureSeconds)) {
                    Write-Host 'Runtime budget reached; remaining repeats are deferred.'
                    break
                }
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
restart
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
                    if (-not (Test-StressBudget -Deadline $script:RunDeadline -RequiredSeconds $minimumCaptureSeconds)) {
                        Write-Host 'Preparation consumed the capture budget; remaining repeats are deferred.'
                        break
                    }
                    if (-not $script:RunDeadline) { $script:RunDeadline = (Get-Date).AddSeconds($MaxRunSeconds) }
                    $process = Start-Process @launch
                    $null = $process.Handle
                    Wait-Marker $process "QR_LOADED_$id"
                    $process.Refresh()
                    $window = $process.MainWindowHandle
                    if ($window -eq [IntPtr]::Zero) { throw 'No runtime window' }
                    if (-not (Test-StressBudget -Deadline $script:RunDeadline -RequiredSeconds $minimumCaptureSeconds)) {
                        Write-Host 'Loading consumed the capture budget; this attempt is deferred.'
                        Send-Key $window 0x7a 0x57
                        $null = $process.WaitForExit((Get-StressExitTimeout -Deadline $script:RunDeadline))
                        break
                    }
                    [StressWin32]::SetForegroundWindow($window) | Out-Null
                    Send-Key $window 0x1B 0x01
                    Start-Sleep -Seconds $Warmup
                    Send-Key $window 0x76 0x41
                    Wait-Marker $process "QR_EFFECTIVE_$id"
                    if (-not [StressWin32]::OwnsFocus($process.Id)) { throw 'Runtime lacks foreground focus; refusing a contaminated measurement.' }
                    Send-Key $window 0x73 0x3e
                    Wait-Marker $process "QR_START_$id"
                    if (-not (Test-StressBudget -Deadline $script:RunDeadline -RequiredSeconds ($Seconds + $shutdownReserve))) {
                        Write-Host 'Start marker consumed the capture budget; this attempt is deferred.'
                        Send-Key $window 0x74 0x3f
                        Send-Key $window 0x7a 0x57
                        $null = $process.WaitForExit((Get-StressExitTimeout -Deadline $script:RunDeadline))
                        break
                    }
                    $deadline = Get-StressDeadline -Deadline $script:RunDeadline.AddSeconds(-$shutdownReserve) -MaximumSeconds $Seconds
                    $nextOwnerCheck = Get-Date
                    while ((Get-Date) -lt $deadline) {
                        if (Test-QuakeRayStopRequested) { break }
                        if (-not [StressWin32]::OwnsFocus($process.Id)) { throw 'Focus was lost during capture.' }
                        if ((Get-Date) -ge $nextOwnerCheck) {
                            $foreign = @(Get-Process -ErrorAction SilentlyContinue | Where-Object {
                                $_.Id -ne $process.Id -and $_.ProcessName -match '^(quakeray|qray_|cmake$|ninja$|cl$|link$)'
                            })
                            if ($foreign.Count) { throw 'MACHINE_BUSY: foreign heavy activity contaminated this capture.' }
                            $nextOwnerCheck = (Get-Date).AddSeconds(1)
                        }
                        Start-Sleep -Milliseconds 100
                    }
                    Send-Key $window 0x74 0x3f
                    if (-not (Test-QuakeRayStopRequested)) { Wait-Marker $process "QR_STOP_$id" }
                    if ($Screenshot -and (Test-StressBudget -Deadline $script:RunDeadline -RequiredSeconds 10)) {
                        Send-Key $window 0x75 0x40
                        Start-Sleep -Seconds 2
                    }
                    Send-Key $window 0x7a 0x57
                    if (-not $process.WaitForExit((Get-StressExitTimeout -Deadline $script:RunDeadline))) {
                        throw 'Runtime did not exit within the remaining budget'
                    }
                    if ($process.ExitCode -ne 0) { throw "Runtime exit code $($process.ExitCode)" }
                    if (Test-QuakeRayStopRequested) { throw 'CANCELLED: owned runtime stopped; capture remains unaccepted.' }
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
                    if ($Screenshot) {
                        foreach ($image in Get-ChildItem $gameDir -Filter 'screenshot*.png' | Where-Object FullName -notin $oldScreenshots) {
                            Copy-Item $image.FullName (Join-Path $output "$label-$($image.Name)")
                        }
                    }
                    Copy-Item $console (Join-Path $output "$label.console.log")
                    if (-not $manifest.Contains('CaptureChecks')) { $manifest.CaptureChecks = [ordered]@{} }
                    $manifest.CaptureChecks[$label] = @{ FocusVerified = $true; Completed = $true; StatsLevel = $StatsLevel }
                    $manifest | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $output 'manifest.json') -Encoding UTF8
                    $results += [PSCustomObject]@{ Save = $save; Preset = $preset; Repeat = $repeat; Output = $output;
                        Frames = $framePath; Summary = Join-Path $output "$label.summary.json" }
                    Write-Host "Saved $label; runtime closed before analysis."
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
    Exit-QuakeRayMachine $mutex
}
foreach ($capture in $results) {
    python (Join-Path $PSScriptRoot 'analyze_stress.py') $capture.Frames --json $capture.Summary
    if ($LASTEXITCODE -ne 0) { throw 'Capture validation failed; see the frame samples.' }
}
$results | Format-Table -AutoSize
