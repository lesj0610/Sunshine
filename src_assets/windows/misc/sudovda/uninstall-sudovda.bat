@echo off
rem Removes the bundled SudoVDA virtual display driver. Run it elevated.
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0uninstall-sudovda.ps1" %*
exit /b %errorlevel%
