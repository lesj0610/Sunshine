@echo off
rem Installs the bundled SudoVDA virtual display driver. Run it elevated.
rem It asks before trusting the driver's certificate; -Silent skips the question.
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install-sudovda.ps1" %*
exit /b %errorlevel%
