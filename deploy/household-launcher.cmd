@echo off
setlocal
"%~dp0bin\lumabri.exe" %*
set "LUMABRI_EXIT=%ERRORLEVEL%"
if not "%LUMABRI_EXIT%"=="0" if "%~1"=="" pause
exit /b %LUMABRI_EXIT%
