@echo off
rem Installs the bundled SudoVDA virtual display driver.
rem Invoked by sunshine-setup.ps1, which already runs elevated.
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install-sudovda.ps1" %*
exit /b %errorlevel%
