@echo off
REM ============================================================================
REM Generate Code Signing Certificate and Sign INF Files
REM For Linux Gadget Driver (VID: 0x0123, PID: 0x0456)
REM 
REM Prerequisites:
REM   - Windows SDK with makecert.exe, pvk2pfx.exe, signtool.exe
REM   - Or use Windows Driver Kit (WDK)
REM   - Run this script as Administrator
REM ============================================================================

setlocal

REM ============================================================================
REM Configuration
REM ============================================================================
set CERT_NAME=LinuxGadgetDriver
set CERT_PASSWORD=GadgetDriver2026
set SIGNTOOL_PATH=C:\Program Files (x86)\Windows Kits\10\bin\10.0.22621.0\x64\signtool.exe

REM Check if SDK/WDK is installed and find signtool
if not exist "%SIGNTOOL_PATH%" (
    echo [ERROR] signtool.exe not found. Please install Windows SDK or WDK.
    echo Download from: https://developer.microsoft.com/windows/downloads/windows-sdk/
    exit /b 1
)

echo ========================================
echo Step 1: Create Self-Signed Certificate
echo ========================================

REM Create certificate using makecert (deprecated but still works)
echo Creating self-signed certificate...
makecert -n "CN=%CERT_NAME%" -r -h 0 -eku "1.3.6.1.5.5.7.3.3,1.3.6.1.4.1.311.10.3.13" ^
    -e 12/31/2035 -sv "%CERT_NAME%.pvk" "%CERT_NAME%.cer"

if errorlevel 1 (
    echo [ERROR] Failed to create certificate
    exit /b 1
)

echo ========================================
echo Step 2: Convert toecho ========================================

REM PFX
 Convert to PFX format for signtool
pvk2pfx.exe -pvk "%CERT_NAME%.pvk" -pi "%CERT_PASSWORD%" ^
    -pfx "%CERT_NAME%.pfx" -f

if errorlevel 1 (
    echo [ERROR] Failed to convert to PFX
    exit /b 1
)

echo ========================================
echo Step 3: Sign ACM Driver INF
echo ========================================

REM Sign the INF file (for testing only - produces a catalog file)
signtool sign /v /f "%CERT_NAME%.pfx" /p "%CERT_PASSWORD%" /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 ^
    acm_driver.inf

if errorlevel 1 (
    echo [WARNING] ACM driver signing failed, but continuing...
) else (
    echo [OK] ACM driver signed successfully
)

echo ========================================
echo Step 4: Sign NCM Driver INF
echo ========================================

signtool sign /v /f "%CERT_NAME%.pfx" /p "%CERT_PASSWORD%" /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 ^
    ncm_driver.inf

if errorlevel 1 (
    echo [WARNING] NCM driver signing failed, but continuing...
) else (
    echo [OK] NCM driver signed successfully
)

echo ========================================
echo Step 5: Export Certificate for Import
echo ========================================

REM Export certificate to PFX that can be imported to Windows
echo Certificate files created:
echo   - %CERT_NAME%.cer (public certificate)
echo   - %CERT_NAME%.pfx (private key - KEEP SECURE!)
echo.
echo To import certificate to Windows Trusted Root:
echo   1. Double-click %CERT_NAME%.cer
echo   2. Click "Install Certificate"
echo   3. Select "Local Machine"
echo   4. Place in "Trusted Root Certification Authorities"
echo.

echo ========================================
echo DONE!
echo ========================================

endlocal
