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

function Test-StressBudget {
    param(
        [AllowNull()][Nullable[DateTime]]$Deadline,
        [double]$RequiredSeconds,
        [DateTime]$Now = (Get-Date)
    )
    return $null -eq $Deadline -or ($Deadline - $Now).TotalSeconds -ge $RequiredSeconds
}

function Get-StressDeadline {
    param(
        [AllowNull()][Nullable[DateTime]]$Deadline,
        [double]$MaximumSeconds,
        [DateTime]$Now = (Get-Date)
    )
    $limit = $Now.AddSeconds($MaximumSeconds)
    if ($null -ne $Deadline -and $Deadline -lt $limit) { return [DateTime]$Deadline }
    return $limit
}

function Get-StressExitTimeout {
    param(
        [DateTime]$Deadline,
        [DateTime]$Now = (Get-Date)
    )
    return [int][Math]::Max(0, [Math]::Min(15000, [Math]::Floor(($Deadline - $Now).TotalMilliseconds)))
}

function Test-StressExternalRunnerOwner {
    param([AllowNull()][string]$CommandLine)
    $pattern = 'run_' + '(menu|place|stress|audit)'
    return $CommandLine -match $pattern -and $CommandLine -notmatch 'run_stress\.ps1'
}
