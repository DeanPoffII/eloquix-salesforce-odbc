# Registers the driver with the Windows ODBC driver manager.
# Run as Administrator. Register both builds: 32-bit Excel is still common.
#
#   powershell -ExecutionPolicy Bypass -File packaging\windows\register.ps1 `
#       -Dll64 build64\Release\EloquixSalesforceODBC.dll `
#       -Dll32 build32\Release\EloquixSalesforceODBC.dll
#
# Not exercised in CI on Windows yet — verify on a real machine before shipping.
param(
  [string]$Dll64 = "build64\Release\EloquixSalesforceODBC.dll",
  [string]$Dll32 = "",
  [string]$InstallDir = "$env:ProgramFiles\Eloquix\Salesforce ODBC",
  [string]$Name = "Eloquix Salesforce ODBC Driver"
)
$ErrorActionPreference = "Stop"

# 64-bit drivers are registered under SOFTWARE\ODBC; 32-bit under SOFTWARE\WOW6432Node\ODBC.
# Writing the paths explicitly avoids registry redirection surprises.
$Base64 = "HKLM:\SOFTWARE\ODBC\ODBCINST.INI"
$Base32 = "HKLM:\SOFTWARE\WOW6432Node\ODBC\ODBCINST.INI"

function Register-Driver {
  param($Dll, $Base, $Label, $Prefix)

  if (-not (Test-Path $Dll)) { Write-Warning "$Label driver not found: $Dll"; return }

  New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
  $target = Join-Path $InstallDir ($Prefix + (Split-Path $Dll -Leaf))
  Copy-Item $Dll $target -Force

  $drivers = Join-Path $Base "ODBC Drivers"
  $entry   = Join-Path $Base $Name
  New-Item -Path $drivers -Force | Out-Null
  New-ItemProperty -Path $drivers -Name $Name -Value "Installed" -PropertyType String -Force | Out-Null
  New-Item -Path $entry -Force | Out-Null
  New-ItemProperty -Path $entry -Name "Driver"        -Value $target -PropertyType String -Force | Out-Null
  New-ItemProperty -Path $entry -Name "Setup"         -Value $target -PropertyType String -Force | Out-Null
  New-ItemProperty -Path $entry -Name "Description"   -Value "Salesforce ODBC driver (Eloquix Labs)" -PropertyType String -Force | Out-Null
  New-ItemProperty -Path $entry -Name "DriverODBCVer" -Value "03.80" -PropertyType String -Force | Out-Null
  New-ItemProperty -Path $entry -Name "Threading"     -Value 1 -PropertyType DWord -Force | Out-Null
  Write-Host "Registered $Label driver: $target"
}

Register-Driver -Dll $Dll64 -Base $Base64 -Label "64-bit" -Prefix ""
if ($Dll32) { Register-Driver -Dll $Dll32 -Base $Base32 -Label "32-bit" -Prefix "x86_" }

Write-Host ""
Write-Host "Before shipping, sign the DLLs with your code-signing certificate:"
Write-Host "  signtool sign /fd sha256 /tr http://timestamp.digicert.com /td sha256 /a <dll>"
