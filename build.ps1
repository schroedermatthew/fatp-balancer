# BALANCER_META:
#   meta_version: 1
#   component: fatp-balancer
#   file_role: build_script
#   path: build.ps1
#   layer: Testing
#   summary: PowerShell build script for fatp-balancer — configure, build, test.
#   api_stability: in_work

param(
    [string]$BuildType = "Debug",
    [string]$FatpDir   = "",
    [switch]$ASan,
    [switch]$TSan,
    [switch]$UBSan
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildDir  = Join-Path $ScriptDir "build"

if (-not $FatpDir) {
    $FatpDir = Join-Path $ScriptDir "..\FatP\include\fat_p"
}

if ($env:FATP_INCLUDE_DIR) {
    $FatpDir = $env:FATP_INCLUDE_DIR
}

Write-Host "=== fatp-balancer build ==="
Write-Host "  Build type : $BuildType"
Write-Host "  FAT-P dir  : $FatpDir"
Write-Host "  ASan       : $($ASan.IsPresent)"
Write-Host "  TSan       : $($TSan.IsPresent)"

$cmakeArgs = @(
    "-S", $ScriptDir,
    "-B", $BuildDir,
    "-DCMAKE_BUILD_TYPE=$BuildType",
    "-DFATP_INCLUDE_DIR=$FatpDir",
    "-DBALANCER_ENABLE_ASAN=$(if ($ASan) { 'ON' } else { 'OFF' })",
    "-DBALANCER_ENABLE_TSAN=$(if ($TSan) { 'ON' } else { 'OFF' })",
    "-DBALANCER_ENABLE_UBSAN=$(if ($UBSan) { 'ON' } else { 'OFF' })"
)

cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

cmake --build $BuildDir --config $BuildType --parallel
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host ""
Write-Host "=== Running tests ==="
Push-Location $BuildDir
ctest --output-on-failure --config $BuildType --parallel 4
$result = $LASTEXITCODE
Pop-Location

Write-Host ""
if ($result -eq 0) { Write-Host "=== All tests passed ===" }
else               { Write-Host "=== Tests FAILED ===" }
exit $result
