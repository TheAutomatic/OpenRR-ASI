@echo off
setlocal
cd /d "%~dp0..\.."
echo [OpenRR] Packaging release archive from staged build...
python asi\build\package_release.py %*
set "PKG_RESULT=%ERRORLEVEL%"
echo.
if "%PKG_RESULT%"=="0" (
    echo [OpenRR] Release archive written under "%CD%\dist\"
) else (
    echo [OpenRR] Packaging failed with exit code %PKG_RESULT%.
)
exit /b %PKG_RESULT%
