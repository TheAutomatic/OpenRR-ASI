@echo off
setlocal
chcp 65001 >nul
cd /d "%~dp0..\.."
echo [OpenRR] Building openrr-amd.asi via WSL...
wsl bash asi/build/build_asi.sh %*
set "BUILD_RESULT=%ERRORLEVEL%"
echo.
if "%BUILD_RESULT%"=="0" (
    echo [OpenRR] Build succeeded.
    echo   ASI plugin : "%CD%\build\asi\stage\plugins\openrr-amd.asi"
    echo   Config INI : "%CD%\build\asi\stage\openrr-amd.ini"
    echo   Shaders    : "%CD%\build\asi\stage\openrr-amd\shaders"
) else (
    echo [OpenRR] Build failed with exit code %BUILD_RESULT%.
)
echo.
pause
exit /b %BUILD_RESULT%
