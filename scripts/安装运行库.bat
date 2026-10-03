@echo off
rem ============================================================
rem  fanyi runtime DLL selector
rem  Detects NVIDIA GPU and copies the matching llama.cpp DLL set
rem  (llama-cpu\ or llama-cuda\) next to fanyi-server.exe.
rem  Re-run this any time, or after moving the folder.
rem ============================================================
setlocal
cd /d "%~dp0"

if not exist "llama-cpu\llama.dll" (
  echo [ERROR] llama-cpu\llama.dll not found.
  pause
  exit /b 1
)

set GPU=0
where nvidia-smi >nul 2>nul && set GPU=1
if %GPU%==0 (
  powershell -NoProfile -Command "if (Get-CimInstance Win32_VideoController | Where-Object { $_.Name -match 'NVIDIA' }) { exit 0 } else { exit 1 }" >nul 2>nul && set GPU=1
)

if %GPU%==1 (
  if not exist "llama-cuda\llama.dll" (
    echo [WARN] NVIDIA GPU detected, but llama-cuda\ is missing. Falling back to CPU.
    xcopy /y /q "llama-cpu\*.dll" . >nul
  ) else (
    echo NVIDIA GPU detected - installing CUDA runtime DLLs...
    xcopy /y /q "llama-cuda\*.dll" . >nul
    echo NOTE: first run may take a while while CUDA kernels initialize.
  )
) else (
  echo No NVIDIA GPU - installing CPU runtime DLLs...
  xcopy /y /q "llama-cpu\*.dll" . >nul
)

echo.
echo Done. DLLs are next to fanyi-server.exe now.
echo Start fanyi-server.exe and open http://127.0.0.1:8765/settings to verify.
pause
