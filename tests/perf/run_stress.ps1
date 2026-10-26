param(
    [string[]]$Saves = @('qr_fuma_start'),
    [ValidateSet('id1', 'mg1', 'mg3', 'ad')][string]$Game = 'ad',
    [ValidatePattern('^[A-Za-z0-9_./-]+$')][string[]]$Maps = @(),
    [ValidateSet('balanced', 'quality', 'performance', 'ultra')][string[]]$Presets = @('balanced'),
    [ValidateSet('audit', 'medium')][string]$RtProfile = 'audit',
    [ValidateRange(2, 25)][int]$Seconds = 10,
    [ValidateRange(2, 30)][int]$Warmup = 5,
    [ValidateRange(1, 2)][int]$Repeats = 1,
    [ValidateRange(30, 300)][int]$MaxRunSeconds = 300,
    [string]$Basedir = '',
    [ValidateSet(0, 1, 2)][int]$Fullscreen = 0,
    [string]$Tag = 'baseline',
    [string[]]$Overrides = @(),
    [ValidateSet(1, 2, 3)][int]$ExpectedVsync = 1,
    [ValidateSet(1)][int]$ExpectedMaxFrameLatency = 1,
    [ValidateSet(0, 1)][int]$ExpectedRestir = 0,
    [ValidateRange(0, 3)][int]$StatsLevel = 0,
    [switch]$NoSound,
    [switch]$EnableStatsAfterWarmup,
    [switch]$Scheduled,
    [switch]$MoveDuringCapture,
    [switch]$AllowProfileOverrides,
    [switch]$Screenshot
)

$ErrorActionPreference = 'Stop'
if ($EnableStatsAfterWarmup -and $StatsLevel -ne 3) {
    throw 'EnableStatsAfterWarmup requires StatsLevel 3.'
}
if ($Scheduled -and $MoveDuringCapture) {
    throw 'Scheduled captures cannot inject movement input.'
}
if ($Scheduled -and $Screenshot) {
    throw 'Scheduled captures do not take interactive screenshots.'
}
if ($Maps.Count -gt 1) {
    throw 'Run one map per invocation; release the GPU between short runs.'
}
if ($Maps.Count -eq 1) {
    $Saves = @($Maps[0])
} else {
    $knownSaves = @('qr_fuma_start', 'qr_ad_start', 'qr_gpu_heavy', 'qr_swampy_start', 'qr_ad_swamp')
    foreach ($saveName in $Saves) {
        if ($saveName -notin $knownSaves) {
            throw "Unknown save scenario '$saveName'; provide a map through -Maps instead."
        }
    }
}
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
$ancestors = @(Get-QuakeRayProcessAncestorIds)
if (-not $Basedir) { $Basedir = Join-Path $PSScriptRoot '..\..\build\Debug' }
$Basedir = (Resolve-Path $Basedir).Path
$gameDir = Join-Path $Basedir $Game
$exe = Join-Path $Basedir 'quakeray.exe'
$console = Join-Path $Basedir 'qconsole.log'
$config = Join-Path $gameDir 'config.cfg'
$scheduledWarmupFrames = if ($Scheduled) { [int][math]::Ceiling($Warmup * 60.0) } else { 0 }
$scheduledCaptureFrames = if ($Scheduled) { [int][math]::Ceiling($Seconds * 60.0) } else { 0 }
$expectedMaps = @{ qr_fuma_start = 'ad_tfuma'; qr_ad_start = 'start'; qr_gpu_heavy = 'ad_swampy'; qr_swampy_start = 'ad_swampy'; qr_ad_swamp = 'ad_swampy' }
$mapMode = $Maps.Count -eq 1 -or $Saves[0] -eq 'qr_swampy_start'
if ($Maps.Count -eq 1) {
    $expectedMaps[$Saves[0]] = (Split-Path -Leaf $Maps[0]) -replace '\.bsp$', ''
}
$runTag = ($Tag -replace '[^a-zA-Z0-9_-]', '_') + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' +
    [Guid]::NewGuid().ToString('N').Substring(0, 6)
