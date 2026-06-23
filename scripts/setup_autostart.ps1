#Requires -Version 3.0
<#
.SYNOPSIS
    Install or remove the EyelashCorne-ClockSync scheduled task.

.DESCRIPTION
    Registers a Windows Task Scheduler task that runs
    "pythonw.exe keyboard_display.py --clock" at user logon,
    using the scripts directory as the working directory.

.PARAMETER Uninstall
    Remove the scheduled task instead of creating it.

.EXAMPLE
    .\setup_autostart.ps1
    .\setup_autostart.ps1 -Uninstall
#>

[CmdletBinding()]
param(
    [switch]$Uninstall
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------
$TaskName   = 'EyelashCorne-ClockSync'
$ScriptFile = 'keyboard_display.py'

# ---------------------------------------------------------------------------
# Resolve paths
# ---------------------------------------------------------------------------
$ScriptsDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$PyScript   = Join-Path $ScriptsDir $ScriptFile

# ---------------------------------------------------------------------------
# Uninstall branch
# ---------------------------------------------------------------------------
if ($Uninstall) {
    Write-Host "Removing scheduled task '$TaskName'..." -ForegroundColor Cyan

    $result = schtasks.exe /Delete /TN $TaskName /F 2>&1
    if ($LASTEXITCODE -eq 0) {
        Write-Host "Task '$TaskName' removed successfully." -ForegroundColor Green
    } else {
        if ($result -match 'cannot find') {
            Write-Warning "Task '$TaskName' was not found - nothing to remove."
        } else {
            Write-Error "Failed to remove task.`n$result"
        }
    }
    exit $LASTEXITCODE
}

# ---------------------------------------------------------------------------
# Install branch
# ---------------------------------------------------------------------------

# --- Verify keyboard_display.py exists -------------------------------------
if (-not (Test-Path $PyScript)) {
    Write-Error "Cannot find '$ScriptFile' in '$ScriptsDir'. Make sure you are running this script from the correct location."
    exit 1
}

# --- Locate pythonw.exe ----------------------------------------------------
function Find-Pythonw {
    $wherePy = where.exe python 2>$null | Select-Object -First 1
    if ($wherePy -and (Test-Path $wherePy)) {
        $candidate = Join-Path (Split-Path $wherePy) 'pythonw.exe'
        if (Test-Path $candidate) { return $candidate }
    }

    $wherePyw = where.exe pythonw 2>$null | Select-Object -First 1
    if ($wherePyw -and (Test-Path $wherePyw)) { return $wherePyw }

    $guesses = @(
        "$env:LOCALAPPDATA\Programs\Python\Python*\pythonw.exe",
        "$env:ProgramFiles\Python*\pythonw.exe",
        "C:\Python*\pythonw.exe"
    )
    foreach ($pattern in $guesses) {
        $found = Get-Item $pattern -ErrorAction SilentlyContinue |
                 Sort-Object Name -Descending |
                 Select-Object -First 1
        if ($found) { return $found.FullName }
    }

    return $null
}

$PythonW = Find-Pythonw
if (-not $PythonW) {
    Write-Error "Could not locate pythonw.exe. Install Python and make sure it is on your PATH, then re-run this script."
    exit 1
}
Write-Host "Using Python:  $PythonW" -ForegroundColor DarkGray
Write-Host "Script dir:    $ScriptsDir" -ForegroundColor DarkGray

# --- Current user ----------------------------------------------------------
$CurrentUser = "$env:USERDOMAIN\$env:USERNAME"

# --- Build the schtasks XML ------------------------------------------------
$XmlContent = @"
<?xml version="1.0" encoding="UTF-16"?>
<Task version="1.2" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task">
  <RegistrationInfo>
    <Description>Runs the EyelashCorne keyboard display clock sync at logon.</Description>
  </RegistrationInfo>
  <Triggers>
    <LogonTrigger>
      <Enabled>true</Enabled>
      <UserId>$CurrentUser</UserId>
    </LogonTrigger>
  </Triggers>
  <Principals>
    <Principal id="Author">
      <UserId>$CurrentUser</UserId>
      <LogonType>InteractiveToken</LogonType>
      <RunLevel>LeastPrivilege</RunLevel>
    </Principal>
  </Principals>
  <Settings>
    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>
    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>
    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>
    <AllowHardTerminate>true</AllowHardTerminate>
    <StartWhenAvailable>false</StartWhenAvailable>
    <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>
    <IdleSettings>
      <StopOnIdleEnd>false</StopOnIdleEnd>
      <RestartOnIdle>false</RestartOnIdle>
    </IdleSettings>
    <AllowStartOnDemand>true</AllowStartOnDemand>
    <Enabled>true</Enabled>
    <Hidden>false</Hidden>
    <RunOnlyIfIdle>false</RunOnlyIfIdle>
    <WakeToRun>false</WakeToRun>
    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>
    <Priority>7</Priority>
  </Settings>
  <Actions Context="Author">
    <Exec>
      <Command>$PythonW</Command>
      <Arguments>keyboard_display.py --clock</Arguments>
      <WorkingDirectory>$ScriptsDir</WorkingDirectory>
    </Exec>
  </Actions>
</Task>
"@

# Write XML to a temp file (schtasks /XML requires a real file path)
$rand = [System.IO.Path]::GetRandomFileName()
$TempXml = Join-Path $env:TEMP "EyelashCorne_task_$rand.xml"
try {
    $Encoding = New-Object System.Text.UnicodeEncoding $false, $true
    [System.IO.File]::WriteAllText($TempXml, $XmlContent, $Encoding)

    Write-Host "Registering scheduled task '$TaskName'..." -ForegroundColor Cyan

    $result = schtasks.exe /Create /TN $TaskName /XML $TempXml /F 2>&1
    if ($LASTEXITCODE -eq 0) {
        Write-Host ""
        Write-Host "Success! Task '$TaskName' has been registered." -ForegroundColor Green
        Write-Host ""
        Write-Host "The task will run:" -ForegroundColor White
        Write-Host "  $PythonW keyboard_display.py --clock" -ForegroundColor Yellow
        Write-Host "  (working directory: $ScriptsDir)" -ForegroundColor Yellow
        Write-Host ""
        Write-Host "To test immediately:  schtasks /Run /TN `"$TaskName`"" -ForegroundColor DarkGray
        Write-Host "To remove:            .\setup_autostart.ps1 -Uninstall" -ForegroundColor DarkGray
    } else {
        Write-Error "schtasks reported an error: $result"
        exit 1
    }
} finally {
    if (Test-Path $TempXml) {
        Remove-Item $TempXml -Force -ErrorAction SilentlyContinue
    }
}
