# Generate Code Signing Certificate and Sign INF Files
# For Linux Gadget Driver (VID: 0x0123, PID: 0x0456)
#
# Prerequisites:
#   - Windows 10/11 with PowerShell 5.1+
#   - SignTool.exe from Windows SDK or WDK
#   - Run this script as Administrator

param(
    [string]$CertName = "LinuxGadgetDriver",
    [string]$CertPassword = "GadgetDriver2026",
    [string]$SignToolPath = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.22621.0\x64\signtool.exe"
)

# Find signtool
$possiblePaths = @(
    "C:\Program Files (x86)\Windows Kits\10\bin\10.0.22621.0\x64\signtool.exe",
    "C:\Program Files (x86)\Windows Kits\10\0.19041.0\x64\signtool.exe",
    "C:\Program Files (x86)\Windows Kits\8.1\bin\x64\signtool.exe"
)

$foundTool = $false
foreach ($path in $possiblePaths) {
    if (Test-Path $path) {
        $SignToolPath = $path
        $foundTool = $true
        break
    }
}

if (-not $foundTool) {
    Write-Host "[ERROR] signtool.exe not found. Please install Windows SDK or WDK." -ForegroundColor Red
    Write-Host "Download from: https://developer.microsoft.com/windows/downloads/windows-sdk/" -ForegroundColor Yellow
    exit 1
}

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "Step 1: Creating Self-Signed Certificate" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan

# Create self-signed certificate using New-SelfSignedCertificate
$cert = New-SelfSignedCertificate `
    -Type CodeSigningCert `
    -Subject "CN=$CertName" `
    -KeyUsage DigitalSignature `
    -CertStoreLocation "Cert:\CurrentUser\My" `
    -NotAfter (Get-Date).AddYears(10)

Write-Host "[OK] Certificate created: $($cert.Thumbprint)" -ForegroundColor Green

# Export to PFX
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "Step 2: Exporting Certificate to PFX" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan

$pfxPath = "$CertName.pfx"
$securePassword = ConvertTo-SecureString -String $CertPassword -Force -AsPlainText
Export-PfxCertificate -Cert $cert -FilePath $pfxPath -Password $securePassword

Write-Host "[OK] Certificate exported to: $pfxPath" -ForegroundColor Green

# Export to CER (public key)
$cerPath = "$CertName.cer"
Export-Certificate -Cert $cert -FilePath $cerPath | Out-Null

Write-Host "[OK] Public certificate exported to: $cerPath" -ForegroundColor Green

# Sign ACM Driver
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "Step 3: Signing ACM Driver INF" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan

& $SignToolPath sign /v /f $pfxPath /p $CertPassword /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 acm_driver.inf

if ($LASTEXITCODE -eq 0) {
    Write-Host "[OK] ACM driver signed successfully" -ForegroundColor Green
} else {
    Write-Host "[WARNING] ACM driver signing returned: $LASTEXITCODE" -ForegroundColor Yellow
}

# Sign NCM Driver
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "Step 4: Signing NCM Driver INF" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan

& $SignToolPath sign /v /f $pfxPath /p $CertPassword /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 ncm_driver.inf

if ($LASTEXITCODE -eq 0) {
    Write-Host "[OK] NCM driver signed successfully" -ForegroundColor Green
} else {
    Write-Host "[WARNING] NCM driver signing returned: $LASTEXITCODE" -ForegroundColor Yellow
}

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "DONE!" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "Certificate files created:" -ForegroundColor White
Write-Host "  - $cerPath (public certificate)" -ForegroundColor Gray
Write-Host "  - $pfxPath (private key - KEEP SECURE!)" -ForegroundColor Gray
Write-Host ""
Write-Host "To import certificate to Windows Trusted Root:" -ForegroundColor White
Write-Host "  1. Double-click $cerPath" -ForegroundColor Gray
Write-Host "  2. Click 'Install Certificate'" -ForegroundColor Gray
Write-Host "  3. Select 'Local Machine'" -ForegroundColor Gray
Write-Host "  4. Place in 'Trusted Root Certification Authorities'" -ForegroundColor Gray
