# Windows MSI real installed-version upgrade smoke.
# This validates the *installer* lifecycle, not the in-app MsiHandoff UI or
# Ed25519 feed authorization; both require a separate signed-feed E2E test.
# Uses immutable Fulcrum v0.13.1 -> v0.13.2 public release assets.
# Also verifies actual installed WinUI/backend cold launch after each MSI step.
[CmdletBinding()]
param(
  [string]$OldVersion = "0.13.1",
  [string]$NewVersion = "0.13.2"
)
$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

# Unlike the staging acceptance, this probe opens the EXE from Program Files
# after a real MSI transaction. An MSI registration alone does not prove the
# installed WinUI window can initialize its embedded Racket backend.
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

if ($OldVersion -eq $NewVersion) {
  throw "Upgrade test requires two different versions"
}

$root = Join-Path $env:RUNNER_TEMP ("fulcrum-msi-upgrade-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $root -Force | Out-Null
$uninstallPaths = @(
  "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*",
  "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*"
)
$msiexec = Join-Path $env:SystemRoot "System32\msiexec.exe"

function Get-FulcrumInstallation {
  @(
    foreach ($registryPath in $uninstallPaths) {
      Get-ItemProperty -Path $registryPath -ErrorAction SilentlyContinue |
        Where-Object { $_.PSObject.Properties["DisplayName"] -and
                       $_.DisplayName -eq "Fulcrum" }
    }
  )
}

function Require-InstalledVersion([string]$version) {
  $entries = @(Get-FulcrumInstallation)
  if ($entries.Count -ne 1) {
    throw "Expected one registered Fulcrum MSI, found $($entries.Count)"
  }
  if ([string]$entries[0].DisplayVersion -ne $version) {
    throw "Expected installed version $version; got $($entries[0].DisplayVersion)"
  }
  $exe = Join-Path $env:ProgramFiles "Fulcrum\RivetHost.exe"
  if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw "Expected installed executable at $exe"
  }
  Write-Host "PASS: installed Fulcrum $version with one MSI registration and executable"
}

