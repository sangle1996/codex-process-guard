$ErrorActionPreference = 'Stop'

cmake -S $PSScriptRoot -B "$PSScriptRoot/build" -G 'Visual Studio 17 2022' -A x64
if ($LASTEXITCODE) { exit $LASTEXITCODE }
cmake --build "$PSScriptRoot/build" --config Release
if ($LASTEXITCODE) { exit $LASTEXITCODE }
ctest --test-dir "$PSScriptRoot/build" -C Release --output-on-failure
if ($LASTEXITCODE) { exit $LASTEXITCODE }

New-Item -ItemType Directory -Force "$PSScriptRoot/bin" | Out-Null
Copy-Item "$PSScriptRoot/build/Release/CodexProcessGuardNative.exe" "$PSScriptRoot/bin/CodexProcessGuardNative.exe" -Force
Write-Host "Built: $PSScriptRoot/bin/CodexProcessGuardNative.exe"
