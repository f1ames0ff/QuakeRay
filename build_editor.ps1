param(
    [string]$Config = "Debug",
    [string]$BuildDir = "",
    [int]$Parallel = 0,
    [switch]$SkipShaders
)

$ErrorActionPreference = "Stop"

$root = $PSScriptRoot
Set-Location $root
[Environment]::CurrentDirectory = $root

if (-not (Test-Path (Join-Path $root "renderer\CMakeLists.txt"))) {
    throw "The renderer SDK submodule is missing. Run: git submodule update --init --recursive"
}

if (-not $BuildDir) {
    $BuildDir = Join-Path $root "build\editor"
} elseif (-not [System.IO.Path]::IsPathRooted($BuildDir)) {
    $BuildDir = Join-Path $root $BuildDir
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found. Install Visual Studio Build Tools with the C++ workload."
}

$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) {
    throw "Visual Studio Build Tools with the C++ workload are not installed."
}

$systemRoot = if ($env:SystemRoot) { $env:SystemRoot } else { "C:\Windows" }
$system32 = Join-Path $systemRoot "System32"
if ($env:PATH -notlike "*$system32*") {
    $env:PATH = "$system32;$env:PATH"
}

$cmdExe = $env:ComSpec
if (-not $cmdExe -or -not (Test-Path $cmdExe)) {
    $cmdExe = Join-Path $system32 "cmd.exe"
}

$devCmd = Join-Path $vsPath "Common7\Tools\VsDevCmd.bat"
$envLines = & $cmdExe /c "`"$devCmd`" -arch=x64 -host_arch=x64 >nul 2>&1 && set"
foreach ($line in $envLines) {
    if ($line -match "^([^=]+)=(.*)$") {
        [Environment]::SetEnvironmentVariable($matches[1], $matches[2], "Process")
    }
}

function Invoke-GitQuietly([string[]]$Arguments)
{
    $previous = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try
    {
        & git @Arguments *> $null
        return $LASTEXITCODE
    }
    finally
    {
        $ErrorActionPreference = $previous
    }
}

$nvrhiDir = Join-Path $root "third_party\nvrhi"
$nvrhiPatch = Join-Path $root "third_party\nvrhi-max-binding-layouts.patch"
$nvrhiPatchedHere = $false

if ((Test-Path $nvrhiPatch) -and (Test-Path (Join-Path $nvrhiDir "include\nvrhi\nvrhi.h")))
{
    if ((Invoke-GitQuietly @("-C", $nvrhiDir, "apply", "--check", "--reverse", $nvrhiPatch)) -eq 0)
    {
        Write-Host "NVRHI patch is already applied, leaving it in place" -ForegroundColor Yellow
    }
    else
    {
        if ((Invoke-GitQuietly @("-C", $nvrhiDir, "apply", $nvrhiPatch)) -ne 0)
        {
            throw "Failed to apply $nvrhiPatch to third_party/nvrhi."
        }

        $nvrhiPatchedHere = $true
        Write-Host "Applied the NVRHI patch for the editor build" -ForegroundColor Yellow
    }
}

try
{
    $shaderDir = Join-Path $BuildDir "shaders"

    if (-not $SkipShaders)
    {
        & (Join-Path $root "build_shaders.ps1") -Rebuild -DestDir $shaderDir
        if ($LASTEXITCODE -ne 0) {
            throw "Shader build failed (exit $LASTEXITCODE)."
        }
    } elseif (-not (Test-Path $shaderDir)) {
        throw "No shaders at $shaderDir. Run without -SkipShaders first."
    }

    $cmakeArgs = @(
        "-S", (Join-Path $root "renderer"),
        "-B", $BuildDir,
        "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=$Config",
        "-DQR_BUILD_EDITOR=ON",
        "-DQR_NVRHI_SOURCE_DIR=$root\third_party\nvrhi",
        "-DQR_EDITOR_SHADER_DIR=$shaderDir",
        "-DQR_EDITOR_TEXTURE_DIR=$root\gfx\textures"
    )

    cmake @cmakeArgs
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configure failed (exit $LASTEXITCODE)."
    }

    $buildArgs = @("--build", $BuildDir, "--target", "quakerayeditor")
    if ($Parallel -gt 0) {
        $buildArgs += "--parallel"
        $buildArgs += "$Parallel"
    }

    cmake @buildArgs
    if ($LASTEXITCODE -ne 0) {
        throw "Editor build failed (exit $LASTEXITCODE)."
    }

    $exe = Join-Path $BuildDir "editor\quakerayeditor.exe"
    Write-Host "Built: $exe" -ForegroundColor Green
    Write-Host "Run:   cd `"$(Split-Path $exe)`"; .\quakerayeditor.exe" -ForegroundColor Green
}
finally
{
    if ($nvrhiPatchedHere)
    {
        if ((Invoke-GitQuietly @("-C", $nvrhiDir, "apply", "--reverse", $nvrhiPatch)) -eq 0)
        {
            Write-Host "Reverted the NVRHI patch" -ForegroundColor Yellow
        }
        else
        {
            Write-Warning "Failed to revert $nvrhiPatch; third_party/nvrhi is left dirty."
        }
    }
}
