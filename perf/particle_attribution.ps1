<#
.SYNOPSIS
Attributes the particle cost of Stage 0 ablation runs.

.DESCRIPTION
Reads the CSV recordings written by rt_stats_dump_start and rt_stats_dump_end
(stats-<date>-<time>.dump) and reports, for every arm, the mean and p95 of the
frozen particle instrumentation columns plus fps, gpu.frame_ms and
cpu.particles_ms, with the delta of every arm against a named baseline arm.

A requested column that a dump does not carry is reported as missing and
warned about; it is never skipped silently.

The cpu.* columns are per-window maxima (rt_stats_interval), so the mean and
p95 below are statistics of those maxima, not of per-frame times.

.PARAMETER Baseline
Name of the arm every other arm is compared against. Defaults to the first arm.

.PARAMETER Arms
A hashtable of arm name -> dump path, an array of hashtables with Name and Path
keys, an array of "name=path" strings, or an array of plain dump paths (the
file name becomes the arm name).

.PARAMETER Output
Writes the report to this path. Without it the report only goes to the console.

.PARAMETER Csv
Writes the long-format CSV instead of the markdown report.

.EXAMPLE
perf\particle_attribution.ps1 -Baseline baseline -Arms @{
    baseline   = 'build\Debug\ad\stats-heavy-baseline.dump'
    no_classic = 'build\Debug\ad\stats-heavy-no_classic.dump'
} -Output perf\stage0\attribution.md
#>
param(
    [string]$Baseline = "",

    [Parameter(Mandatory = $true)]
    [object]$Arms,

    [string]$Output = "",

    [switch]$Csv
)

$ErrorActionPreference = "Stop"

$invariant = [System.Globalization.CultureInfo]::InvariantCulture

$metricList = @(
    [pscustomobject]@{ Name = 'fps';                      Unit = 'fps';   Format = '0.0'  }
    [pscustomobject]@{ Name = 'gpu.frame_ms';             Unit = 'ms';    Format = '0.00' }
    [pscustomobject]@{ Name = 'cpu.particles_ms';         Unit = 'ms';    Format = '0.00' }
    [pscustomobject]@{ Name = 'cpu.particles_sim_ms';     Unit = 'ms';    Format = '0.00' }
    [pscustomobject]@{ Name = 'cpu.particles_resolve_ms'; Unit = 'ms';    Format = '0.00' }
    [pscustomobject]@{ Name = 'cpu.particles_fill_ms';    Unit = 'ms';    Format = '0.00' }
    [pscustomobject]@{ Name = 'cpu.particles_upload_ms';  Unit = 'ms';    Format = '0.00' }
    [pscustomobject]@{ Name = 'cpu.fte_convert_ms';       Unit = 'ms';    Format = '0.00' }
    [pscustomobject]@{ Name = 'gpu.particles_ms';         Unit = 'ms';    Format = '0.00' }
    [pscustomobject]@{ Name = 'particles_classic';        Unit = 'count'; Format = '0.##' }
    [pscustomobject]@{ Name = 'particles_fte';            Unit = 'count'; Format = '0.##' }
    [pscustomobject]@{ Name = 'particles_vertices';       Unit = 'count'; Format = '0.##' }
    [pscustomobject]@{ Name = 'particles_smoke';          Unit = 'count'; Format = '0.##' }
    [pscustomobject]@{ Name = 'particles_dropped';        Unit = 'count'; Format = '0.##' }
    [pscustomobject]@{ Name = 'particles_emit_culled';    Unit = 'count'; Format = '0.##' }
    [pscustomobject]@{ Name = 'particles_emit_faded';     Unit = 'count'; Format = '0.##' }
    [pscustomobject]@{ Name = 'fte_convert_bytes';        Unit = 'bytes'; Format = '0.##' }
    [pscustomobject]@{ Name = 'particle_upload_bytes';    Unit = 'bytes'; Format = '0.##' }
    [pscustomobject]@{ Name = 'particles_cache_hits';     Unit = 'count'; Format = '0.##' }
    [pscustomobject]@{ Name = 'particles_cache_misses';   Unit = 'count'; Format = '0.##' }
    [pscustomobject]@{ Name = 'particles_cache_avg_ns';   Unit = 'ns';    Format = '0'    }
    [pscustomobject]@{ Name = 'raster_upload_bytes';           Unit = 'bytes'; Format = '0.##' }
    [pscustomobject]@{ Name = 'raster_upload_dropped_batches'; Unit = 'count'; Format = '0.##' }
    [pscustomobject]@{ Name = 'rays_particle';            Unit = 'rays';  Format = '0.##' }
)

