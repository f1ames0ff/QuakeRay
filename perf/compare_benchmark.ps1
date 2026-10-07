param(
    [Parameter(Mandatory = $true)]
    [string]$Benchmark,

    [string]$Baseline = "",

    [double]$Tolerance = 0.20,

    [int]$Index = -1,

    [switch]$Update,

    [string[]]$RequireMetrics = @(),

    [switch]$Strict,

    [string]$Samples = ""
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

        if ($line -match '^\s*cpu\.slot\s+(.+?)\s+avg_ms=([0-9.]+)\s+max_ms=([0-9.]+)(.*)$')
        {
            $name = $Matches[1].Trim()
            $avg = [double]$Matches[2]
            $tail = $Matches[4]
            $p95 = $null
            if ($tail -match 'p95_ms=([0-9.]+)') { $p95 = [double]$Matches[1] }

            $current.Values["slot:$name"] = $avg
            if ($null -ne $p95) { $current.Values["slot:${name}:p95"] = $p95 }
            continue
        }

        if ($line -match '^\s*cpu\.main\s+(.+?)\s+avg_ms=([0-9.]+)(.*)$')
        {
            $name = $Matches[1].Trim()
            $avg = [double]$Matches[2]
            $tail = $Matches[3]
            $p95 = $null
            if ($tail -match 'p95_ms=([0-9.]+)') { $p95 = [double]$Matches[1] }

            $current.Values["main:$name"] = $avg
            if ($null -ne $p95) { $current.Values["main:${name}:p95"] = $p95 }
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

function Get-Percentile95([double[]]$Values)
{
    $sorted = [double[]]$Values.Clone()
    [array]::Sort($sorted)

    $rank = [int][math]::Ceiling(0.95 * $sorted.Length) - 1
    if ($rank -lt 0) { $rank = 0 }
    if ($rank -ge $sorted.Length) { $rank = $sorted.Length - 1 }

    return [double]$sorted[$rank]
}

function Read-SampleP95([string]$Path)
{
    if (-not (Test-Path -LiteralPath $Path)) { throw "stats dump not found: $Path" }

    $culture = [System.Globalization.CultureInfo]::InvariantCulture
    $styles = [System.Globalization.NumberStyles]::Float
    $header = $null
    $values = [ordered]@{}

    foreach ($line in Get-Content -LiteralPath $Path)
    {
        if ([string]::IsNullOrWhiteSpace($line) -or $line -match '^\s*#') { continue }

        $fields = $line.Trim().Split(",")

        if ($null -eq $header)
        {
            $header = $fields
            for ($i = 0; $i -lt $header.Length; $i++)
            {
                if ($header[$i] -match '^(fps|cpu\..*_ms|gpu\..*_ms)$')
                {
                    $values[$header[$i]] = New-Object System.Collections.Generic.List[double]
                }
            }
            continue
        }

        if ($fields.Length -ne $header.Length) { continue }

        for ($i = 0; $i -lt $header.Length; $i++)
        {
            if (-not $values.Contains($header[$i])) { continue }

            $parsed = 0.0
            if ([double]::TryParse($fields[$i], $styles, $culture, [ref]$parsed))
            {
                $values[$header[$i]].Add($parsed)
            }
        }
    }

    if ($null -eq $header) { throw "no table found in $Path" }

    $count = 0
    foreach ($name in $values.Keys)
    {
        $list = $values[$name]
        if ($list.Count -gt 0)
        {
            [pscustomobject]@{ Name = "p95:$name"; Value = Get-Percentile95 $list.ToArray() }
            ++$count
        }
    }

    if ($count -eq 0) { throw "no fps/cpu/gpu millisecond samples found in $Path" }
}

function Test-LowerIsWorse([string]$Key)
{
    return $Key -eq "fps" -or $Key -eq "p95:fps" -or $Key -eq "cluster:hits"
}

$blocks = Read-BenchmarkBlocks $Benchmark

$resolvedIndex = $Index
if ($resolvedIndex -lt 0) { $resolvedIndex = $blocks.Count + $resolvedIndex }
if ($resolvedIndex -lt 0 -or $resolvedIndex -ge $blocks.Count) { throw "block index $Index is out of range (0..$($blocks.Count - 1))" }

$block = $blocks[$resolvedIndex]
$measured = $block.Values

if (-not [string]::IsNullOrWhiteSpace($Samples))
{
    $sampleP95 = @(Read-SampleP95 $Samples)
    foreach ($entry in $sampleP95) { $measured[$entry.Name] = $entry.Value }
    Write-Host "p95 sample metrics: $($sampleP95.Count) from $(Resolve-Path -LiteralPath $Samples)"
}

if ([string]::IsNullOrWhiteSpace($Baseline))
{
    $Baseline = Join-Path (Split-Path -Parent (Resolve-Path -LiteralPath $Benchmark)) 'benchmark.baseline.json'
}

if ($Update -or -not (Test-Path -LiteralPath $Baseline))
{
    $measured | ConvertTo-Json | Set-Content -LiteralPath $Baseline -Encoding UTF8
    Write-Host "baseline stored: $Baseline"
    Write-Host "metrics stored: $($measured.Count)"
    Write-Host "rt_bench $($block.Header)"
    exit 0
}

$baselineData = Get-Content -LiteralPath $Baseline -Raw | ConvertFrom-Json
$baselineNames = @($baselineData.PSObject.Properties | ForEach-Object { $_.Name })

$required = @{}
foreach ($key in $RequireMetrics)
{
    if (-not [string]::IsNullOrWhiteSpace($key)) { $required[$key.Trim()] = $true }
}

$rows = New-Object System.Collections.ArrayList
$missingBaseline = New-Object System.Collections.ArrayList
$unusableBaseline = New-Object System.Collections.ArrayList
$staleBaseline = New-Object System.Collections.ArrayList
$failed = $false
$regressions = 0

foreach ($key in $measured.Keys)
{
    if (-not ($baselineNames -contains $key))
    {
        [void]$missingBaseline.Add($key)
        continue
    }

    $base = [double]$baselineData.PSObject.Properties[$key].Value
    $value = [double]$measured[$key]

    if ($base -le 0.0)
    {
        [void]$unusableBaseline.Add($key)
        continue
    }

    if (Test-LowerIsWorse $key)
    {
        $worse = $value -lt $base * (1.0 - $Tolerance)
    }
    else
    {
        $worse = $value -gt $base * (1.0 + $Tolerance)
    }

    $verdict = 'ok'
    if ($worse) { $verdict = 'REGRESSION' }

    [void]$rows.Add([pscustomobject]@{
        Metric = $key
        Baseline = [math]::Round($base, 3)
        Measured = [math]::Round($value, 3)
        Delta = [math]::Round(($value / $base - 1.0) * 100.0, 1)
        Verdict = $verdict
    })

    if ($worse)
    {
        ++$regressions
        $failed = $true
    }
}

foreach ($key in $missingBaseline)
{
    $verdict = 'missing'
    if ($Strict -or $required.ContainsKey($key))
    {
        $verdict = 'MISSING'
        $failed = $true
    }

    [void]$rows.Add([pscustomobject]@{
        Metric = $key
        Baseline = $null
        Measured = [math]::Round([double]$measured[$key], 3)
        Delta = $null
        Verdict = $verdict
    })
}

foreach ($key in $unusableBaseline)
{
    $verdict = 'unusable'
    if ($Strict -or $required.ContainsKey($key))
    {
        $verdict = 'UNUSABLE'
        $failed = $true
    }

    [void]$rows.Add([pscustomobject]@{
        Metric = $key
        Baseline = [math]::Round([double]$baselineData.PSObject.Properties[$key].Value, 3)
        Measured = [math]::Round([double]$measured[$key], 3)
        Delta = $null
        Verdict = $verdict
    })
}

foreach ($key in $required.Keys)
{
    if ($measured.Contains($key)) { continue }

    $base = $null
    if ($baselineNames -contains $key)
    {
        $base = [math]::Round([double]$baselineData.PSObject.Properties[$key].Value, 3)
    }

    [void]$rows.Add([pscustomobject]@{
        Metric = $key
        Baseline = $base
        Measured = $null
        Delta = $null
        Verdict = 'NOT-MEASURED'
    })
    $failed = $true
}

foreach ($key in $baselineNames)
{
    if ($measured.Contains($key)) { continue }
    if ($required.ContainsKey($key)) { continue }

    if ($Strict)
    {
        [void]$rows.Add([pscustomobject]@{
            Metric = $key
            Baseline = [math]::Round([double]$baselineData.PSObject.Properties[$key].Value, 3)
            Measured = $null
            Delta = $null
            Verdict = 'NOT-MEASURED'
        })
        $failed = $true
    }
    else
    {
        [void]$staleBaseline.Add($key)
    }
}

$rows | Format-Table -AutoSize | Out-String | Write-Host

Write-Host "rt_bench $($block.Header)"
Write-Host "tolerance: $([math]::Round($Tolerance * 100.0, 1))%"
if ($Strict) { Write-Host "strict: the measured and baseline metric sets must match" }
if ($required.Count -gt 0) { Write-Host "required metrics: $(@($required.Keys) -join ', ')" }

if (-not $Strict)
{
    $warnMissing = @($missingBaseline | Where-Object { -not $required.ContainsKey($_) })
    if ($warnMissing.Count -gt 0)
    {
        Write-Warning "metrics missing from the baseline (not compared): $($warnMissing -join ', ')"
    }

    $warnUnusable = @($unusableBaseline | Where-Object { -not $required.ContainsKey($_) })
    if ($warnUnusable.Count -gt 0)
    {
        Write-Warning "metrics with a baseline <= 0 (not compared): $($warnUnusable -join ', ')"
    }
}

if ($staleBaseline.Count -gt 0)
{
    Write-Warning "baseline metrics that this run does not measure: $($staleBaseline -join ', ')"
}

if ($failed)
{
    if ($regressions -gt 0) { Write-Host "perf comparison failed: $regressions regression(s)" }
    else { Write-Host "perf comparison failed: metric coverage incomplete" }
    exit 1
}

Write-Host "perf comparison passed"
exit 0
