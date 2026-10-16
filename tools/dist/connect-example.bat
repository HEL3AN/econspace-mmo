@echo off
rem Connects to a server. Edit the address, the name and the secret.
rem The first login with a name creates the account and sets its secret.
cd /d "%~dp0"
start "" econspace.exe connect 127.0.0.1 50800 pilot change-this-secret
