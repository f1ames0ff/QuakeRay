
param(
    [string]$Config = "Release",
    [string]$BuildDir = "",
    [int]$Parallel = 0,
    [switch]$PkzOnly
)

$ErrorActionPreference = "Stop"

if (-not $BuildDir) {
    $BuildDir = Join-Path "build" $Config
}

if (-not $PkzOnly)
{
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
}

# Git writes to stderr when a patch does not apply, and with $ErrorActionPreference = 'Stop' a native
# command's stderr terminates the script before $LASTEXITCODE can be looked at - so the patch calls run
# through this helper, which relaxes the preference for their duration and reports through the exit code.
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

# The NVRHI pin needs one local change - the binding-layout limit raised to 16 for the ray-tracing
# pipeline - which the repository carries as third_party/nvrhi-max-binding-layouts.patch. The patch is
# applied for the duration of this build and removed again before the script ends, so the checked-out
# dependency always stays exactly what the submodule records. Any other build path (an IDE, a manual
# cmake invocation) has to apply the patch itself.
$nvrhiDir = Join-Path $PSScriptRoot "third_party\nvrhi"
$nvrhiPatch = Join-Path $PSScriptRoot "third_party\nvrhi-max-binding-layouts.patch"
$nvrhiPatchedHere = $false

if (-not $PkzOnly)
{
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
            Write-Host "Applied the NVRHI patch for this build" -ForegroundColor Yellow
        }
    }
}

$exitCode = 0

if (-not $PkzOnly)
{
    $cmakeArgs = @("-B", $BuildDir, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=$Config", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON")

    cmake @cmakeArgs
    if ($LASTEXITCODE -ne 0) { $exitCode = $LASTEXITCODE }

    if ($exitCode -eq 0)
    {
        if ($Parallel -gt 0)
        {
            cmake --build $BuildDir --parallel $Parallel
        }
        else
        {
            cmake --build $BuildDir
        }
        if ($LASTEXITCODE -ne 0) { $exitCode = $LASTEXITCODE }
    }
}
else
{
    Write-Host "Pack-only run: the engine is not rebuilt" -ForegroundColor Yellow
}

if ($exitCode -ne 0)
{
    if ($nvrhiPatchedHere)
    {
        if ((Invoke-GitQuietly @("-C", $nvrhiDir, "apply", "--reverse", $nvrhiPatch)) -eq 0)
        {
            Write-Host "Reverted the NVRHI patch" -ForegroundColor Yellow
        }
    }
    exit $exitCode
}

$srcRoot = Join-Path $PSScriptRoot "renderer\Source"
$gameDir = Join-Path $BuildDir "id1"
if (-not (Test-Path $gameDir)) { New-Item -ItemType Directory -Path $gameDir -Force | Out-Null }

# The engine assets live in one qray.pkz; loose copies left by earlier builds
# would shadow it and are removed first.
foreach ($stale in @("materials", "textures", "progs", "mdl_skins", "shaders", "gfx",
                     "BlueNoise_LDR_RGBA_128.ktx2", "WaterNormal_n.ktx2",
                     "BlueNoise_LDR_RGBA_128.png", "WaterNormal_n.png")) {
    $stalePath = Join-Path $gameDir $stale
    if (Test-Path $stalePath) { Remove-Item $stalePath -Recurse -Force }
}

$staleGfx = Join-Path $BuildDir "gfx"
if (Test-Path $staleGfx) { Remove-Item $staleGfx -Recurse -Force }
$stalePak = Join-Path $BuildDir "vkquake.pak"
if (Test-Path $stalePak) { Remove-Item $stalePak -Force }

# The material definitions are the one engine file the editor rewrites, so they
# stay loose as the gamedir's qray.materials.yaml.
Copy-Item (Join-Path $srcRoot "materials.yaml") (Join-Path $gameDir "qray.materials.yaml") -Force

$stage = Join-Path $BuildDir "qray_stage"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
$stage = (New-Item -ItemType Directory -Path $stage -Force).FullName

# All of the shaders, every time: the generator decides what to build by what it
# remembers changing, and it has let a change in a header the shaders include go by
# without rebuilding them -- stale .spv files in the game folder with a build that
# reported success, more than once. A minute of shader work per build is what that
# costs, and it is worth it.
if ($PkzOnly) {
    & (Join-Path $PSScriptRoot "build_shaders.ps1") -DestDir (Join-Path $stage "shaders")
}
else {
    & (Join-Path $PSScriptRoot "build_shaders.ps1") -Rebuild -DestDir (Join-Path $stage "shaders")
}
if ($LASTEXITCODE -ne 0) { $exitCode = $LASTEXITCODE }

