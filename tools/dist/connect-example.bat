@echo off
rem Connects to a server. Edit the address, the name and the secret.
rem The first login with a name creates the account and sets its secret.
rem If the game cannot get in it says why in its own window; should it end in an error
rem anyway, this console stays open on the reason instead of vanishing.
cd /d "%~dp0"
econspace.exe connect 127.0.0.1 50800 pilot change-this-secret
if errorlevel 1 (
    echo.
    echo The game ended with an error ^(exit code %errorlevel%^). The reason is above.
    pause
)
