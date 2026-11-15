@echo off
rem Double-click start for Windows.
rem The native Windows build has no web server, so there is no browser chat
rem page on this computer and this file does not open a browser. It opens the
rem native Lumabri workspace window, exactly as Lumabri.cmd does. Create or
rem join a household there. A browser chat page is served by a Linux or macOS
rem computer of the household that runs its own start file.
setlocal
call "%~dp0Lumabri.cmd" %*
exit /b %ERRORLEVEL%
