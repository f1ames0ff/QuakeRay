#Requires -Version 5.1

<#
.SYNOPSIS
Captures a reproducible QuakeRay caustics screenshot for the manual A/B gates.

.DESCRIPTION
Launches quakeray.exe with -condebug, applies the given console variables, runs
"+load quick", the requested number of "+wait" frames and "+screenshot", then waits
for a new screenshotNNNN.png to appear, copies it to <OutDir>\<Name>.png and
downscales that copy to 1600 px wide.

Console variables are space-separated name/value pairs, for example:
    .\capture_caustics.ps1 -Name caustics_on -Cvars "rt_caustics 1 rt_water_lightpath 1"

The script never edits config.cfg, never deletes or modifies existing screenshots,
and never restores or terminates anything: the game keeps running after the capture.

.PARAMETER Name
Output base name, without extension. Required.

.PARAMETER Cvars
Space-separated cvar name/value pairs, passed as +<name> <value> arguments.

.PARAMETER Waits
Number of "+wait" commands issued after "+load quick" (default 25).

.PARAMETER TimeoutSeconds
Seconds to wait for the new screenshot file (default 300).

.PARAMETER GameDir
Directory containing quakeray.exe (default <script dir>\build\Debug).

.PARAMETER OutDir
Directory for the output copy (default <script dir>\build\Debug\id1).
#>

param(
    [Parameter(Mandatory = $true)]
    [string]$Name,
    [string]$Cvars = "",
    [int]$Waits = 25,
    [int]$TimeoutSeconds = 300,
    [string]$GameDir = "$PSScriptRoot\build\Debug",
    [string]$OutDir = "$PSScriptRoot\build\Debug\id1"
)

$ErrorActionPreference = "Stop"

if ($Name -like "screenshot*") {
    Write-Host "ERROR: -Name must not start with 'screenshot'; it would collide with the game's own screenshot files." -ForegroundColor Red
    exit 1
}

if (Get-Process -Name "quakeray" -ErrorAction SilentlyContinue) {
    Write-Host "ERROR: quakeray is already running. Close the game before capturing." -ForegroundColor Red
    exit 1
}

$exe = Join-Path $GameDir "quakeray.exe"
if (-not (Test-Path -LiteralPath $exe)) {
    Write-Host "ERROR: quakeray.exe not found at $exe" -ForegroundColor Red
    exit 1
}

$tokens = @($Cvars -split '\s+' | Where-Object { $_ -ne "" })
if (($tokens.Count % 2) -ne 0) {
    Write-Host "ERROR: -Cvars must be space-separated name/value pairs, e.g. -Cvars 'rt_caustics 1 rt_water_lightpath 1'." -ForegroundColor Red
    exit 1
}

$cvarArgs = @()
for ($i = 0; $i -lt $tokens.Count; $i += 2) {
    $cvarArgs += "+$($tokens[$i])"
    $cvarArgs += $tokens[$i + 1]
}

$watchDirs = @()
foreach ($candidate in @((Join-Path $GameDir "id1"), $GameDir, $OutDir)) {
    if (Test-Path -LiteralPath $candidate) {
        $full = (Resolve-Path -LiteralPath $candidate).Path
        if ($watchDirs -notcontains $full) {
            $watchDirs += $full
        }
    }
}
if ($watchDirs.Count -eq 0) {
    Write-Host "ERROR: none of the screenshot directories exist (GameDir: $GameDir, OutDir: $OutDir)." -ForegroundColor Red
    exit 1
}

$known = @{}
foreach ($dir in $watchDirs) {
    $known[$dir] = @(Get-ChildItem -LiteralPath $dir -Filter "screenshot*.png" -File -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Name)
}

function Get-NewScreenshot {
    param(
        [string[]]$Directories,
        [hashtable]$Known
    )

    foreach ($dir in $Directories) {
        if (-not (Test-Path -LiteralPath $dir)) { continue }
        $files = @(Get-ChildItem -LiteralPath $dir -Filter "screenshot*.png" -File -ErrorAction SilentlyContinue)
        foreach ($file in $files) {
            if (@($Known[$dir]) -notcontains $file.Name) {
                return $file
            }
        }
    }
    return $null
}

$gameArgs = @("-condebug") + $cvarArgs + @("+load", "quick")
for ($i = 0; $i -lt $Waits; $i++) {
    $gameArgs += "+wait"
}
$gameArgs += "+screenshot"

Write-Host "Launching: $exe $($gameArgs -join ' ')"
$proc = Start-Process -FilePath $exe -WorkingDirectory $GameDir -ArgumentList $gameArgs -PassThru

$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
$newShot = $null
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 500
    $newShot = Get-NewScreenshot -Directories $watchDirs -Known $known
    if ($null -ne $newShot) { break }
    if ($proc.HasExited) {
        Start-Sleep -Milliseconds 1000
        $newShot = Get-NewScreenshot -Directories $watchDirs -Known $known
        break
    }
}

if ($null -eq $newShot) {
    Write-Host "ERROR: no new screenshot appeared within $TimeoutSeconds seconds." -ForegroundColor Red
    $log = Join-Path $GameDir "qconsole.log"
    if (Test-Path -LiteralPath $log) {
        Write-Host "Last 20 lines of ${log}:"
        Get-Content -LiteralPath $log -Tail 20 | ForEach-Object { Write-Host $_ }
    } else {
        Write-Host "qconsole.log not found at $log"
    }
    Write-Host "quakeray is left as it is; close it manually if it is still running."
    exit 1
}

$lastSize = -1
for ($i = 0; $i -lt 20; $i++) {
    $info = Get-Item -LiteralPath $newShot.FullName -ErrorAction SilentlyContinue
    if ($null -eq $info) {
        Start-Sleep -Milliseconds 250
        continue
    }
    if ($info.Length -gt 0 -and $info.Length -eq $lastSize) {
        break
    }
    $lastSize = $info.Length
    Start-Sleep -Milliseconds 250
}

if (-not (Test-Path -LiteralPath $OutDir)) {
    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
}
$dest = Join-Path $OutDir "$Name.png"
Copy-Item -LiteralPath $newShot.FullName -Destination $dest -Force

Add-Type -AssemblyName System.Drawing
$source = [System.Drawing.Image]::FromFile($dest)
try {
    $working = New-Object -TypeName System.Drawing.Bitmap -ArgumentList $source
} finally {
    $source.Dispose()
}

try {
    if ($working.Width -gt 1600) {
        $newHeight = [int][Math]::Round(($working.Height * 1600.0) / $working.Width)
        if ($newHeight -lt 1) { $newHeight = 1 }
        $resized = New-Object -TypeName System.Drawing.Bitmap -ArgumentList 1600, $newHeight
        try {
            $graphics = [System.Drawing.Graphics]::FromImage($resized)
            try {
                $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
                $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
                $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
                $graphics.DrawImage($working, 0, 0, 1600, $newHeight)
            } finally {
                $graphics.Dispose()
            }
        } finally {
            $working.Dispose()
        }
        $working = $resized
    }
    $working.Save($dest, [System.Drawing.Imaging.ImageFormat]::Png)
} finally {
    $working.Dispose()
}

Write-Host "Capture saved: $dest" -ForegroundColor Green
Write-Host "The game keeps running; the script changed nothing else and restored nothing."
