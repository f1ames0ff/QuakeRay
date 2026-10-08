param([Parameter(Mandatory = $true)][string]$Spec)

$ErrorActionPreference = 'Stop'
$request = Get-Content -LiteralPath $Spec -Raw | ConvertFrom-Json
$jobDir = Split-Path -Parent $Spec
$env:QUAKERAY_JOB_STOP_FILE = Join-Path $jobDir 'stop.request'
. (Join-Path $request.repo_root 'tests\perf\machine_guard.ps1')
$guard = $null
try {
    $guard = Enter-QuakeRayMachine
    if ($request.operation -eq 'build') {
        & (Join-Path $request.repo_root 'build_win.ps1') Debug -BuildDir $request.runtime -Parallel $request.parallel -Tests:$request.tests
        if ($LASTEXITCODE -ne 0) { throw "BUILD_FAILED: exit code $LASTEXITCODE" }
        if (-not (Test-Path (Join-Path $request.runtime 'quakeray.exe')) -or
            -not (Test-Path (Join-Path $request.runtime 'id1\qray.pkz'))) { throw 'BUILD_FAILED: deployed runtime is incomplete.' }
        @{ operation = 'build'; runtime = $request.runtime; tests = 'not_run' } |
            ConvertTo-Json | Set-Content (Join-Path $jobDir 'result.json') -Encoding UTF8
    } else {
        $sandbox = Join-Path $jobDir 'runtime'
        $entries = @(Get-ChildItem -LiteralPath $request.runtime -Recurse -Force)
        if (@($entries | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) {
            throw 'PERMISSION_DENIED: runtime contains reparse points; prepare an independent runtime.'
        }
        $bytes = ($entries | Where-Object { -not $_.PSIsContainer } | Measure-Object Length -Sum).Sum
        if ($bytes -gt 8GB) { throw 'DISK_QUOTA: runtime exceeds the 8 GiB sandbox limit.' }
        New-Item -ItemType Directory -Path $sandbox | Out-Null
        Get-ChildItem -LiteralPath $request.runtime -Force | Where-Object {
            $_.Name -notmatch '^(audit-|benchmark|qconsole|crash)' -and $_.Name -notin @('qray_stage', 'renderer')
        } | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $sandbox -Recurse -Force }
        $before = @(Get-ChildItem -LiteralPath $sandbox -Recurse -File | ForEach-Object FullName)
        $remaining = [int]([DateTime]::Parse($request.deadline_utc).ToUniversalTime() - [DateTime]::UtcNow).TotalSeconds
        if ($remaining -lt 30) { throw 'RUN_TIMEOUT: runtime preparation consumed the capture budget.' }
        if ($request.operation -eq 'menu') {
            & (Join-Path $request.repo_root 'tests\perf\run_menu.ps1') -Candidate $sandbox -Seconds $request.seconds -Smoke:$request.smoke -Validation:$request.validation
        } elseif ($request.operation -in @('benchmark', 'capture')) {
            & (Join-Path $request.repo_root 'tests\perf\run_stress.ps1') -Basedir $sandbox -Saves $request.save -Presets $request.preset -Seconds $request.seconds -Warmup $request.warmup -Repeats $request.repeats -MaxRunSeconds ([Math]::Min(300, $remaining)) -StatsLevel $request.stats_level -Tag mcp
        } else { throw 'INVALID_ARGUMENT: unknown worker operation.' }
        if (-not $?) { throw 'RUN_FAILED: runner did not complete.' }
        $captures = @(Get-ChildItem -LiteralPath $sandbox -Recurse -File | Where-Object {
            $_.FullName -notin $before -and ($_.Name -like '*.frames.csv' -or $_.Name -like 'stats-*.dump' -or $_.Name -like '*.dump')
        } | ForEach-Object FullName)
        if (-not $captures.Count) { throw 'CAPTURE_MISSING: runner produced no complete capture.' }
        @{ operation = $request.operation; runtime = $sandbox; captures = $captures } |
            ConvertTo-Json | Set-Content (Join-Path $jobDir 'result.json') -Encoding UTF8
    }
} catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
} finally {
    Exit-QuakeRayMachine $guard
}
