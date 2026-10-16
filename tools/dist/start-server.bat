@echo off
rem Hosts an EconSpace server on port 50800, reachable from other machines.
rem The world and the accounts are saved in this folder.
cd /d "%~dp0"
econserver.exe host 50800 --public
pause