function Get-DictionaryValue($Dictionary, [string[]]$Names)
{
    foreach ($key in $Dictionary.Keys)
    {
        foreach ($name in $Names)
        {
            if ([string]$key -ieq $name) { return $Dictionary[$key] }
        }
    }
    return $null
}

function Get-ObjectProperty($Object, [string[]]$Names)
{
    foreach ($property in $Object.PSObject.Properties)
    {
        foreach ($name in $Names)
        {
            if ($property.Name -ieq $name) { return $property }
        }
    }
    return $null
}

function ConvertTo-ArmList($Arms)
{
    $list = New-Object System.Collections.ArrayList

    if ($Arms -is [System.Collections.IDictionary])
    {
        foreach ($key in $Arms.Keys)
        {
            [void]$list.Add([pscustomobject]@{ Name = [string]$key; Path = [string]$Arms[$key] })
        }
        return ,$list
    }

    foreach ($item in @($Arms))
    {
        if ($null -eq $item) { continue }

        if ($item -is [System.Collections.IDictionary])
        {
            $name = Get-DictionaryValue $item @('Name', 'Arm', 'Label')
            $path = Get-DictionaryValue $item @('Path', 'File', 'Dump')
            if ([string]::IsNullOrWhiteSpace([string]$name)) { throw "an -Arms entry has no Name: $item" }
            if ([string]::IsNullOrWhiteSpace([string]$path)) { throw "arm '$name' has no Path" }
            [void]$list.Add([pscustomobject]@{ Name = [string]$name; Path = [string]$path })
            continue
        }

        if ($item -is [string])
        {
            $text = $item.Trim()
            $equals = $text.IndexOf('=')
            if ($equals -gt 0)
            {
                $name = $text.Substring(0, $equals).Trim()
                $path = $text.Substring($equals + 1).Trim()
            }
            else
            {
                $name = [System.IO.Path]::GetFileNameWithoutExtension($text)
                $path = $text
            }
            if ([string]::IsNullOrWhiteSpace($name)) { throw "could not read an -Arms entry: $item" }
            [void]$list.Add([pscustomobject]@{ Name = $name; Path = $path })
            continue
        }

        $nameProperty = Get-ObjectProperty $item @('Name', 'Arm', 'Label')
        $pathProperty = Get-ObjectProperty $item @('Path', 'File', 'Dump')
        if ($null -eq $nameProperty -or $null -eq $pathProperty)
        {
            throw "an -Arms entry needs Name and Path: $item"
        }
        [void]$list.Add([pscustomobject]@{ Name = [string]$nameProperty.Value; Path = [string]$pathProperty.Value })
    }

    return ,$list
}

