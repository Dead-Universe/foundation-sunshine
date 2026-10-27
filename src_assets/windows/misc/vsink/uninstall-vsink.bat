@echo off
rem VB-CABLE is a shared system driver. Sunshine must not remove other audio users.
echo Sunshine keeps the shared VB-CABLE driver installed.
echo To remove it explicitly, extract ..\tools\vbcable\VBCABLE_Driver_Pack45.zip,
echo run its original VBCABLE_Setup_x64.exe as administrator, choose Remove Driver,
echo and restart Windows when you are ready. Sunshine will not restart Windows.
echo Official instructions: https://vb-audio.com/Cable/
exit /b 0
