[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
$buildDirectory = Join-Path $projectRoot "build"
$distributionDirectory = Join-Path $projectRoot "dist"

$cmake = Get-Command cmake.exe -ErrorAction Stop

& $cmake.Source -S $projectRoot -B $buildDirectory -G "Visual Studio 17 2022" -A x64 -DLIGHTLAUNCH_BUILD_TESTS=ON
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }

& $cmake.Source --build $buildDirectory --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { throw "Build failed." }

$ctest = Join-Path (Split-Path -Parent $cmake.Source) "ctest.exe"
& $ctest --test-dir $buildDirectory -C $Configuration --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "Tests failed." }

& $cmake.Source --install $buildDirectory --config $Configuration --prefix $distributionDirectory
if ($LASTEXITCODE -ne 0) { throw "Install failed." }

$executable = Get-Item -LiteralPath (Join-Path $distributionDirectory "LightLaunch.exe")
Write-Host "Built: $($executable.FullName)"
Write-Host "Size : $([math]::Round($executable.Length / 1KB, 1)) KiB"