function Read-StatsDump([string]$Path)
{
    if (-not (Test-Path -LiteralPath $Path)) { throw "stats dump not found: $Path" }

    $header = $null
    $columns = $null
    $rows = New-Object System.Collections.ArrayList

    foreach ($line in Get-Content -LiteralPath $Path)
    {
        if ($line -match '^\s*#\s*rt_stats_dump_start\b(.*)$')
        {
            if ($null -ne $header) { break }
            $header = $Matches[1].Trim()
            continue
        }

        if ($null -eq $header) { continue }

        if ($line -match '^\s*#')
        {
            if ($null -eq $columns) { continue }
            if ($line -match 'rt_stats_dump_start') { Write-Warning "$Path : another recording follows; only the first is read" }
            break
        }

        if ([string]::IsNullOrWhiteSpace($line)) { continue }

        if ($null -eq $columns)
        {
            $columns = @($line.Split(',') | ForEach-Object { $_.Trim() })
            continue
        }

        if ($rows.Count -eq 0 -and $line.StartsWith(','))
        {
            $extra = @($line.Split(',') | Select-Object -Skip 1 | ForEach-Object { $_.Trim() })
            if ($extra.Count -gt 0 -and -not [string]::IsNullOrWhiteSpace($extra[0]))
            {
                $columns += $extra
                continue
            }
        }

        [void]$rows.Add(@($line.Split(',') | ForEach-Object { $_.Trim() }))
    }

    if ($null -eq $header) { throw "not an rt_stats_dump_start recording (no header line): $Path" }
    if ($null -eq $columns) { throw "no CSV column header after the rt_stats_dump_start line: $Path" }

    $panels = $null
    $samples = $null
    $interval = $null
    $duration = $null

    if ($header -match 'panels\s+(\d+)') { $panels = [int]$Matches[1] }
    if ($header -match 'samples\s+(\d+)') { $samples = [int]$Matches[1] }
    if ($header -match 'interval\s+([0-9.]+)') { $interval = [double]::Parse($Matches[1], $invariant) }
    if ($header -match 'duration\s+([0-9.]+)') { $duration = [double]::Parse($Matches[1], $invariant) }

    if ($null -eq $samples)
    {
        Write-Warning "$Path : the header does not name a sample count"
        $samples = $rows.Count
    }
    if ($null -eq $duration)
    {
        Write-Warning "$Path : the header does not name a duration"
    }
    if ($rows.Count -ne $samples)
    {
        Write-Warning "$Path : the header says $samples samples, $($rows.Count) were read"
    }

    $columnIndex = @{}
    for ($i = 0; $i -lt $columns.Count; $i++)
    {
        $key = $columns[$i].ToLowerInvariant()
        if ([string]::IsNullOrWhiteSpace($key)) { continue }
        if ($columnIndex.ContainsKey($key))
        {
            Write-Warning "$Path : duplicate column '$($columns[$i])' is ignored"
            continue
        }
        $columnIndex[$key] = $i
    }

    return [pscustomobject]@{
        Path        = $Path
        Header      = $header
        Panels      = $panels
        Samples     = $samples
        Interval    = $interval
        Duration    = $duration
        Columns     = $columns
        ColumnIndex = $columnIndex
        Rows        = $rows
    }
}

function Get-Percentile($Values, [double]$Percentile)
{
    $sorted = @($Values | Sort-Object)
    if ($sorted.Count -eq 0) { return $null }

    $rank = [int][math]::Ceiling($Percentile * $sorted.Count) - 1
    if ($rank -lt 0) { $rank = 0 }
    if ($rank -ge $sorted.Count) { $rank = $sorted.Count - 1 }
    return $sorted[$rank]
}

function Get-ArmMetric($Dump, [string]$Metric)
{
    $stats = [pscustomobject]@{
        Status    = 'ok'
        Values    = @()
        Missing   = $Dump.Rows.Count
        Unparsed  = 0
        Mean      = $null
        P95       = $null
        DeltaMean = $null
        DeltaP95  = $null
    }

    $key = $Metric.ToLowerInvariant()
    if (-not $Dump.ColumnIndex.ContainsKey($key))
    {
        $stats.Status = 'missing'
        return $stats
    }

    $index = $Dump.ColumnIndex[$key]
    $values = New-Object System.Collections.ArrayList
    $missing = 0
    $unparsed = 0

    foreach ($row in $Dump.Rows)
    {
        if ($index -ge $row.Count -or [string]::IsNullOrWhiteSpace($row[$index]))
        {
            $missing++
            continue
        }

        $value = 0.0
        if ([double]::TryParse($row[$index], [System.Globalization.NumberStyles]::Float, $invariant, [ref]$value))
        {
            [void]$values.Add($value)
        }
        else
        {
            $unparsed++
        }
    }

    $stats.Values = @($values)
    $stats.Missing = $missing
    $stats.Unparsed = $unparsed

    if ($values.Count -eq 0)
    {
        $stats.Status = 'empty'
        return $stats
    }

    $sum = 0.0
    foreach ($value in $values) { $sum += $value }

    $stats.Mean = $sum / $values.Count
    $stats.P95 = Get-Percentile $values 0.95
    return $stats
}

