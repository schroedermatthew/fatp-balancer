@echo off
:: build-wasm.bat
:: Wrapper so build-wasm.ps1 can be invoked from cmd, VS Developer Command
:: Prompt, or by double-clicking — without needing to open PowerShell first.
::
:: Usage (same arguments as the .ps1):
::   build-wasm.bat
::   build-wasm.bat -FatpInclude "C:\path\to\fat_p"
::   build-wasm.bat -EmsdkDir "C:\tools\emsdk"
::   build-wasm.bat -Serve

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build-wasm.ps1" %*