if ($exitCode -eq 0)
{
foreach ($sub in @("textures", "progs", "mdl_skins")) {
    $src = Join-Path $srcRoot $sub
    if (Test-Path $src) {
        Copy-Item $src (Join-Path $stage $sub) -Recurse -Force
    }
}

# the material definitions name model masks as progs/... and mdl_skins/...:
# stage those two folders at the archive root as well, next to their
# textures/ copies, so every reference resolves
foreach ($legacy in @("progs", "mdl_skins")) {
    $src = Join-Path $srcRoot "textures\$legacy"
    if (Test-Path $src) {
        Copy-Item $src (Join-Path $stage $legacy) -Recurse -Force
    }
}

foreach ($f in @("BlueNoise_LDR_RGBA_128.png", "WaterNormal_n.png")) {
    $src = Join-Path $srcRoot $f
    if (Test-Path $src) {
        Copy-Item $src (Join-Path $stage $f) -Force
    }
}

$stageGfx = Join-Path $stage "gfx"
New-Item -ItemType Directory -Path $stageGfx -Force | Out-Null
Copy-Item -Path (Join-Path $PSScriptRoot "renderer\gfx\*") -Destination $stageGfx -Recurse -Force
Copy-Item (Join-Path $PSScriptRoot "third_party\imgui\fonts\Roboto-Regular.ttf") (Join-Path $stageGfx "Roboto-Regular.ttf") -Force

# The engine's own UI pack: its menu artwork reaches the game through qray.pkz.
function Expand-QuakePak([string]$PakPath, [string]$DestDir) {
    if (-not (Test-Path $PakPath)) { return }
    $stream = [System.IO.File]::OpenRead($PakPath)
    try {
        $reader = New-Object System.IO.BinaryReader($stream)
        $ident = [System.Text.Encoding]::ASCII.GetString($reader.ReadBytes(4))
        if ($ident -ne "PACK") { throw "$PakPath is not a pak file" }
        $fileLen = $stream.Length
        $dirofs = $reader.ReadInt32()
        $dirlen = $reader.ReadInt32()
        $count = [int]($dirlen / 64)
        $stream.Position = $dirofs
        $seen = @{}
        for ($i = 0; $i -lt $count; $i++) {
            $name = [System.Text.Encoding]::ASCII.GetString($reader.ReadBytes(56)).Split([char]0)[0]
            $pos = $reader.ReadInt32()
            $len = $reader.ReadInt32()
            if (-not $name -or $name.EndsWith("/")) { continue }
            if ($pos -lt 0 -or $len -lt 0 -or ($pos + $len) -gt $fileLen) {
                throw "$PakPath entry '$name' is outside the file"
            }
            if ($seen.ContainsKey($name)) {
                Write-Warning "Duplicate pak entry '$name' in $PakPath; keeping the first"
                continue
            }
            $seen[$name] = $true
            $out = Join-Path $DestDir $name
            $parent = Split-Path $out -Parent
            if (-not (Test-Path $parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
            $save = $stream.Position
            $stream.Position = $pos
            [System.IO.File]::WriteAllBytes($out, $reader.ReadBytes($len))
            $stream.Position = $save
        }
    }
    finally {
        $stream.Dispose()
    }
}

Expand-QuakePak (Join-Path $PSScriptRoot "Quake\vkquake.pak") $stage
Write-Host "Unpacked the engine UI pack into the pkz stage"

$pkzPath = Join-Path $gameDir "qray.pkz"
if (Test-Path $pkzPath) { Remove-Item $pkzPath -Force }

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$zipFs = [System.IO.Compression.ZipFile]::Open($pkzPath, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($file in (Get-ChildItem $stage -Recurse -File)) {
        $rel = $file.FullName.Substring($stage.Length + 1).Replace('\', '/')
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
            $zipFs,
            $file.FullName,
            $rel,
            [System.IO.Compression.CompressionLevel]::Optimal
        ) | Out-Null
    }
}
finally {
    $zipFs.Dispose()
}

Remove-Item $stage -Recurse -Force
Write-Host "Packed the engine assets into $pkzPath"
}

if ($nvrhiPatchedHere)
{
    if ((Invoke-GitQuietly @("-C", $nvrhiDir, "apply", "--reverse", $nvrhiPatch)) -ne 0)
    {
        throw "Could not revert $nvrhiPatch; third_party/nvrhi is left patched."
    }
    Write-Host "Reverted the NVRHI patch, third_party/nvrhi is clean again" -ForegroundColor Yellow
}

exit $exitCode
