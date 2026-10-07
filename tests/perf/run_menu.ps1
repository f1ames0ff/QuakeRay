param(
    [string]$Candidate = '',
    [string]$Baseline = '',
    [ValidateRange(2, 25)][int]$Seconds = 8,
    [int]$Width = 1920,
    [int]$Height = 1080,
    [switch]$Smoke,
    [switch]$Validation,
    [switch]$BaselineOnly
)

$ErrorActionPreference = 'Stop'
if (-not $Candidate) { $Candidate = Join-Path $PSScriptRoot '..\..\build\Debug' }

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class MenuPerfWin32 {
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr window, uint message, IntPtr key, IntPtr data);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window);
    public static void Key(IntPtr window, int key, int scan, bool up) {
        int data = 1 | (scan << 16);
        if (up) data |= unchecked((int)0xc0000000);
        PostMessage(window, up ? 0x101u : 0x100u, (IntPtr)key, (IntPtr)data);
    }
}
"@

function Send-Key([IntPtr]$Window, [int]$Key, [int]$Scan) {
    [MenuPerfWin32]::Key($Window, $Key, $Scan, $false)
    Start-Sleep -Milliseconds 60
    [MenuPerfWin32]::Key($Window, $Key, $Scan, $true)
    Start-Sleep -Milliseconds 200
}

function Median($Values) {
    $sorted = @($Values | Sort-Object)
    if ($sorted.Count -eq 0) { return $null }
    $middle = [int][Math]::Floor($sorted.Count / 2)
    if ($sorted.Count % 2) { return $sorted[$middle] }
    return ($sorted[$middle - 1] + $sorted[$middle]) / 2
}

function Read-Capture([string]$GameDir, $PreviousFiles) {
    $file = Get-ChildItem -Path $GameDir -Filter 'stats-*.dump' |
        Where-Object FullName -notin $PreviousFiles | Sort-Object LastWriteTime | Select-Object -Last 1
    if (-not $file) { throw "No stats capture was written in $GameDir" }
    $rows = @(Get-Content $file.FullName | Where-Object { $_ -and -not $_.StartsWith('#') } | ConvertFrom-Csv)
    if ($rows.Count -lt 5) { throw "Too few samples in $($file.FullName)" }
    return @{ Path = $file.FullName; Rows = $rows }
}

function Values($Rows, [string]$Name) {
    return @($Rows | ForEach-Object {
        if ($null -ne $_.$Name -and $_.$Name -ne '') {
            [double]::Parse($_.$Name, [System.Globalization.CultureInfo]::InvariantCulture)
        }
    })
}

function Check-UiCapture($Capture) {
    $passes = @('clouds', 'sky', 'primary', 'decals', 'godrays', 'reflrefr', 'reflgodr',
                'gradient', 'direct', 'indirect', 'compose', 'upscale', 'post')
    foreach ($pass in $passes) {
        if (@(Values $Capture.Rows "gpu.${pass}_ms" | Where-Object { $_ -ne 0.0 }).Count -gt 0) {
            throw "A UI-only frame ran the $pass pass: $($Capture.Path)"
        }
    }
    foreach ($row in $Capture.Rows) {
        $total = [double]::Parse($row.'gpu.frame_ms', [System.Globalization.CultureInfo]::InvariantCulture)
        foreach ($property in $row.PSObject.Properties) {
            if ($property.Name -match '^gpu\.(?!frame_ms).*_ms$' -and $property.Value -ne '') {
                $value = [double]::Parse($property.Value, [System.Globalization.CultureInfo]::InvariantCulture)
                if ($value -gt $total + 0.02) { throw "GPU pass exceeds its frame total: $($property.Name)" }
            }
        }
    }
    foreach ($pass in @('legacy_AS', 'staging', 'scene_record', 'compose', 'upscale', 'post')) {
        if (@(Values $Capture.Rows "cpu.draw.${pass}_max_ms" | Where-Object { $_ -ne 0.0 }).Count -gt 0) {
            throw "A UI-only frame recorded $pass CPU work: $($Capture.Path)"
        }
    }
}