function Format-Number($Value, [string]$Format)
{
    if ($null -eq $Value) { return '-' }
    return ([double]$Value).ToString($Format, $invariant)
}

function Format-Delta($Value)
{
    if ($null -eq $Value) { return '-' }
    return ([double]$Value).ToString('+0.0;-0.0;0.0', $invariant) + '%'
}

function Format-CsvField($Value)
{
    $text = [string]$Value
    if ($text.IndexOf(',') -ge 0 -or $text.IndexOf('"') -ge 0)
    {
        return '"' + $text.Replace('"', '""') + '"'
    }
    return $text
}

function Format-CsvNumber($Value)
{
    if ($null -eq $Value) { return '' }
    return ([double]$Value).ToString('0.######', $invariant)
}

function Format-CsvDelta($Value)
{
    if ($null -eq $Value) { return '' }
    return ([double]$Value).ToString('0.##', $invariant)
}

function Get-ArmRole($Record, $BaselineRecord)
{
    if ($Record.Name -ieq $BaselineRecord.Name) { return 'baseline' }
    return 'arm'
}

$armsList = ConvertTo-ArmList $Arms
if ($null -eq $armsList -or $armsList.Count -eq 0) { throw "-Arms is empty" }

$seenNames = @{}
foreach ($arm in $armsList)
{
    if ([string]::IsNullOrWhiteSpace($arm.Name)) { throw "an arm has no name" }
    if ([string]::IsNullOrWhiteSpace($arm.Path)) { throw "arm '$($arm.Name)' has no dump path" }

    $key = $arm.Name.ToLowerInvariant()
    if ($seenNames.ContainsKey($key)) { throw "duplicate arm name '$($arm.Name)'" }
    $seenNames[$key] = $true
}

$armRecords = New-Object System.Collections.ArrayList
foreach ($arm in $armsList)
{
    $dump = Read-StatsDump $arm.Path
    $record = [pscustomobject]@{
        Name    = $arm.Name
        Path    = $arm.Path
        Dump    = $dump
        Metrics = [ordered]@{}
    }

    foreach ($metric in $metricList)
    {
        $record.Metrics[$metric.Name] = Get-ArmMetric -Dump $dump -Metric $metric.Name
    }

    [void]$armRecords.Add($record)
}

if ([string]::IsNullOrWhiteSpace($Baseline))
{
    $Baseline = $armRecords[0].Name
    Write-Warning "-Baseline was not given; comparing against the first arm '$Baseline'"
}

$baselineRecord = $null
foreach ($record in $armRecords)
{
    if ($record.Name -ieq $Baseline) { $baselineRecord = $record; break }
}
if ($null -eq $baselineRecord)
{
    $armNames = ($armRecords | ForEach-Object { $_.Name }) -join ', '
    throw "baseline arm '$Baseline' is not among -Arms: $armNames"
}

$orderedRecords = New-Object System.Collections.ArrayList
[void]$orderedRecords.Add($baselineRecord)
foreach ($record in $armRecords)
{
    if ($record -ne $baselineRecord) { [void]$orderedRecords.Add($record) }
}
$armRecords = $orderedRecords

foreach ($record in $armRecords)
{
    foreach ($metric in $metricList)
    {
        $stats = $record.Metrics[$metric.Name]

        if ($stats.Status -eq 'missing')
        {
            Write-Warning "arm '$($record.Name)': column '$($metric.Name)' is absent from the header of $($record.Path)"
        }
        elseif ($stats.Status -eq 'empty')
        {
            Write-Warning "arm '$($record.Name)': column '$($metric.Name)' has no values in $($record.Dump.Rows.Count) samples (the source was off?)"
        }
        elseif ($stats.Missing -gt 0)
        {
            Write-Warning "arm '$($record.Name)': column '$($metric.Name)' has $($stats.Missing) empty value(s) out of $($record.Dump.Rows.Count) samples"
        }

        if ($stats.Unparsed -gt 0)
        {
            Write-Warning "arm '$($record.Name)': $($stats.Unparsed) value(s) in column '$($metric.Name)' could not be parsed"
        }
    }
}

