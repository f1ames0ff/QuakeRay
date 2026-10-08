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
