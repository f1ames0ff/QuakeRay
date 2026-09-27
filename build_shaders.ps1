
param(
    [switch]$Rebuild,
    [switch]$GenCommon,
    [string]$DestDir = ""
)

$ErrorActionPreference = "Stop"

$shaderSrc = Join-Path $PSScriptRoot "vkpt\Source\Shaders"
$shaderOut = Join-Path $PSScriptRoot "vkpt\Build"
if (-not $DestDir) { $DestDir = Join-Path $PSScriptRoot "build\Debug\id1\shaders" }
$destDir   = $DestDir

# The generator rebuilds a shader when a file it depends on is newer than the .spv
# it produced, and the headers the shaders include (CloudLayer.h and the rest, which
# sit next to the sources) have been outside that check: a change in one of them has
# shipped stale .spv files more than once, with a build that reported success. When
# any header in the source folder is newer than the oldest built shader, throw them
# all away and let the generator build them again. The same is done for every build
# that asks for a rebuild.
$headers = @(Get-ChildItem -Path (Join-Path $shaderSrc "*.h") -ErrorAction SilentlyContinue)
$built   = @(Get-ChildItem -Path (Join-Path $shaderOut "*.spv") -ErrorAction SilentlyContinue)
if ($headers.Count -gt 0 -and $built.Count -gt 0)
{
    $newestHeader = ($headers | Sort-Object LastWriteTime -Descending)[0]
    $oldestBuild  = ($built   | Sort-Object LastWriteTime)[0]

    if ($newestHeader.LastWriteTime -gt $oldestBuild.LastWriteTime)
    {
        Write-Host "Shader header $($newestHeader.Name) is newer than the built shaders: rebuilding all of them." -ForegroundColor Yellow
        Remove-Item (Join-Path $shaderOut "*.spv") -Force
    }
}
if ($Rebuild -and $built.Count -gt 0)
{
    Remove-Item (Join-Path $shaderOut "*.spv") -Force
}

if ($env:VULKAN_SDK) {
    $sdkBin = Join-Path $env:VULKAN_SDK "Bin"
    if (Test-Path (Join-Path $sdkBin "glslc.exe")) {
        $env:PATH = "$sdkBin;$env:PATH"
    }
}
if (-not (Get-Command glslc -ErrorAction SilentlyContinue)) {
    throw "glslc not found. Install the Vulkan SDK or set VULKAN_SDK."
}

# dxc ships with the Vulkan SDK; the Windows SDK also has one. It is only required while HLSL
# shaders exist, i.e. from the first ported file until the GLSL sources are gone.
$hlslSources = @(Get-ChildItem -Path $shaderSrc -Filter "*.hlsl" -Recurse -ErrorAction SilentlyContinue)
if ($hlslSources.Count -gt 0 -and -not (Get-Command dxc -ErrorAction SilentlyContinue)) {
    throw ("dxc not found, but $($hlslSources.Count) HLSL shader file(s) are present. " +
           "Install the Vulkan SDK or set VULKAN_SDK.")
}

if ($GenCommon) {
    Push-Location (Join-Path $PSScriptRoot "vkpt\Source\Generated")
    try {
        python GenerateShaderCommon.py --path .
        if ($LASTEXITCODE -ne 0) { throw "GenerateShaderCommon.py failed (exit $LASTEXITCODE)." }
    }
    finally {
        Pop-Location
    }
}

$genArgs = @()
if ($GenCommon) { $genArgs += "-gencomm" }
if ($Rebuild)  { $genArgs += "-rebuild" }
$genArgs += "-psout"

Push-Location $shaderSrc
try {
    $genOutput = python GenerateShaders.py @genArgs 2>&1
    $genOutput | Write-Host
    if ($LASTEXITCODE -ne 0) { throw "GenerateShaders.py failed (exit $LASTEXITCODE)." }
    # A shader that fails to compile is reported by the generator, and the run still
    # ends with a zero exit code: from here it looked exactly like a success with
    # fewer files. Worse, the deploy below then found no .spv for the failed shader
    # and removed the one in the game's folder -- a game left without a shader, and a
    # build that said nothing was wrong. The report is part of the run now, and a
    # failure stops it before anything is deployed or removed.
    if ($genOutput -match 'shader builds failed') {
        throw "GenerateShaders.py reported a failed shader build (see above)."
    }
}
finally {
    Pop-Location
}

if (-not (Test-Path $destDir)) {
    New-Item -ItemType Directory -Path $destDir -Force | Out-Null
}

$srcFiles = @(Get-ChildItem -Path (Join-Path $shaderOut "*.spv") -ErrorAction SilentlyContinue)
if ($srcFiles.Count -eq 0) {
    throw "No SPIR-V was produced in $shaderOut; $destDir is left untouched."
}

$srcNames = @($srcFiles | Select-Object -ExpandProperty Name)

$stale = Get-ChildItem -Path (Join-Path $destDir "*.spv") | Where-Object { $srcNames -notcontains $_.Name }
foreach ($f in $stale) {
    Write-Host "Removing stale shader: $($f.Name)" -ForegroundColor Yellow
    Remove-Item $f.FullName -Force
}

$copied = Copy-Item -Path (Join-Path $shaderOut "*.spv") -Destination $destDir -Force -PassThru
Write-Host "Deployed $($copied.Count) shader(s) to $destDir" -ForegroundColor Green
