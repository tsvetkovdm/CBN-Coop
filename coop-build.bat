@echo off
rem Local Windows build of CBN-Coop. See COOP_BUILD_WINDOWS.md.
rem Arguments are passed to coop-build.ps1, e.g.: coop-build.bat -Reconfigure
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0coop-build.ps1" %*
pause
