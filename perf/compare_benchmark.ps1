param(
    [Parameter(Mandatory = $true)]
    [string]$Benchmark,

    [string]$Baseline = "",

    [double]$Tolerance = 0.20,

    [int]$Index = -1,

    [switch]$Update
)

$ErrorActionPreference = "Stop"

function Read-BenchmarkBlocks([string]$Path)
{
    if (-not (Test-Path -LiteralPath $Path)) { throw "benchmark log not found: $Path" }

    $blocks = New-Object System.Collections.ArrayList
    $current = $null

    foreach ($line in Get-Content -LiteralPath $Path)
    {
        if ($line -match '^#\s*rt_bench\s+(.*)$')
        {
            if ($null -ne $current) { [void]$blocks.Add($current) }
            $current = [ordered]@{ Header = $Matches[1]; Values = [ordered]@{} }
            if ($current.Header -match 'fps=([0-9.]+)') { $current.Values['fps'] = [double]$Matches[1] }
            continue
        }

        if ($null -eq $current) { continue }

        if ($line -match '^\s*cpu\.slot\s+(.+?)\s+avg_ms=([0-9.]+)\s+max_ms=([0-9.]+)')
        {
            $current.Values["slot:$($Matches[1].Trim())"] = [double]$Matches[2]
            continue
        }

        if ($line -match '^\s*cpu\.main\s+(.+?)\s+avg_ms=([0-9.]+)')
        {
            $current.Values["main:$($Matches[1].Trim())"] = [double]$Matches[2]
            continue
        }

        if ($line -match '^\s*cpu\.cluster lists\s+hits=(\d+)\s+misses=(\d+)')
        {
            $current.Values['cluster:hits'] = [double]$Matches[1]
            $current.Values['cluster:misses'] = [double]$Matches[2]
            continue
        }
    }

    if ($null -ne $current) { [void]$blocks.Add($current) }
    if ($blocks.Count -eq 0) { throw "no rt_bench blocks found in $Path" }

    return ,$blocks
}

$blocks = Read-BenchmarkBlocks $Benchmark

$resolvedIndex = $Index
if ($resolvedIndex -lt 0) { $resolvedIndex = $blocks.Count + $resolvedIndex }
if ($resolvedIndex -lt 0 -or $resolvedIndex -ge $blocks.Count) { throw "block index $Index is out of range (0..$($blocks.Count - 1))" }

$block = $blocks[$resolvedIndex]
$measured = $block.Values

if ([string]::IsNullOrWhiteSpace($Baseline))
{
    $Baseline = Join-Path (Split-Path -Parent (Resolve-Path -LiteralPath $Benchmark)) 'benchmark.baseline.json'
}

if ($Update -or -not (Test-Path -LiteralPath $Baseline))
{
    $measured | ConvertTo-Json | Set-Content -LiteralPath $Baseline -Encoding UTF8
    Write-Host "baseline stored: $Baseline"
    Write-Host "rt_bench $($block.Header)"
    exit 0
}

$baselineData = Get-Content -LiteralPath $Baseline -Raw | ConvertFrom-Json

$failed = $false
$rows = New-Object System.Collections.ArrayList

foreach ($key in $measured.Keys)
{
    if (-not $baselineData.PSObject.Properties.Name.Contains($key)) { continue }

    $base = [double]$baselineData.PSObject.Properties[$key].Value
    $value = [double]$measured[$key]
    if ($base -le 0.0) { continue }

    if ($key -eq 'fps' -or $key -eq 'cluster:hits' -or $key -eq 'cluster:misses')
    {
        $worse = $value -lt $base * (1.0 - $Tolerance)
    }
    else
    {
        $worse = $value -gt $base * (1.0 + $Tolerance)
    }

    [void]$rows.Add([pscustomobject]@{
        Metric = $key
        Baseline = [math]::Round($base, 3)
        Measured = [math]::Round($value, 3)
        Delta = [math]::Round(($value / $base - 1.0) * 100.0, 1)
        Verdict = if ($worse) { 'REGRESSION' } else { 'ok' }
    })

    if ($worse) { $failed = $true }
}

$rows | Format-Table -AutoSize | Out-String | Write-Host

Write-Host "rt_bench $($block.Header)"
Write-Host "tolerance: $([math]::Round($Tolerance * 100.0, 1))%"

if ($failed)
{
    Write-Host "perf comparison failed"
    exit 1
}

Write-Host "perf comparison passed"
exit 0
