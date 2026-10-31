@echo off
setlocal
set "helper=%~dp0..\tools\vbcable\vb-cable-helper.ps1"
if not exist "%helper%" (
    echo This package does not contain the optional offline VB-CABLE component.
    echo See https://vb-audio.com/Cable/ or rerun Sunshine Setup with VB-CABLE selected.
    exit /b 102
)
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy RemoteSigned -File "%helper%" -AllowVendorUI %*
exit /b %ERRORLEVEL%
