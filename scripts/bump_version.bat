@echo off
rem Wrapper for bump_version.cmake. Usage: bump_version.bat major|minor|patch|x.y.z
if "%~1"=="" (
    echo Usage: %~nx0 major^|minor^|patch^|x.y.z 1>&2
    exit /b 1
)
cmake "-DBUMP=%~1" -P "%~dp0bump_version.cmake"
exit /b %ERRORLEVEL%