$notes = New-Object System.Collections.ArrayList
foreach ($record in $armRecords)
{
    if ($record.Name -ieq $baselineRecord.Name) { continue }

    foreach ($metric in $metricList)
    {
        $stats = $record.Metrics[$metric.Name]
        $baseStats = $baselineRecord.Metrics[$metric.Name]

        if ($stats.Status -ne 'ok' -or $baseStats.Status -ne 'ok') { continue }

        if ($baseStats.Mean -le 0.0)
        {
            $note = "baseline mean of '$($metric.Name)' is $([math]::Round($baseStats.Mean, 3)); deltas are not defined"
            if ($notes -notcontains $note) { [void]$notes.Add($note) }
            continue
        }

        $stats.DeltaMean = ($stats.Mean / $baseStats.Mean - 1.0) * 100.0
        if ($baseStats.P95 -gt 0.0)
        {
            $stats.DeltaP95 = ($stats.P95 / $baseStats.P95 - 1.0) * 100.0
        }
    }
}

$missingLines = New-Object System.Collections.ArrayList
foreach ($record in $armRecords)
{
    foreach ($metric in $metricList)
    {
        $stats = $record.Metrics[$metric.Name]

        if ($stats.Status -eq 'missing')
        {
            [void]$missingLines.Add("- arm '$($record.Name)': column '$($metric.Name)' absent from the header")
        }
        elseif ($stats.Status -eq 'empty')
        {
            [void]$missingLines.Add("- arm '$($record.Name)': column '$($metric.Name)' present but no values in $($record.Dump.Rows.Count) samples")
        }
        elseif ($stats.Missing -gt 0)
        {
            [void]$missingLines.Add("- arm '$($record.Name)': column '$($metric.Name)' has $($stats.Missing) empty value(s) out of $($record.Dump.Rows.Count) samples")
        }

        if ($stats.Unparsed -gt 0)
        {
            [void]$missingLines.Add("- arm '$($record.Name)': column '$($metric.Name)' has $($stats.Unparsed) unparseable value(s)")
        }
    }
}
foreach ($note in $notes) { [void]$missingLines.Add("- $note") }
if ($missingLines.Count -eq 0) { [void]$missingLines.Add('- none') }