$output = (New-Item -ItemType Directory -Path (Join-Path $Basedir "audit-$runTag")).FullName

if (-not (Test-Path $exe)) { throw "Missing Debug runtime: $exe" }
if (-not (Test-Path $gameDir)) {
    New-Item -ItemType Directory -Path $gameDir | Out-Null
}
if (-not $mapMode) {
    foreach ($save in $Saves) {
        if (-not (Test-Path (Join-Path $gameDir "$save.sav"))) { throw "Missing save: $save" }
    }
}

try {
    $activeDisplays = @(
        Get-CimInstance Win32_VideoController -ErrorAction Stop |
            Where-Object { $_.CurrentHorizontalResolution -gt 0 -and $_.CurrentVerticalResolution -gt 0 } |
            ForEach-Object {
                [ordered]@{
                    Name = $_.Name
                    PnpDeviceId = $_.PNPDeviceID
                    DriverVersion = $_.DriverVersion
                    Width = $_.CurrentHorizontalResolution
                    Height = $_.CurrentVerticalResolution
                    RefreshHz = $_.CurrentRefreshRate
                    VideoModeDescription = $_.VideoModeDescription
                }
            }
    )
} catch {
    $activeDisplays = @(Get-QuakeRayActiveDisplays)
}
if (-not $activeDisplays.Count) {
    throw 'MACHINE_GUARD_UNAVAILABLE: no active display mode was reported.'
}