function Assert-InstalledColdLaunch([string]$label) {
  $exe = Join-Path $env:ProgramFiles "Fulcrum\\RivetHost.exe"
  $dataDir = Join-Path $root ("user-data-" + $label)
  New-Item -ItemType Directory -Path $dataDir -Force | Out-Null
  $savedData = $env:FULCRUM_DATA_DIR
  $savedInstance = $env:FULCRUM_INSTANCE_ID
  $process = $null
  try {
    # Keep test user data and instance ownership isolated from any installed
    # Fulcrum for the runner account. The tested executable is still the
    # actual copy the MSI placed in Program Files.
    $env:FULCRUM_DATA_DIR = $dataDir
    $env:FULCRUM_INSTANCE_ID = "site.jrtx.fulcrum.installed-smoke.$PID.$label"
    $process = Start-Process -FilePath $exe -PassThru
    $deadline = [DateTime]::UtcNow.AddSeconds(45)
    $ready = $false
    $desktop = [System.Windows.Automation.AutomationElement]::RootElement
    $processCondition = [System.Windows.Automation.PropertyCondition]::new(
      [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $process.Id)
    $queryCondition = [System.Windows.Automation.PropertyCondition]::new(
      [System.Windows.Automation.AutomationElement]::AutomationIdProperty, "QueryBox")
    $statusCondition = [System.Windows.Automation.PropertyCondition]::new(
      [System.Windows.Automation.AutomationElement]::AutomationIdProperty, "StatusText")
    while ([DateTime]::UtcNow -lt $deadline) {
      $process.Refresh()
      if ($process.HasExited) {
        throw "Installed Fulcrum ($label) exited before presenting its launcher (exit $($process.ExitCode))"
      }
      try {
        $window = $desktop.FindFirst(
          [System.Windows.Automation.TreeScope]::Children, $processCondition)
        if ($null -ne $window) {
          $query = $window.FindFirst(
            [System.Windows.Automation.TreeScope]::Descendants, $queryCondition)
          $status = $window.FindFirst(
            [System.Windows.Automation.TreeScope]::Descendants, $statusCondition)
          if ($null -ne $query -and $null -ne $status -and
              $status.Current.Name -notmatch 'Starting embedded|启动中') {
            $ready = $true
            break
          }
        }
      } catch [System.Windows.Automation.ElementNotAvailableException] {
        # The window may be re-parented during first native activation.
      }
      Start-Sleep -Milliseconds 250
    }
    if (-not $ready) {
      throw "Installed Fulcrum ($label) did not expose a ready WinUI QueryBox and StatusText in 45s"
    }
    Write-Host "PASS: $label installed EXE launched real WinUI and initialized backend"
  } finally {
    $env:FULCRUM_DATA_DIR = $savedData
    $env:FULCRUM_INSTANCE_ID = $savedInstance
    if ($null -ne $process) {
      $process.Refresh()
      if (-not $process.HasExited) {
        # Teardown only. This test does not claim a graceful tray Quit; it
        # verifies cold launch separately from installer process handoff.
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        Wait-Process -Id $process.Id -Timeout 10 -ErrorAction SilentlyContinue
      }
      $process.Dispose()
    }
  }
}

function Get-OfficialMsi([string]$version) {
  $base = "https://github.com/turinglambdaai/fulcrum/releases/download/v$version"
  $filename = "fulcrum-$version-windows-x64.msi"
  $destination = Join-Path $root $filename
  $shaFile = "$destination.sha256"
  Invoke-WebRequest -Uri "$base/$filename" -OutFile $destination -MaximumRedirection 5
  Invoke-WebRequest -Uri "$base/$filename.sha256" -OutFile $shaFile -MaximumRedirection 5
  $checksumText = (Get-Content -LiteralPath $shaFile -Raw).Trim()
  if ($checksumText -notmatch "(?i)^([0-9a-f]{64})\s+\*?\S+") {
    throw "Official checksum sidecar is missing SHA-256: $shaFile"
  }
  $expected = $Matches[1].ToLowerInvariant()
  $actual = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
  if ($actual -ne $expected) {
    throw "Published MSI hash mismatch for $version"
  }
  Write-Host "PASS: official release $version MSI SHA-256 is correct"
  return $destination
}

function Invoke-Msi([string]$verb, [string]$msiOrGuid, [string]$label) {
  $log = Join-Path $root "$label.log"
  $argLine = "/$verb `"$msiOrGuid`" /qn /norestart /L*v `"$log`""
  Write-Host "Running Windows Installer: $label"
  $process = Start-Process -FilePath $msiexec -ArgumentList $argLine -Wait -PassThru
  if ($process.ExitCode -notin @(0, 3010)) {
    if (Test-Path $log) {
      Get-Content $log -Tail 45 | ForEach-Object { Write-Host $_ }
    }
    throw "msiexec $label failed with exit code $($process.ExitCode)"
  }
  if ($process.ExitCode -eq 3010) {
    throw "msiexec $label requested a reboot; cannot assert a complete in-session upgrade"
  }
}

function Invoke-ExpectedMsiFailure([string]$msi, [string]$label) {
  $log = Join-Path $root "$label.log"
  $argLine = "/i `"$msi`" /qn /norestart /L*v `"$log`""
  Write-Host "Running deliberately invalid MSI: $label"
  $process = Start-Process -FilePath $msiexec -ArgumentList $argLine -Wait -PassThru
  if ($process.ExitCode -notin @(1619, 1620)) {
    if (Test-Path $log) {
      Get-Content $log -Tail 45 | ForEach-Object { Write-Host $_ }
    }
    throw "Invalid MSI failed for an unexpected reason (exit $($process.ExitCode)); expected 1619/1620"
  }
  Write-Host "PASS: invalid MSI package rejected with exit code $($process.ExitCode)"
}

$installed = $false
try {
  if (@(Get-FulcrumInstallation).Count -gt 0) {
    throw "Runner is not clean: a Fulcrum MSI is already installed"
  }

  $old = Get-OfficialMsi $OldVersion
  $new = Get-OfficialMsi $NewVersion

  Invoke-Msi "i" $old "install-old"
  $installed = $true
  Require-InstalledVersion $OldVersion
  Assert-InstalledColdLaunch "old-install"

  # Corrupt the MSI database header but keep the previously installed product
  # untouched. A subsequent valid retry must still upgrade successfully.
  $broken = Join-Path $root "deliberately-invalid.msi"
  Copy-Item -LiteralPath $new -Destination $broken
  $bytes = [System.IO.File]::ReadAllBytes($broken)
  $bytes[0] = 0
  [System.IO.File]::WriteAllBytes($broken, $bytes)
  Invoke-ExpectedMsiFailure $broken "invalid-upgrade"
  Require-InstalledVersion $OldVersion
  Write-Host "PASS: failed upgrade preserved the old installation; retrying the valid MSI"

  Invoke-Msi "i" $new "upgrade-new"
  Require-InstalledVersion $NewVersion
  Assert-InstalledColdLaunch "upgraded-install"
  Write-Host "PASS: major upgrade $OldVersion -> $NewVersion removed the previous registration"

  # The old MSI must refuse a downgrade, preserving the working new app.
  # This is an MSI downgrade-policy regression, not a crash recovery test.
  $downgradeLog = Join-Path $root "blocked-downgrade.log"
  $args = "/i `"$old`" /qn /norestart /L*v `"$downgradeLog`""
  $downgrade = Start-Process -FilePath $msiexec -ArgumentList $args -Wait -PassThru
  if ($downgrade.ExitCode -in @(0, 3010, 1641)) {
    throw "Older MSI unexpectedly accepted a downgrade (exit $($downgrade.ExitCode))"
  }
  Require-InstalledVersion $NewVersion
  Write-Host "PASS: older MSI downgrade rejected without damaging the newer installation"
} finally {
  if ($installed) {
    $entries = @(Get-FulcrumInstallation)
    foreach ($entry in $entries) {
      if ($entry.PSChildName -notmatch "^\{[0-9A-Fa-f-]{36}\}$") {
        Write-Warning "Cannot safely extract MSI ProductCode from $($entry.PSChildName)"
        continue
      }
      try {
        Invoke-Msi "x" $entry.PSChildName "uninstall"
        Write-Host "Cleanup: removed test Fulcrum installation"
      } catch {
        Write-Warning "Cleanup failed: $_"
      }
    }
  }
  Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
