
param(
    [switch]$Rebuild,
    [switch]$GenCommon,
    [string]$DestDir = "",
    [string]$Pkz = ""
)

$ErrorActionPreference = "Stop"

$shaderSrc = Join-Path $PSScriptRoot "renderer\Source\Shaders"
$shaderOut = Join-Path $PSScriptRoot "renderer\Build"
$pkzPath   = if ($Pkz) { $Pkz } else { Join-Path $PSScriptRoot "build\Debug\id1\qray.pkz" }

$built = @(Get-ChildItem -Path (Join-Path $shaderOut "*.spv") -ErrorAction SilentlyContinue)
if ($Rebuild -and $built.Count -gt 0)
{
    Remove-Item (Join-Path $shaderOut "*.spv") -Force
}

if ($env:VULKAN_SDK) {
    $sdkBin = Join-Path $env:VULKAN_SDK "Bin"
    if (Test-Path (Join-Path $sdkBin "dxc.exe")) {
        $env:PATH = "$sdkBin;$env:PATH"
    }
}
if (-not (Get-Command dxc -ErrorAction SilentlyContinue)) {
    throw "dxc not found. Install the Vulkan SDK or set VULKAN_SDK."
}

if ($GenCommon) {
    Push-Location (Join-Path $PSScriptRoot "renderer\Source\Generated")
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

$srcFiles = @(Get-ChildItem -Path (Join-Path $shaderOut "*.spv") -ErrorAction SilentlyContinue)
if ($srcFiles.Count -eq 0) {
    throw "No SPIR-V was produced in $shaderOut; nothing was deployed."
}

$srcNames = @($srcFiles | Select-Object -ExpandProperty Name)

if ($DestDir) {
    if (-not (Test-Path $DestDir)) {
        New-Item -ItemType Directory -Path $DestDir -Force | Out-Null
    }

    $stale = Get-ChildItem -Path (Join-Path $DestDir "*.spv") | Where-Object { $srcNames -notcontains $_.Name }
    foreach ($f in $stale) {
        Write-Host "Removing stale shader: $($f.Name)" -ForegroundColor Yellow
        Remove-Item $f.FullName -Force
    }

    $copied = Copy-Item -Path (Join-Path $shaderOut "*.spv") -Destination $DestDir -Force -PassThru
    Write-Host "Deployed $($copied.Count) shader(s) to $DestDir" -ForegroundColor Green
}
else {
    if (-not (Test-Path $pkzPath)) {
        throw "$pkzPath does not exist; run .\build_win.ps1 once to create the engine pack, or pass -DestDir."
    }

    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem

    $zipFs = [System.IO.Compression.ZipFile]::Open($pkzPath, [System.IO.Compression.ZipArchiveMode]::Update)
    try {
        foreach ($f in $srcFiles) {
            $entryName = "shaders/$($f.Name)"
            $entry = $zipFs.GetEntry($entryName)
            if ($entry) { $entry.Delete() }
            [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                $zipFs,
                $f.FullName,
                $entryName,
                [System.IO.Compression.CompressionLevel]::Optimal
            ) | Out-Null
        }

        $stale = @($zipFs.Entries | Where-Object { $_.FullName -like 'shaders/*.spv' -and $srcNames -notcontains $_.Name })
        foreach ($e in $stale) {
            Write-Host "Removing stale pack entry: $($e.FullName)" -ForegroundColor Yellow
            $e.Delete()
        }

        Write-Host "Updated $($srcFiles.Count) shader(s) in $pkzPath" -ForegroundColor Green
        Write-Host "Restart the game to pick them up (the engine pack is read at startup)."
    }
    finally {
        $zipFs.Dispose()
    }
}
