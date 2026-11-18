@echo off
rem One-click update: runs the console build so progress and the summary are visible.
"%~dp0katoshe-cli.exe" update %*
if errorlevel 1 pause
