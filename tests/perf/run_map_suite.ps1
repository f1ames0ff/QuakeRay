param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('id1', 'mg1', 'mg3', 'ad')][string]$Game,
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9_./-]+$')][string[]]$Maps,
    [string]$Basedir = '',
    [ValidateSet('balanced', 'quality', 'performance', 'ultra')][string]$Preset = 'balanced',
    [ValidateSet('audit', 'medium')][string]$RtProfile = 'medium',
    [ValidateRange(2, 30)][int]$Warmup = 5,
    [ValidateRange(2, 25)][int]$Seconds = 10,
    [ValidateRange(1, 2)][int]$Repeats = 1,
    [ValidateRange(30, 300)][int]$RunSeconds = 300,
    [ValidateRange(0, 3)][int]$StatsLevel = 3,
    [string]$Tag = 'map-suite',
    [switch]$NoSound,
    [switch]$Scheduled,
    [switch]$MoveDuringCapture,
    [switch]$ContinueOnError
)

$ErrorActionPreference = 'Stop'
$runner = Join-Path $PSScriptRoot 'run_stress.ps1'
if (-not $Basedir) { $Basedir = Join-Path $PSScriptRoot '..\..\build\Debug' }
$Basedir = (Resolve-Path $Basedir).Path
$results = @()
$failed = $false

foreach ($map in $Maps) {
    $safeMap = $map -replace '[^A-Za-z0-9_.-]', '_'
    $mapTag = "$Tag-$Game-$safeMap"
    $arguments = @{
        Game = $Game
        Maps = @($map)
        Presets = @($Preset)
        RtProfile = $RtProfile
        Basedir = $Basedir
        Warmup = $Warmup
        Seconds = $Seconds
        Repeats = $Repeats
        MaxRunSeconds = $RunSeconds
        StatsLevel = $StatsLevel
        Tag = $mapTag
    }
    if ($NoSound) { $arguments.NoSound = $true }
    if ($Scheduled) { $arguments.Scheduled = $true }
    if ($MoveDuringCapture) { $arguments.MoveDuringCapture = $true }

    $started = Get-Date
    try {
        & $runner @arguments
        if ($LASTEXITCODE -ne 0) { throw "run_stress exited with code $LASTEXITCODE" }
        $capture = Get-ChildItem $Basedir -Directory -Filter "audit-$mapTag-*" |
            Where-Object { $_.LastWriteTime -ge $started } |
            Sort-Object LastWriteTime -Descending |
            Select-Object -First 1
        $summaryPath = if ($capture) { Get-ChildItem $capture.FullName -Filter '*.summary.json' | Select-Object -First 1 -ExpandProperty FullName }
        $results += [PSCustomObject]@{
            Game = $Game
            Map = $map
            Status = 'passed'
            Capture = $capture.FullName
            Summary = $summaryPath
        }
    } catch {
        $failed = $true
        $results += [PSCustomObject]@{
            Game = $Game
            Map = $map
            Status = 'failed'
            Error = $_.Exception.Message
        }
        if (-not $ContinueOnError) { throw }
    }
}

$suiteTag = ($Tag -replace '[^A-Za-z0-9_-]', '_') + '-' + $Game + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss')
$resultPath = Join-Path $Basedir "audit-$suiteTag.suite.json"
$results | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $resultPath -Encoding UTF8
$results | Format-Table -AutoSize
Write-Host "Suite result: $resultPath"
if ($failed) { exit 1 }
