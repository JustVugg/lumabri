@echo off
rem Double-click start for Windows. It creates this computer's household if
rem there is none, starts sharing and the local web page, and opens the chat
rem page in the default browser. The page is served on 127.0.0.1 only.
rem   --tui         open the console workspace instead of the browser
rem   --no-browser  print the private sign-in link instead of opening it
rem Everything keeps running after this window closes. To stop it, run
rem Lumabri.cmd in this folder with: service stop
setlocal
"%~dp0bin\lumabri.exe" start %*
set "LUMABRI_EXIT=%ERRORLEVEL%"
rem A window opened by a double click closes with this file. Keep a failure
rem on screen until it has been read.
if not "%LUMABRI_EXIT%"=="0" if "%~1"=="" pause
exit /b %LUMABRI_EXIT%
