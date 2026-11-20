<#
  Keeps the phone's date/time across power-off and restarts.

  The firmware (MunchClockDxe) adds an offset to the PMIC RTC counter to get the date. It
  publishes the counter value at boot as the UEFI variable MunchRtc; this script works out
  the offset from Windows' current (correct) time and writes it to \EFI\munch\clock.txt on
  the EFI system partition, where the firmware reads it at the next boot.

  Run as Administrator / SYSTEM, after the time is right (manual set or NTP).
  Install as a scheduled task with tools/windows/munch-clock-task.xml-style triggers:
  at startup (+3 min), hourly, and on system-time-change events.
#>
$ErrorActionPreference = 'Stop'
$log = 'C:\munch-diag\clock-sync.log'
New-Item -ItemType Directory -Force -Path (Split-Path $log) | Out-Null
function Log($m) { "$(Get-Date -Format s) $m" | Add-Content -Path $log }

# Windows cannot read firmware variables on this port (GetFirmwareEnvironmentVariable gives
# error 87), so the firmware leaves the counter it read in \EFI\munch\rtc.txt on the ESP.
Log ("whoami: " + [Security.Principal.WindowsIdentity]::GetCurrent().Name)
mountvol S: /S | Out-Null
try {
  $rtc = Get-Content -Path 'S:\EFI\munch\rtc.txt' -ErrorAction Stop | Select-Object -First 1
  $pmicBoot = [uint64]($rtc.Trim())
} catch {
  Log "rtc.txt not readable on the ESP: $_"
  mountvol S: /D | Out-Null
  exit 1
}

# A firmware without the rtc.txt writer leaves an old file behind. The counter changes on
# every boot, so the same value seen from a different Windows boot means it is stale.
$bootUtc = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime()
$state = 'C:\munch-clock\last-pmic.txt'
if (Test-Path $state) {
  $parts = (Get-Content $state -First 1).Split(' ')
  $prevBoot = [DateTime]::Parse($parts[1], $null, 'RoundtripKind')
  if (([uint64]$parts[0] -eq $pmicBoot) -and ([math]::Abs(($bootUtc - $prevBoot).TotalSeconds) -gt 120)) {
    Log "rtc.txt is stale (pmicBoot=$pmicBoot unchanged since an earlier boot), not saving"
    mountvol S: /D | Out-Null
    exit 3
  }
}
Set-Content -Path $state -Value ("$pmicBoot " + $bootUtc.ToString('o'))

# PMIC counter now = counter at firmware read + time since then. Windows started a few
# seconds after the firmware read the counter; add that fixed lead.
$lead = 4   # measured against an exact Android-side offset: 4 s, not 8
$boot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime()
$utc  = [DateTimeOffset]::UtcNow
$up   = [math]::Round(($utc.UtcDateTime - $boot).TotalSeconds)
$pmicNow = $pmicBoot + $up + $lead
$offset  = $utc.ToUnixTimeSeconds() - $pmicNow
$tz      = [int][TimeZoneInfo]::Local.GetUtcOffset($utc).TotalMinutes
if ([TimeZoneInfo]::Local.IsDaylightSavingTime($utc.LocalDateTime) -and $false) { }

# Only trust a sane date (the firmware default is its build date).
if ($utc.Year -lt 2026) { Log "system time looks wrong ($utc), not saving"; mountvol S: /D | Out-Null; exit 2 }

$line = "$offset $tz`r`n"
try {
  New-Item -ItemType Directory -Force -Path 'S:\EFI\munch' | Out-Null
  [IO.File]::WriteAllText('S:\EFI\munch\clock.txt', $line)
  Log ("saved: " + $line.Trim() + " (pmicBoot=$pmicBoot up=$up lead=$lead)")
} finally { mountvol S: /D | Out-Null }
