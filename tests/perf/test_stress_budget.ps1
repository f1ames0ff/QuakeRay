# Copyright (c) 2026 f1ames0ff <f1am3sdev.github@protonmail.com>
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along
# with this program; if not, write to the Free Software Foundation, Inc.,
# 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
#

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'stress_budget.ps1')

function Assert-Budget([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

$start = [DateTime]::Parse('2026-10-08T10:00:00')
$deadline = $start.AddSeconds(300)
Assert-Budget (Test-StressBudget -Deadline $null -RequiredSeconds 30 -Now $start) 'The first launch may establish its budget.'
Assert-Budget (Test-StressBudget -Deadline $deadline -RequiredSeconds 25 -Now $start.AddSeconds(275)) 'A complete capture fits at the exact boundary.'
Assert-Budget (-not (Test-StressBudget -Deadline $deadline -RequiredSeconds 25 -Now $start.AddSeconds(280))) 'A repeat with only 20 seconds must be deferred before launch.'
Assert-Budget (-not (Test-StressBudget -Deadline $deadline -RequiredSeconds 40 -Now $start.AddSeconds(299))) 'A delayed start marker cannot admit a full capture.'
Assert-Budget ((Get-StressDeadline -Deadline $deadline -MaximumSeconds 300 -Now $start.AddSeconds(270)) -eq $deadline) 'Owner waits cannot extend an existing deadline.'
Assert-Budget ((Get-StressDeadline -Deadline $deadline.AddSeconds(-15) -MaximumSeconds 25 -Now $start.AddSeconds(280)) -eq $start.AddSeconds(285)) 'Capture deadlines leave shutdown time.'
Assert-Budget ((Get-StressExitTimeout -Deadline $deadline -Now $start.AddSeconds(299)) -eq 1000) 'Shutdown uses only the remaining second.'
Assert-Budget ((Get-StressExitTimeout -Deadline $deadline -Now $start.AddSeconds(301)) -eq 0) 'Expired shutdown waits are immediate.'
Assert-Budget ((Get-StressExitTimeout -Deadline $deadline -Now $start) -eq 15000) 'Early shutdown waits retain their 15-second cap.'
Assert-Budget (-not (Test-StressExternalRunnerOwner 'powershell.exe -File C:\tests\perf\run_stress.ps1')) 'Mutex waiters and analysis-only stress runners are not active owners.'
Assert-Budget (Test-StressExternalRunnerOwner 'powershell.exe -File C:\tests\perf\run_place.ps1') 'External non-cooperating performance runners remain owners.'
Assert-Budget (-not (Test-StressExternalRunnerOwner 'powershell.exe -File C:\tests\perf\analyze.ps1')) 'Unrelated processes are not performance owners.'
$admissionDeadline = $start.AddSeconds(32)
Assert-Budget (Test-StressBudget -Deadline $admissionDeadline -RequiredSeconds 30 -Now $start) 'A repeat may initially fit before preparation.'
Assert-Budget (-not (Test-StressBudget -Deadline $admissionDeadline -RequiredSeconds 30 -Now $start.AddSeconds(4))) 'Admission must be checked again after preparation.'
try {
    & (Join-Path $PSScriptRoot 'run_stress.ps1') -MaxRunSeconds 30 -Warmup 30 -Seconds 25
    throw 'An impossible first-attempt budget was accepted.'
} catch {
    Assert-Budget ($_.Exception.Message -eq 'Warmup, capture and shutdown reserve cannot fit within MaxRunSeconds.') 'Impossible parameter combinations are rejected before ownership waits or launch.'
}
Write-Output 'PASS: short-run budget and deferral tests.'
