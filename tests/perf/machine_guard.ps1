function Enter-QuakeRayMachine {
    param([ValidateRange(0, 300)][int]$TimeoutSeconds = 5)
    $mutex = [System.Threading.Mutex]::new($false, 'Local\QuakeRayPerformanceRun')
    $locked = $false
    try {
        try { $locked = $mutex.WaitOne([TimeSpan]::FromSeconds($TimeoutSeconds)) }
        catch [System.Threading.AbandonedMutexException] { $locked = $true }
        if (-not $locked) { throw 'MACHINE_BUSY: QuakeRay machine guard is occupied; defer this operation.' }
        $busy = @(Get-Process -ErrorAction SilentlyContinue | Where-Object {
            $_.ProcessName -match '^(quakeray|qray_|cmake$|ninja$|cl$|link$)'
        })
        if ($busy.Count) { throw "MACHINE_BUSY: foreign game/build processes are running: $($busy.Id -join ', ')." }
        $ancestors = @($PID)
        $parent = Get-CimInstance Win32_Process -Filter "ProcessId=$PID"
        while ($parent -and $parent.ParentProcessId -gt 0 -and $parent.ParentProcessId -notin $ancestors) {
            $ancestors += $parent.ParentProcessId
            $parent = Get-CimInstance Win32_Process -Filter "ProcessId=$($parent.ParentProcessId)"
        }
        $runners = @(Get-CimInstance Win32_Process -Filter "Name='powershell.exe' OR Name='pwsh.exe'" |
            Where-Object { $_.ProcessId -notin $ancestors -and $_.CommandLine -match 'run_(place|menu)\.ps1|build_win\.ps1' })
        if ($runners.Count) { throw 'MACHINE_BUSY: a non-cooperating runner/build owns the machine.' }
        return $mutex
    } catch {
        if ($locked) { $mutex.ReleaseMutex() }
        $mutex.Dispose()
        throw
    }
}

function Exit-QuakeRayMachine {
    param($Guard)
    if ($null -ne $Guard) {
        $Guard.ReleaseMutex()
        $Guard.Dispose()
    }
}

function Test-QuakeRayStopRequested {
    return $env:QUAKERAY_JOB_STOP_FILE -and (Test-Path -LiteralPath $env:QUAKERAY_JOB_STOP_FILE)
}
