param(
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
$source = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $source "build"
$plugins = [System.IO.Path]::GetFullPath((Join-Path $source "..\..\..\.."))
$destination = Join-Path $plugins "DLSS5\DLSSNRRuntime\Win64"

cmake -S $source -B $build -A x64
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }
cmake --build $build --config $Configuration
if ($LASTEXITCODE -ne 0) { throw "Caller adapter build failed." }

New-Item -ItemType Directory -Path $destination -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $build "$Configuration\nvngx.dll_research_adapter.dll") -Destination $destination -Force
Write-Output "Built unofficial caller adapter to $destination"