function Run-Arm([string]$Directory, [string]$Name, [bool]$RunSmoke) {
    $Directory = (Resolve-Path $Directory).Path
    $exe = Join-Path $Directory 'quakeray.exe'
    $gameDir = Join-Path $Directory 'id1'
    $config = Join-Path $gameDir 'config.cfg'
    $libraryConfig = Join-Path $Directory 'qray.txt'
    $savedConfig = if (Test-Path $config) { [System.IO.File]::ReadAllBytes($config) } else { $null }
    $savedLibraryConfig = if (Test-Path $libraryConfig) { [System.IO.File]::ReadAllBytes($libraryConfig) } else { $null }
    $tag = [Guid]::NewGuid().ToString('N').Substring(0, 8)
    $fixture = Join-Path $gameDir "qr_m_$tag.cfg"
    $console = Join-Path $Directory 'qconsole.log'
    $stderr = Join-Path $Directory "menu-$Name-$tag.stderr.log"
    $stdout = Join-Path $Directory "menu-$Name-$tag.stdout.log"
    $previous = @(Get-ChildItem $gameDir -Filter 'stats-*.dump' | ForEach-Object FullName)
    $process = $null

    @"
cl_startdemos 0
stopdemo
disconnect
vid_vsync 0
host_maxfps 0
rt_stats 3
rt_stats_interval 0.2
developer 0
rt_ef_crt 0
bind F4 "rt_stats_dump_start"
bind F5 "rt_stats_dump_end"
bind F6 "screenshot"
bind F7 "rt_stats 0"
bind F8 "rt_ef_crt 1"
bind F9 "rt_ef_crt 0; rt_stats 3; map start"
bind F10 "disconnect; menu_main"
bind F11 "toggleconsole; quit"
bind F12 "vid_unlock; vid_width 1280; vid_height 720; vid_restart"
menu_main
echo QR_MENU_READY_$tag
"@ | Set-Content $fixture -Encoding Ascii

    try {
        if ($Validation) { 'vulkanvalidation' | Set-Content $libraryConfig -Encoding Ascii }
        $arguments = @('-basedir', '.', '-window', '-width', $Width, '-height', $Height,
                       '-nosound', '-condebug', '+exec', "qr_m_$tag.cfg")
        $process = Start-Process $exe -ArgumentList $arguments -WorkingDirectory $Directory -RedirectStandardError $stderr -RedirectStandardOutput $stdout -PassThru
        $null = $process.Handle
        $deadline = (Get-Date).AddSeconds(120)
        $window = [IntPtr]::Zero
        while ((Get-Date) -lt $deadline) {
            $process.Refresh()
            if ($process.HasExited) { throw "$Name exited during startup; see $console and $stderr" }
            if ($process.MainWindowHandle -ne 0 -and (Test-Path $console)) {
                if ((Get-Content $console -Raw) -match "QR_MENU_READY_$tag") {
                    $window = $process.MainWindowHandle
                    break
                }
            }
            Start-Sleep -Milliseconds 250
        }
        if ($window -eq [IntPtr]::Zero) { throw "$Name did not reach the menu" }
        [MenuPerfWin32]::SetForegroundWindow($window) | Out-Null
        Start-Sleep -Seconds 5
        Send-Key $window 0x73 0x3e
        Start-Sleep -Seconds $Seconds
        Send-Key $window 0x74 0x3f
        Start-Sleep -Seconds 1
        $capture = Read-Capture $gameDir $previous
        if ($Name -eq 'candidate') { Check-UiCapture $capture }

        $result = [PSCustomObject]@{
            Arm = $Name
            DrawFrameWindowMaxMedianMs = Median (Values $capture.Rows 'cpu.qrDrawFrame_ms')
            DrawFrameAverageMedianMs = Median (Values $capture.Rows 'cpu.qrDrawFrame_avg_ms')
            GpuMedianMs = Median (Values $capture.Rows 'gpu.frame_ms')
            RasterMedian = Median (Values $capture.Rows 'calls_raster')
            Capture = $capture.Path
        }
        Send-Key $window 0x75 0x40

        if ($RunSmoke) {
            Send-Key $window 0x76 0x41
            Send-Key $window 0x75 0x40
            Send-Key $window 0x77 0x42
            Send-Key $window 0x75 0x40
            Send-Key $window 0x78 0x43
            Start-Sleep -Seconds 5
            Send-Key $window 0x75 0x40
            Send-Key $window 0x1b 0x01
            $beforePause = @(Get-ChildItem $gameDir -Filter 'stats-*.dump' | ForEach-Object FullName)
            Send-Key $window 0x73 0x3e
            Start-Sleep -Seconds 3
            Send-Key $window 0x74 0x3f
            Start-Sleep -Seconds 1
            $pause = Read-Capture $gameDir $beforePause
            if ((Median (Values $pause.Rows 'gpu.primary_ms')) -le 0.0) { throw 'An in-game menu stopped primary rendering' }
            Send-Key $window 0x75 0x40
            Send-Key $window 0x79 0x44
            Start-Sleep -Seconds 3
            $beforeReturn = @(Get-ChildItem $gameDir -Filter 'stats-*.dump' | ForEach-Object FullName)
            Send-Key $window 0x73 0x3e
            Start-Sleep -Seconds 3
            Send-Key $window 0x74 0x3f
            Start-Sleep -Seconds 1
            $returned = Read-Capture $gameDir $beforeReturn
            if ($Name -eq 'candidate') { Check-UiCapture $returned }
            Send-Key $window 0x7b 0x58
            Start-Sleep -Seconds 2
            Send-Key $window 0x75 0x40
        }

        Send-Key $window 0x7a 0x57
        if (-not $process.WaitForExit(15000)) { throw "$Name did not exit cleanly" }
        if ($process.ExitCode -ne 0) { throw "$Name exited with code $($process.ExitCode)" }
        if ($Validation -and (Select-String $console -Pattern 'VUID-|Validation Error|NVRHI::ERROR' -Quiet)) {
            throw "Renderer validation errors; see $console"
        }
        return $result
    } finally {
        if ($process -and -not $process.HasExited) { Stop-Process -Id $process.Id -Force }
        if ($null -ne $savedConfig) { [System.IO.File]::WriteAllBytes($config, $savedConfig) }
        elseif (Test-Path $config) { Remove-Item $config }
        if ($Validation) {
            if ($null -ne $savedLibraryConfig) { [System.IO.File]::WriteAllBytes($libraryConfig, $savedLibraryConfig) }
            elseif (Test-Path $libraryConfig) { Remove-Item $libraryConfig }
        }
        Remove-Item $fixture
    }
}

$results = @()
if ($BaselineOnly -and -not $Baseline) { throw '-BaselineOnly requires -Baseline' }
if ($Baseline) { $results += Run-Arm $Baseline 'baseline' ([bool]($BaselineOnly -and $Smoke)) }
if (-not $BaselineOnly) { $results += Run-Arm $Candidate 'candidate' ([bool]$Smoke) }
$results | Format-List