$manifest = [ordered]@{
    Timestamp = (Get-Date).ToString('o')
    Revision = (git -C (Join-Path $PSScriptRoot '..\..') rev-parse HEAD)
    WorkingChanges = @(git -C (Join-Path $PSScriptRoot '..\..') status --short)
    Executable = $exe
    ExecutableSha256 = (Get-FileHash $exe -Algorithm SHA256).Hash
    EngineAssetsSha256 = (Get-FileHash (Join-Path $Basedir 'id1\qray.pkz') -Algorithm SHA256).Hash
    Game = $Game
    Maps = $Maps
    Saves = $Saves
    Presets = $Presets
    RtProfile = $RtProfile
    Seconds = $Seconds
    Warmup = $Warmup
    Fullscreen = $Fullscreen
    Repeats = $Repeats
    MaxRunSeconds = $MaxRunSeconds
    StatsLevel = $StatsLevel
    NoSound = [bool]$NoSound
    EnableStatsAfterWarmup = [bool]$EnableStatsAfterWarmup
    Scheduled = [bool]$Scheduled
    ScheduledWarmupFrames = $scheduledWarmupFrames
    ScheduledCaptureFrames = $scheduledCaptureFrames
    MoveDuringCapture = [bool]$MoveDuringCapture
    AllowProfileOverrides = [bool]$AllowProfileOverrides
    ExpectedVsync = $ExpectedVsync
    ExpectedMaxFrameLatency = $ExpectedMaxFrameLatency
    ExpectedRestir = $ExpectedRestir
    Overrides = $Overrides
    LoaderLayers = $env:VK_INSTANCE_LAYERS
    LoaderLayersDisabled = $env:VK_LOADER_LAYERS_DISABLE
    ActiveDisplays = $activeDisplays
    Assets = @()
}
$assetNames = @('pak0.pak', 'pak1.pak', 'pak2.pak', 'qray.materials.yaml', 'qray.lights.yaml')
if (-not $mapMode) { $assetNames += @($Saves | ForEach-Object { "$_.sav" }) }
foreach ($asset in $assetNames) {
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
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint attach, uint attachTo, bool connect);
    [DllImport("user32.dll")] public static extern IntPtr SetFocus(IntPtr window);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr window);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr window, int command);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    public static bool OwnsFocus(int process) {
        uint owner;
        GetWindowThreadProcessId(GetForegroundWindow(), out owner);
        return owner == process;
    }
    public static bool ForceFocus(IntPtr window) {
        IntPtr foreground = GetForegroundWindow();
        uint ignored;
        uint foregroundThread = GetWindowThreadProcessId(foreground, out ignored);
        uint currentThread = GetCurrentThreadId();
        bool attached = foregroundThread != 0 && foregroundThread != currentThread &&
            AttachThreadInput(currentThread, foregroundThread, true);
        try {
            ShowWindow(window, 9);
            BringWindowToTop(window);
            SetForegroundWindow(window);
            SetFocus(window);
        }
        finally {
            if (attached) AttachThreadInput(currentThread, foregroundThread, false);
        }
        return GetForegroundWindow() == window;
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
    $reported = $false
    while ($true) {
        $busy = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -match '^(quakeray|qray_|cmake$|ninja$|cl$|link$)' -and $_.Threads.Count -gt 0 })
        $runners = @(Get-QuakeRayExternalRunnerProcesses -ExcludedProcessIds $ancestors |
            Where-Object { Test-StressExternalRunnerOwner $_.CommandLine })
        $gpuBusy = @(Get-QuakeRayGpuBusyProcesses -ExcludedProcessIds $ancestors)
        if ($busy.Count -eq 0 -and $runners.Count -eq 0 -and $gpuBusy.Count -eq 0) { return }
        if (-not $reported) {
            $gpuOwners = ($gpuBusy | ForEach-Object { "$($_.ProcessId):$($_.ProcessName)" }) -join ', '
            Write-Host "Waiting for GPU/build owners: $($busy.Id -join ', '); runners: $($runners.ProcessId -join ', '); GPU: $gpuOwners"
            $reported = $true
        }
        if ((Get-Date) -ge $deadline) {
            $gpuOwners = ($gpuBusy | ForEach-Object { "$($_.ProcessId):$($_.ProcessName)" }) -join ', '
            throw "Another agent still owns the machine after 5 minutes or the shared deadline expired; defer this run: $($busy.Id -join ', '); runners $($runners.ProcessId -join ', '); GPU $gpuOwners."
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
                $label = if ($Maps.Count -eq 1) { "$Game-$($Maps[0])-$preset-$repeat" -replace '[^A-Za-z0-9_.-]', '_' }
                    else { "$save-$preset-$repeat" }
                $savedConfig = if (Test-Path $config) { [System.IO.File]::ReadAllBytes($config) } else { $null }
                $process = $null
                $oldDumps = @(Get-ChildItem $gameDir -Filter 'stats-*.dump' | ForEach-Object FullName)
                $oldScreenshots = @(Get-ChildItem $gameDir -Filter 'screenshot*.png' | ForEach-Object FullName)
                $savedMinimize = $env:SDL_VIDEO_MINIMIZE_ON_FOCUS_LOSS
                $bench = Join-Path $gameDir 'benchmark.log'
                $oldBench = if (Test-Path $bench) { @(Get-Content $bench).Count } else { 0 }
                $fsr = switch ($preset) { 'quality' { 2 } 'balanced' { 3 } 'performance' { 4 } 'ultra' { 5 } }
                $startDump = if ($StatsLevel -gt 0) { 'rt_stats_dump_start;' } else { '' }
                $endDump = if ($StatsLevel -gt 0) { 'rt_stats_dump_end;' } else { '' }
                $initialStatsLevel = if ($EnableStatsAfterWarmup) { 0 } else { $StatsLevel }
                $startStats = if ($EnableStatsAfterWarmup) { "rt_stats $StatsLevel;" } else { '' }
                $mapCommand = if ($mapMode) { "map $($expectedMaps[$save])" } else { "load $save" }
                $profileCfg = if ($RtProfile -eq 'medium') { 'qr_medium.cfg' } else { 'qr_audit_max.cfg' }
                if ($Scheduled) {
                    $warmupWaits = 'wait;' * $scheduledWarmupFrames
                    $captureWaits = 'wait;' * $scheduledCaptureFrames
                    $flushWaits = 'wait;' * 3
                    $startupWaits = 'wait;' * 30
                    $fixtureTail = @"
$startupWaits
$mapCommand
echo QR_LOADED0_$id
mapname
menu_close
echo QR_MAP_$id
$warmupWaits
$startStats rt_bench start; $startDump echo QR_START_$id
$captureWaits
rt_bench stop; $endDump echo QR_STOP_$id
$flushWaits
echo QR_QUIT_$id
toggleconsole; quit
"@
                } else {
                    $fixtureTail = @"
bind F4 "$startStats rt_bench start; $startDump echo QR_START_$id"
bind F5 "rt_bench stop; $endDump echo QR_STOP_$id"
bind F6 "screenshot"
bind F7 "mapname; vid_describecurrentmode; echo QR_EFFECTIVE_$id"
bind F8 "restart; echo QR_RESTART_$id"
bind F9 "load $save; wait; menu_close; echo QR_RELOAD_$id"
bind F11 "toggleconsole; quit"
$mapCommand
echo QR_LOADED0_$id
mapname
echo QR_MAP_$id
"@
                }
                $fixtureText = (Get-Content (Join-Path $PSScriptRoot $profileCfg) -Raw) + "`n" +
                    "rt_upscale_fsr31 $fsr`n" + ($Overrides -join "`n") + "`n" + @"
vid_unlock
vid_fullscreen $Fullscreen
vid_width 3840
vid_height 2160
vid_restart
rt_stats $initialStatsLevel
"@ + "`r`n" + $fixtureTail
                $fixtureText | Set-Content $fixture -Encoding Ascii
                Copy-Item $fixture (Join-Path $output "$label.cfg")
                try {
                    $arguments = @('-basedir', '.', '-game', $Game, '-window', '-width', 1280, '-height', 720,
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
                    Wait-Marker $process "QR_LOADED0_$id"
                    Wait-Marker $process "QR_MAP_$id"
                    $process.Refresh()
                    if ($Scheduled) {
                        $focusLosses = 0
                        Wait-Marker $process "QR_START_$id"
                        $gpuBusy = @(Get-QuakeRayGpuBusyProcesses -ExcludedProcessIds ($ancestors + @($process.Id)))
                        if ($gpuBusy.Count) {
                            $owners = ($gpuBusy | ForEach-Object { "$($_.ProcessId):$($_.ProcessName)" }) -join ', '
                            throw "MACHINE_BUSY: foreign GPU activity contaminated this capture before it started: $owners"
                        }

                        $deadline = Get-StressDeadline -Deadline $script:RunDeadline.AddSeconds(-$shutdownReserve) -MaximumSeconds $MaxRunSeconds
                        $nextOwnerCheck = Get-Date
                        while ($true) {
                            if (Test-QuakeRayStopRequested) { throw 'CANCELLED: owned runtime stopped; capture remains unaccepted.' }
                            $process.Refresh()
                            if ((Test-Path $console) -and (Get-Content $console -Raw) -match [regex]::Escape("QR_STOP_$id")) { break }
                            if ($process.HasExited) { throw "Runtime exited with code $($process.ExitCode) before QR_STOP_$id" }
                            if ((Get-Date) -ge $deadline) { throw "Timed out waiting for QR_STOP_$id" }
                            if ((Get-Date) -ge $nextOwnerCheck) {
                                $foreign = @(Get-Process -ErrorAction SilentlyContinue | Where-Object {
                                    $_.Id -ne $process.Id -and
                                    $_.Threads.Count -gt 0 -and
                                    $_.ProcessName -match '^(quakeray|qray_|cmake$|ninja$|cl$|link$)'
                                })
                                if ($foreign.Count) {
                                    $owners = ($foreign | ForEach-Object { "$($_.Id):$($_.ProcessName)" }) -join ', '
                                    throw "MACHINE_BUSY: foreign heavy activity contaminated this capture: $owners"
                                }
                                $gpuBusy = @(Get-QuakeRayGpuBusyProcesses -ExcludedProcessIds ($ancestors + @($process.Id)))
                                if ($gpuBusy.Count) {
                                    $owners = ($gpuBusy | ForEach-Object { "$($_.ProcessId):$($_.ProcessName)" }) -join ', '
                                    throw "MACHINE_BUSY: foreign GPU activity contaminated this capture: $owners"
                                }
                                $nextOwnerCheck = (Get-Date).AddSeconds(1)
                            }
                            Start-Sleep -Milliseconds 100
                        }
                        $gpuBusy = @(Get-QuakeRayGpuBusyProcesses -ExcludedProcessIds ($ancestors + @($process.Id)))
                        if ($gpuBusy.Count) {
                            $owners = ($gpuBusy | ForEach-Object { "$($_.ProcessId):$($_.ProcessName)" }) -join ', '
                            throw "MACHINE_BUSY: foreign GPU activity contaminated this capture: $owners"
                        }
                        if (-not $process.WaitForExit((Get-StressExitTimeout -Deadline $script:RunDeadline))) {
                            throw 'Runtime did not exit within the remaining budget'
                        }
                        if ($process.ExitCode -ne 0) { throw "Runtime exit code $($process.ExitCode)" }
                    } else {
                    $window = $process.MainWindowHandle
                    if ($window -eq [IntPtr]::Zero) { throw 'No runtime window' }
                    if (-not (Test-StressBudget -Deadline $script:RunDeadline -RequiredSeconds $minimumCaptureSeconds)) {
                        Write-Host 'Loading consumed the capture budget; this attempt is deferred.'
                        Send-Key $window 0x7a 0x57
                        $null = $process.WaitForExit((Get-StressExitTimeout -Deadline $script:RunDeadline))
                        break
                    }
                    [StressWin32]::ForceFocus($window) | Out-Null
                    if (-not $mapMode) {
                        [StressWin32]::ForceFocus($window) | Out-Null
                        Wait-Marker $process 'RHI: acceleration structures:'
                        Start-Sleep -Milliseconds 300
                        Send-Key $window 0x77 0x42
                        Wait-Marker $process "QR_RESTART_$id"
                        Start-Sleep -Milliseconds 800
                        Send-Key $window 0x79 0x43
                        Wait-Marker $process "QR_RELOAD_$id"
                        Start-Sleep -Milliseconds 800
                        if (-not (Test-StressBudget -Deadline $script:RunDeadline -RequiredSeconds $minimumCaptureSeconds)) {
                            Write-Host 'The restart sequence consumed the capture budget; this attempt is deferred.'
                            Send-Key $window 0x7a 0x57
                            $null = $process.WaitForExit((Get-StressExitTimeout -Deadline $script:RunDeadline))
                            break
                        }
                    }
                    Start-Sleep -Seconds $Warmup
                    Send-Key $window 0x76 0x41
                    Wait-Marker $process "QR_EFFECTIVE_$id"
                    for ($focusTry = 0; $focusTry -lt 10; $focusTry++) {
                        if ([StressWin32]::OwnsFocus($process.Id)) { break }
                        [StressWin32]::ForceFocus($window) | Out-Null
                        Start-Sleep -Milliseconds 300
                    }
                    if (-not [StressWin32]::OwnsFocus($process.Id)) { throw 'Runtime lacks foreground focus; refusing a contaminated measurement.' }
                    $gpuBusy = @(Get-QuakeRayGpuBusyProcesses -ExcludedProcessIds ($ancestors + @($process.Id)))
                    if ($gpuBusy.Count) {
                        $owners = ($gpuBusy | ForEach-Object { "$($_.ProcessId):$($_.ProcessName)" }) -join ', '
                        throw "MACHINE_BUSY: foreign GPU activity contaminated this capture before it started: $owners"
                    }
                    Send-Key $window 0x73 0x3e
                    Wait-Marker $process "QR_START_$id"
                    if ($MoveDuringCapture) {
                        [StressWin32]::Key($window, 0x44, 0x20, $false)
                    }
                    if (-not (Test-StressBudget -Deadline $script:RunDeadline -RequiredSeconds ($Seconds + $shutdownReserve))) {
                        Write-Host 'Start marker consumed the capture budget; this attempt is deferred.'
                        if ($MoveDuringCapture) {
                            [StressWin32]::Key($window, 0x44, 0x20, $true)
                        }
                        Send-Key $window 0x74 0x3f
                        Send-Key $window 0x7a 0x57
                        $null = $process.WaitForExit((Get-StressExitTimeout -Deadline $script:RunDeadline))
                        break
                    }
                    $deadline = Get-StressDeadline -Deadline $script:RunDeadline.AddSeconds(-$shutdownReserve) -MaximumSeconds $Seconds
                    $nextOwnerCheck = Get-Date
                    $focusLostSince = $null
                    $focusLosses = 0
                    while ((Get-Date) -lt $deadline) {
                        if (Test-QuakeRayStopRequested) { break }
                        if (-not [StressWin32]::OwnsFocus($process.Id)) {
                            if ($null -eq $focusLostSince) { $focusLostSince = Get-Date; $focusLosses++ }
                            [StressWin32]::ForceFocus($window) | Out-Null
                            if (((Get-Date) - $focusLostSince).TotalSeconds -gt 3) { throw 'Focus was lost during capture.' }
                        } else {
                            $focusLostSince = $null
                        }
                        if ((Get-Date) -ge $nextOwnerCheck) {
                            $foreign = @(Get-Process -ErrorAction SilentlyContinue | Where-Object {
                                $_.Id -ne $process.Id -and
                                $_.Threads.Count -gt 0 -and
                                $_.ProcessName -match '^(quakeray|qray_|cmake$|ninja$|cl$|link$)'
                            })
                            if ($foreign.Count) {
                                $owners = ($foreign | ForEach-Object { "$($_.Id):$($_.ProcessName)" }) -join ', '
                                throw "MACHINE_BUSY: foreign heavy activity contaminated this capture: $owners"
                            }
                            $nextOwnerCheck = (Get-Date).AddSeconds(1)
                        }
                        Start-Sleep -Milliseconds 100
                    }
                    $gpuBusy = @(Get-QuakeRayGpuBusyProcesses -ExcludedProcessIds ($ancestors + @($process.Id)))
                    if ($gpuBusy.Count) {
                        $owners = ($gpuBusy | ForEach-Object { "$($_.ProcessId):$($_.ProcessName)" }) -join ', '
                        throw "MACHINE_BUSY: foreign GPU activity contaminated this capture: $owners"
                    }
                    if ($focusLosses -ne 0) {
                        throw "Capture lost runtime focus $focusLosses time(s); refusing a contaminated measurement."
                    }
                    if ($MoveDuringCapture) {
                        [StressWin32]::Key($window, 0x44, 0x20, $true)
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
                    }
                    if (Test-QuakeRayStopRequested) { throw 'CANCELLED: owned runtime stopped; capture remains unaccepted.' }
                    $windowChecks = $null
                    if ($StatsLevel -gt 0) {
                        $dump = Get-ChildItem $gameDir -Filter 'stats-*.dump' | Where-Object FullName -notin $oldDumps |
                            Sort-Object LastWriteTime | Select-Object -Last 1
                        if (-not $dump) { throw 'No stats dump was generated' }
                        Copy-Item $dump.FullName (Join-Path $output "$label.dump")
                        if ($StatsLevel -eq 3) {
                            $dumpRows = @(Get-Content $dump.FullName | Where-Object { $_ -and $_ -notmatch '^#' } | ConvertFrom-Csv)
                            $published = @($dumpRows | Where-Object { $_.'cpu.window_id' -match '^[1-9][0-9]*$' })
                            $ids = @($published | ForEach-Object { [uint32]$_.'cpu.window_id' } | Sort-Object -Unique)
                            if ($ids.Count -lt 2) { throw 'Profiler windows did not advance during capture; rebuild or investigate publication.' }
                            foreach ($row in $published) {
                                if ([int]$row.'cpu.window_frames' -le 0 -or
                                    [int]$row.'cpu.window_frames' -ne [int]$row.'cpu.renderer_samples' -or
                                    [double]$row.'cpu.qrDrawFrame_avg_ms' -le 0) {
                                    throw 'Profiler window has an incomplete result set or an invalid denominator.'
                                }
                            }
                            $windowChecks = @{ PublishedWindows = $ids.Count; FirstWindowFrames = [int]$published[0].'cpu.window_frames';
                                FirstRendererSamples = [int]$published[0].'cpu.renderer_samples'; Complete = $true }
                        }
                    }
                    $blocks = @(Get-Content $bench)
                    $newBlock = $blocks[$oldBench..($blocks.Count - 1)]
                    $newBlock | Set-Content (Join-Path $output "$label.bench.log")
                    $settings = $newBlock | Where-Object { $_ -match '^settings ' } | Select-Object -Last 1
                    $expectedClouds = if ($RtProfile -eq 'medium') { 1 } else { 0 }
                    $profileSettings = if ($RtProfile -eq 'medium' -and -not $AllowProfileOverrides) {
                        @('rt_gi_level=1', 'rt_sky_godrays=1', 'rt_sky_godrays_quality=1',
                          'rt_sky_clouds=1', 'rt_physical_sky=1', 'rt_physical_sun=1',
                          'rt_sky_nee=1', 'rt_denoiser=1', 'r_particles=1', 'r_smoke=1',
                          'rt_renderscale=0', 'rt_upscale_dlss=0', 'rt_dtal_lod=0',
                          'rt_q2_lightstats=1')
                    } else {
                        @()
                    }
                    if ($settings -notmatch 'vid=3840x2160@' -or $settings -notmatch "rt_upscale_fsr31=$fsr(?: |$)" -or
                        $settings -notmatch "rt_sky_clouds_quality=$expectedClouds(?: |$)") { throw 'Effective resolution/FSR/cloud quality does not match the requested target.' }
                    foreach ($expected in @(
                        "vsync=$ExpectedVsync", "vid_maxframelatency=$ExpectedMaxFrameLatency", 'fov=90',
                        'rt_enable_pvs=0', 'sv_novis=1', "rt_restir=$ExpectedRestir",
                        'rt_nee_samples=2', 'rt_restir_candidates=8',
                        'rt_reflrefr_depth=8', 'rt_dtal_groups=0',
                        'rt_dtal_spacing=128', 'r_fteparticles=0') + $profileSettings) {
                        if ($settings -notmatch "(?:^| )$([regex]::Escape($expected))(?: |$)") {
                            throw "Effective settings do not contain the standard '$expected'."
                        }
                    }
                    if ($mapMode) {
                        $expectedMapMarker = "`"mapname`" is `"$($expectedMaps[$save])`""
                        if ((Get-Content $console -Raw) -notmatch [regex]::Escape($expectedMapMarker)) {
                            throw 'Unexpected map in benchmark capture'
                        }
                    } elseif (($newBlock -join "`n") -notmatch "demo=$([regex]::Escape("maps/$($expectedMaps[$save]).bsp"))") {
                        throw 'Unexpected map in benchmark capture'
                    }
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
                    $manifest.CaptureChecks[$label] = @{
                        FocusVerified = (-not $Scheduled -and $focusLosses -eq 0)
                        FocusLosses = $focusLosses
                        ScheduledCommands = [bool]$Scheduled
                        ScheduledWarmupFrames = $scheduledWarmupFrames
                        ScheduledCaptureFrames = $scheduledCaptureFrames
                        Completed = $true
                        StatsLevel = $StatsLevel
                    }
                    if ($null -ne $windowChecks) { $manifest.CaptureChecks[$label].ProfilerWindows = $windowChecks }
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
