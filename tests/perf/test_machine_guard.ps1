$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'machine_guard.ps1')
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