if ($Csv)
{
    $lines = New-Object System.Collections.ArrayList
    [void]$lines.Add('arm,role,file,panels,header_samples,rows,duration_s,interval_s,metric,unit,status,values,missing,unparsed,mean,p95,delta_mean_pct,delta_p95_pct,note')

    foreach ($record in $armRecords)
    {
        $role = Get-ArmRole $record $baselineRecord

        foreach ($metric in $metricList)
        {
            $stats = $record.Metrics[$metric.Name]
            $rowNote = ''

            if ($stats.Status -eq 'missing') { $rowNote = 'column absent from the header' }
            elseif ($stats.Status -eq 'empty') { $rowNote = 'no values in the samples' }
            elseif ($stats.Missing -gt 0) { $rowNote = "$($stats.Missing) empty value(s) out of $($record.Dump.Rows.Count) samples" }
            if ($stats.Unparsed -gt 0)
            {
                if ($rowNote) { $rowNote = "$rowNote; unparseable values" } else { $rowNote = 'unparseable values' }
            }

            if ($role -ne 'baseline' -and $stats.Status -eq 'ok' -and $null -eq $stats.DeltaMean)
            {
                $baseStats = $baselineRecord.Metrics[$metric.Name]
                $deltaNote = 'baseline has no values'
                if ($baseStats.Status -eq 'ok' -and $baseStats.Mean -le 0.0)
                {
                    $deltaNote = 'baseline mean is zero; delta undefined'
                }
                if ($rowNote) { $rowNote = "$rowNote; $deltaNote" } else { $rowNote = $deltaNote }
            }

            $cells = @(
                (Format-CsvField $record.Name)
                (Format-CsvField $role)
                (Format-CsvField $record.Path)
                (Format-CsvNumber $record.Dump.Panels)
                (Format-CsvNumber $record.Dump.Samples)
                (Format-CsvNumber $record.Dump.Rows.Count)
                (Format-CsvNumber $record.Dump.Duration)
                (Format-CsvNumber $record.Dump.Interval)
                (Format-CsvField $metric.Name)
                (Format-CsvField $metric.Unit)
                (Format-CsvField $stats.Status)
                (Format-CsvNumber $stats.Values.Count)
                (Format-CsvNumber $stats.Missing)
                (Format-CsvNumber $stats.Unparsed)
                (Format-CsvNumber $stats.Mean)
                (Format-CsvNumber $stats.P95)
                (Format-CsvDelta $stats.DeltaMean)
                (Format-CsvDelta $stats.DeltaP95)
                (Format-CsvField $rowNote)
            )
            [void]$lines.Add(($cells -join ','))
        }
    }
}
else
{
    $lines = New-Object System.Collections.ArrayList
    [void]$lines.Add('# Particle attribution')
    [void]$lines.Add('')
    [void]$lines.Add("- baseline: $($baselineRecord.Name) ($($baselineRecord.Path))")
    [void]$lines.Add("- generated: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')")
    [void]$lines.Add('')
    [void]$lines.Add('The cpu.* columns are per-window maxima (rt_stats_interval), so the mean and p95 below are statistics of those maxima, not of per-frame times.')
    [void]$lines.Add('')
    [void]$lines.Add('## Samples')
    [void]$lines.Add('')
    [void]$lines.Add('| arm | role | file | panels | header samples | rows | duration s | interval s |')
    [void]$lines.Add('| --- | --- | --- | ---: | ---: | ---: | ---: | ---: |')

    foreach ($record in $armRecords)
    {
        $role = Get-ArmRole $record $baselineRecord
        $cells = @(
            $record.Name
            $role
            $record.Path
            (Format-Number $record.Dump.Panels '0')
            (Format-Number $record.Dump.Samples '0')
            (Format-Number $record.Dump.Rows.Count '0')
            (Format-Number $record.Dump.Duration '0.00')
            (Format-Number $record.Dump.Interval '0.000')
        )
        [void]$lines.Add('| ' + ($cells -join ' | ') + ' |')
    }

    [void]$lines.Add('')
    [void]$lines.Add('## Metrics')
    [void]$lines.Add('')

    $header = @('metric', 'unit')
    $align = @('---', '---')
    foreach ($record in $armRecords)
    {
        $header += "$($record.Name) mean"
        $header += "$($record.Name) p95"
        $align += '---:'
        $align += '---:'

        if ($record.Name -ine $baselineRecord.Name)
        {
            $header += "$($record.Name) delta mean"
            $header += "$($record.Name) delta p95"
            $align += '---:'
            $align += '---:'
        }
    }
    [void]$lines.Add('| ' + ($header -join ' | ') + ' |')
    [void]$lines.Add('| ' + ($align -join ' | ') + ' |')

    foreach ($metric in $metricList)
    {
        $cells = @($metric.Name, $metric.Unit)

        foreach ($record in $armRecords)
        {
            $stats = $record.Metrics[$metric.Name]
            $cells += (Format-Number $stats.Mean $metric.Format)
            $cells += (Format-Number $stats.P95 $metric.Format)

            if ($record.Name -ine $baselineRecord.Name)
            {
                $cells += (Format-Delta $stats.DeltaMean)
                $cells += (Format-Delta $stats.DeltaP95)
            }
        }

        [void]$lines.Add('| ' + ($cells -join ' | ') + ' |')
    }

    [void]$lines.Add('')
    [void]$lines.Add('## Missing data')
    [void]$lines.Add('')
    foreach ($missingLine in $missingLines) { [void]$lines.Add($missingLine) }
}

$text = ($lines -join [Environment]::NewLine) + [Environment]::NewLine

if (-not [string]::IsNullOrWhiteSpace($Output))
{
    $parent = Split-Path -Parent $Output
    if (-not [string]::IsNullOrWhiteSpace($parent) -and -not (Test-Path -LiteralPath $parent))
    {
        [void](New-Item -ItemType Directory -Path $parent)
    }

    Set-Content -LiteralPath $Output -Value $text -Encoding UTF8
    Write-Host "report written: $Output"
}

Write-Host $text
