# build-wasm.ps1
# Builds the fatp-balancer WASM demo target.
# Run from the fatp-balancer root, e.g.:
#   .\wasm\build-wasm.ps1
#
# Optional parameters:
#   -FatpInclude  Path to the FAT-P include directory (containing Expected.h)
#   -EmsdkDir     Path to your emsdk installation
#   -Serve        Start a local HTTP server in demo/ after a successful build

param(
    [string]$FatpInclude = "C:\Users\mtthw\Desktop\AI Projects\FatP\include\fat_p",
    [string]$EmsdkDir    = "C:\tools\emsdk",
    [switch]$Serve
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

function Write-Step([string]$msg) {
    Write-Host ""
    Write-Host "==> $msg" -ForegroundColor Cyan
}

function Fail([string]$msg) {
    Write-Host ""
    Write-Host "ERROR: $msg" -ForegroundColor Red
    exit 1
}

# ---------------------------------------------------------------------------
# Locate script / project root
# Works whether you run the script from the project root or from wasm/:
#   .\wasmuild-wasm.ps1   (from project root)
#   .uild-wasm.ps1        (from inside wasm/)
# ---------------------------------------------------------------------------

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

# If the script lives in wasm/, project root is one level up.
# If it was somehow invoked from the root directly, stay there.
if ((Split-Path -Leaf $ScriptDir) -eq "wasm") {
    $ProjectRoot = Split-Path -Parent $ScriptDir
} else {
    $ProjectRoot = $ScriptDir
}

Write-Host "fatp-balancer WASM build" -ForegroundColor White
Write-Host "  Project root : $ProjectRoot"
Write-Host "  FAT-P include: $FatpInclude"
Write-Host "  emsdk        : $EmsdkDir"

# ---------------------------------------------------------------------------
# Validate inputs
# ---------------------------------------------------------------------------

Write-Step "Validating paths"

if (-not (Test-Path "$FatpInclude\Expected.h")) {
    Fail "Expected.h not found at: $FatpInclude\Expected.h`nSet -FatpInclude to the directory containing Expected.h."
}

if (-not (Test-Path "$EmsdkDir\emsdk_env.bat")) {
    Fail "emsdk_env.bat not found at: $EmsdkDir\emsdk_env.bat`nSet -EmsdkDir to your emsdk installation, or run `.\emsdk install latest` and `.\emsdk activate latest` first."
}

Write-Host "  Paths OK" -ForegroundColor Green

# ---------------------------------------------------------------------------
# Activate emsdk into this process's environment
# ---------------------------------------------------------------------------

Write-Step "Activating Emscripten environment"

# emsdk_env.bat sets env vars — capture them by running it and dumping the env.
$envDump = cmd /c "`"$EmsdkDir\emsdk_env.bat`" 2>nul && set" 2>&1
foreach ($line in $envDump) {
    if ($line -match "^([^=]+)=(.*)$") {
        [System.Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], "Process")
    }
}

# Verify emcmake is now reachable.
$emcmake = Get-Command emcmake -ErrorAction SilentlyContinue
if (-not $emcmake) {
    Fail "emcmake not found on PATH after activating: $EmsdkDir\emsdk_env.bat`nCheck that emsdk is installed and activated."
}
Write-Host "  emcmake: $($emcmake.Source)" -ForegroundColor Green

# ---------------------------------------------------------------------------
# Create / clean build directory
# ---------------------------------------------------------------------------

Write-Step "Preparing build directory"

$BuildDir = Join-Path $ProjectRoot "build-wasm"

if (Test-Path $BuildDir) {
    Write-Host "  Removing existing build-wasm/" -ForegroundColor Yellow
    Remove-Item -Recurse -Force $BuildDir
}
New-Item -ItemType Directory -Path $BuildDir | Out-Null
Write-Host "  Created: $BuildDir" -ForegroundColor Green

# ---------------------------------------------------------------------------
# CMake configure
# ---------------------------------------------------------------------------

Write-Step "Configuring with emcmake cmake"

Push-Location $BuildDir
try {
    & emcmake cmake "$ScriptDir" `
        "-DFATP_INCLUDE_DIR=$FatpInclude" `
        "-DCMAKE_BUILD_TYPE=Release"

    if ($LASTEXITCODE -ne 0) { Fail "CMake configuration failed (exit $LASTEXITCODE).`nSee output above for details." }
}
finally {
    Pop-Location
}

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------

Write-Step "Building"

Push-Location $BuildDir
try {
    & cmake --build . --config Release

    if ($LASTEXITCODE -ne 0) { Fail "Build failed (exit $LASTEXITCODE).`nSee output above for details." }
}
finally {
    Pop-Location
}

# ---------------------------------------------------------------------------
# Verify outputs landed in demo/
# ---------------------------------------------------------------------------

Write-Step "Verifying outputs"

$DemoDir = Join-Path $ProjectRoot "demo"
$jsOut   = Join-Path $DemoDir "balancer.js"
$wasmOut = Join-Path $DemoDir "balancer.wasm"

if (-not (Test-Path $jsOut))   { Fail "balancer.js not found at: $jsOut`nCheck that the CMake post-build copy step ran successfully." }
if (-not (Test-Path $wasmOut)) { Fail "balancer.wasm not found at: $wasmOut`nCheck that the CMake post-build copy step ran successfully." }

Write-Host "  balancer.js   OK" -ForegroundColor Green
Write-Host "  balancer.wasm OK" -ForegroundColor Green

# ---------------------------------------------------------------------------
# Done
# ---------------------------------------------------------------------------

Write-Host ""
Write-Host "Build complete." -ForegroundColor Green
Write-Host "  demo/balancer.js   -> $('{0:N0}' -f (Get-Item $jsOut).Length) bytes"
Write-Host "  demo/balancer.wasm -> $('{0:N0}' -f (Get-Item $wasmOut).Length) bytes"

# ---------------------------------------------------------------------------
# Optional: serve
# ---------------------------------------------------------------------------

if ($Serve) {
    Write-Host ""
    Write-Host "Starting local server at http://localhost:8080 (Ctrl+C to stop)" -ForegroundColor Cyan
    Push-Location $DemoDir
    python -m http.server 8080
    Pop-Location
}
else {
    Write-Host ""
    Write-Host "To run the demo:" -ForegroundColor White
    Write-Host "  cd demo"
    Write-Host "  python -m http.server 8080"
    Write-Host "  # then open http://localhost:8080"
}
