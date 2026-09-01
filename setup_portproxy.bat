@echo off
setlocal EnableExtensions
title MailForge port forward setup

set "PORT=8888"
set "LOG=%USERPROFILE%\Desktop\portproxy_setup.log"

echo ============================================ >  "%LOG%"
echo MailForge setup started at %DATE% %TIME%   >> "%LOG%"
echo ============================================ >> "%LOG%"

echo [1/3] Getting WSL2 IP address...
for /f %%i in ('wsl.exe hostname -I 2^>nul') do set "WSL_IP=%%i"
if not defined WSL_IP (
    echo [ERROR] Cannot get WSL2 IP. Is WSL running?
    echo [ERROR] Cannot get WSL2 IP               >> "%LOG%"
    pause
    exit /b 1
)
echo        WSL2 IP = %WSL_IP%
echo WSL2_IP=%WSL_IP%                             >> "%LOG%"

echo [2/3] Configuring portproxy ...
netsh interface portproxy delete v4tov4 listenport=%PORT% listenaddress=0.0.0.0 >> "%LOG%" 2>&1
netsh interface portproxy add v4tov4 listenport=%PORT% listenaddress=0.0.0.0 connectport=%PORT% connectaddress=%WSL_IP% >> "%LOG%" 2>&1
if errorlevel 1 (
    echo [ERROR] portproxy add FAILED! See log: %LOG%
    echo [ERROR] portproxy add failed             >> "%LOG%"
    pause
    exit /b 1
)
echo portproxy added OK                           >> "%LOG%"

echo [3/3] Opening Windows Firewall ...
netsh advfirewall firewall delete rule name="MailForge Port %PORT%" >> "%LOG%" 2>&1
netsh advfirewall firewall add rule name="MailForge Port %PORT%" dir=in action=allow protocol=TCP localport=%PORT% >> "%LOG%" 2>&1
if errorlevel 1 (
    echo [ERROR] firewall rule FAILED! See log: %LOG%
    echo [ERROR] firewall rule add failed         >> "%LOG%"
    pause
    exit /b 1
)
echo firewall rule added OK                       >> "%LOG%"
echo Setup completed at %TIME%                    >> "%LOG%"

echo.
echo ============================================
echo  DONE! Now verifying portproxy rules:
echo ============================================
netsh interface portproxy show all
echo.
echo Log saved to: %LOG%
echo.
echo Test from another device:
echo    http://10.46.223.81:%PORT%   (ZeroTier IP)
pause
