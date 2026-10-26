$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'machine_guard.ps1')

$currentAncestorIds = @(Get-QuakeRayProcessAncestorIds)
if ($currentAncestorIds -notcontains $PID) {
    throw 'Process ancestor enumeration omitted the current process.'
}
$currentParentId = [int][QuakeRayMachineGuardProcessTree]::GetParentProcessId([uint32]$PID)
if ($currentParentId -gt 0 -and
    $null -ne (Get-Process -Id $currentParentId -ErrorAction SilentlyContinue) -and
    $currentAncestorIds -notcontains $currentParentId) {
    throw 'Process ancestor enumeration omitted the live parent process.'
}
Write-Output 'PASS: process ancestor enumeration includes the current process and live parent when available.'

$activeDisplays = @(Get-QuakeRayActiveDisplays)
if (-not $activeDisplays.Count) {
    Write-Output 'SKIP: active display enumeration (no active interactive display was detected).'
} else {
    $validDisplays = @($activeDisplays | Where-Object {
        $_.Width -gt 0 -and $_.Height -gt 0 -and $_.RefreshHz -is [ValueType]
    })
    if (-not $validDisplays.Count) {
        throw 'Active display enumeration did not return positive dimensions and refresh metadata.'
    }
    Write-Output "PASS: active display enumeration returned $($validDisplays.Count) display(s) with dimensions and refresh metadata."
}

$externalRunners = @(Get-QuakeRayExternalRunnerProcesses -ExcludedProcessIds $currentAncestorIds)
Write-Output "PASS: external runner enumeration completed with the current process tree excluded ($($externalRunners.Count) candidate(s))."

$syntheticCounters = @(
    [pscustomobject]@{ InstanceName = "pid_$PID`_luid_0x0_phys_0_eng_0"; UtilizationPercentage = 17 },
    [pscustomobject]@{ InstanceName = "pid_$PID`_luid_0x0_phys_0_eng_1"; UtilizationPercentage = 42 },
    [pscustomobject]@{ InstanceName = 'not-a-process-engine'; UtilizationPercentage = 99 }
)
$usage = @(ConvertFrom-QuakeRayGpuEngineCounters -Counters $syntheticCounters)
if ($usage.Count -ne 1 -or $usage[0].ProcessId -ne $PID -or
    $usage[0].PeakUtilizationPercent -ne 42 -or $usage[0].EngineCount -ne 2) {
    throw 'GPU engine counter parsing or grouping failed.'
}
$selected = @(Select-QuakeRayGpuBusyProcesses -Usage $usage -MinimumUtilizationPercent 40)
if ($selected.Count -ne 1 -or $selected[0].ProcessId -ne $PID) {
    throw 'GPU busy-process threshold selection failed.'
}
$excluded = @(Select-QuakeRayGpuBusyProcesses -Usage $usage -MinimumUtilizationPercent 40 -ExcludedProcessIds @($PID))
if ($excluded.Count) { throw 'GPU guard did not exclude the current process tree root.' }

$script:gpuSampleIndex = 0
$script:gpuSamples = @(
    @([pscustomobject]@{ InstanceName = "pid_$PID`_luid_0x0_phys_0_eng_0"; UtilizationPercentage = 50 }),
    @([pscustomobject]@{ InstanceName = "pid_$PID`_luid_0x0_phys_0_eng_0"; UtilizationPercentage = 50 })
)
$counterProvider = {
    $sample = $script:gpuSamples[$script:gpuSampleIndex]
    $script:gpuSampleIndex++
    return $sample
}
$sustained = @(Get-QuakeRayGpuBusyProcesses -MinimumUtilizationPercent 40 -Samples 2 `
    -SampleIntervalMilliseconds 0 -CounterProvider $counterProvider)
if ($sustained.Count -ne 1 -or $sustained[0].ProcessId -ne $PID) {
    throw 'GPU guard did not recognize sustained foreign activity.'
}
$script:gpuSampleIndex = 0
$script:gpuSamples[1][0].UtilizationPercentage = 0
$transient = @(Get-QuakeRayGpuBusyProcesses -MinimumUtilizationPercent 40 -Samples 2 `
    -SampleIntervalMilliseconds 0 -CounterProvider $counterProvider)
if ($transient.Count) { throw 'GPU guard admitted transient GPU activity.' }

$busy = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -match '^(quakeray|qray_|cmake$|ninja$|cl$|link$)' })
if ($busy.Count) {
    try {
        $unexpected = Enter-QuakeRayMachine -TimeoutSeconds 0
        Exit-QuakeRayMachine $unexpected
        throw 'Guard admitted a foreign game/build.'
    } catch {
        if ($_.Exception.Message -notlike 'MACHINE_BUSY:*') { throw }
    }
    Write-Output 'PASS: foreign owner rejected; reentrancy check deferred while machine is occupied.'
    exit 0
}
$first = Enter-QuakeRayMachine -TimeoutSeconds 0
try {
    $second = Enter-QuakeRayMachine -TimeoutSeconds 0
    Exit-QuakeRayMachine $second
    $scriptPath = Join-Path $PSScriptRoot 'machine_guard.ps1'
    $command = ". '$scriptPath'; try { `$g = Enter-QuakeRayMachine -TimeoutSeconds 0; Exit-QuakeRayMachine `$g; exit 2 } catch { if (`$_.Exception.Message -like 'MACHINE_BUSY:*') { exit 0 }; exit 3 }"
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($command))
    & powershell -NoProfile -NonInteractive -EncodedCommand $encoded
    if ($LASTEXITCODE -ne 0) { throw 'Cross-process machine exclusion failed.' }
} finally {
    Exit-QuakeRayMachine $first
}
Write-Output 'PASS: same-thread reentrancy and cross-process machine exclusion.'
