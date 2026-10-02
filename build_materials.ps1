$s = "$PWD\renderer\Source"; $g = "$PWD\build\Debug\id1"
New-Item -ItemType Directory -Path $g -Force | Out-Null
Copy-Item "$s\materials.yaml" "$g\qray.materials.yaml" -Force
Write-Host "Deployed materials into $g" -ForegroundColor Green
