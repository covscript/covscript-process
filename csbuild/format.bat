@echo off
REM Format all C++ source files in the covscript-process project.
REM Requires astyle:  https://astyle.sourceforge.net/
REM Options:  -A4  attach braces to the end of lines (Linux/Java style)
REM            -N   do not indent namespaces
REM            -t   use tabs for indentation (8 spaces = 1 tab)
REM            -n   do not create .orig backup files

setlocal enabledelayedexpansion

set "PROJECT_ROOT=%~dp0.."

where astyle >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo error: astyle not found. Install it from https://astyle.sourceforge.net/
    exit /b 1
)

echo Formatting C++ sources...

for /r "%PROJECT_ROOT%" %%f in (*.cpp *.hpp) do (
    set "filepath=%%f"
    if "!filepath:third_party=!"=="!filepath!" (
        astyle -A4 -N -t -n "%%f"
    )
)

echo Done.
endlocal
