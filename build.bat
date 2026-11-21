@echo off
setlocal
chcp 65001 >nul

where gcc >nul 2>nul
if errorlevel 1 (
    echo [ERROR] gcc not found in PATH.
    echo Install MinGW-w64 and make sure gcc/windres are available.
    pause
    exit /b 1
)
where windres >nul 2>nul
if errorlevel 1 (
    echo [ERROR] windres not found in PATH.
    pause
    exit /b 1
)

echo [1/2] Compiling resources...
windres app.rc -O coff -o app.res
if errorlevel 1 goto fail

echo [2/2] Compiling shot.c...
gcc -O2 -s -Wall -mwindows shot.c app.res -o snapshot.exe ^
    -lgdiplus -lgdi32 -luser32 -lshell32 -lole32 -lcomdlg32 -lm
if errorlevel 1 goto fail

echo.
echo [OK] snapshot.exe built.
pause
exit /b 0

:fail
echo.
echo [FAILED] Build error, see messages above.
pause
exit /b 1
