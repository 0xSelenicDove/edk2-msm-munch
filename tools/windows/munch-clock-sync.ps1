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

Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public class Fw {
  [DllImport("kernel32.dll", SetLastError=true)] public static extern uint GetFirmwareEnvironmentVariableW(string n, string g, byte[] b, uint size);
  [DllImport("advapi32.dll", SetLastError=true)] public static extern bool OpenProcessToken(IntPtr h, uint a, out IntPtr t);
  [DllImport("advapi32.dll", SetLastError=true)] public static extern bool LookupPrivilegeValue(string s, string n, out long luid);
  [DllImport("advapi32.dll", SetLastError=true)] public static extern bool AdjustTokenPrivileges(IntPtr t, bool d, ref TP n, int l, IntPtr p, IntPtr r);
  [DllImport("kernel32.dll")] public static extern IntPtr GetCurrentProcess();
  [StructLayout(LayoutKind.Sequential, Pack=1)] public struct TP { public int Count; public long Luid; public int Attr; }
  public static void Enable() { IntPtr t; OpenProcessToken(GetCurrentProcess(), 0x28, out t); long l; LookupPrivilegeValue(null, "SeSystemEnvironmentPrivilege", out l);
    TP tp = new TP(); tp.Count = 1; tp.Luid = l; tp.Attr = 2; AdjustTokenPrivileges(t, false, ref tp, 0, IntPtr.Zero, IntPtr.Zero); }
}
'@
[Fw]::Enable()

$buf = New-Object byte[] 24
$n = [Fw]::GetFirmwareEnvironmentVariableW('MunchRtc', '{4a1d8b3c-6e52-47f0-9c3a-71e05b28d416}', $buf, 24)
if ($n -ne 24) { Log "MunchRtc variable not readable (n=$n)"; exit 1 }
$pmicBoot = [BitConverter]::ToUInt64($buf, 0)

# PMIC counter now = counter at firmware read + time since then. Windows started a few
# seconds after the firmware read the counter; add that fixed lead.
$lead = 8
$boot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime()
$utc  = [DateTimeOffset]::UtcNow
$up   = [math]::Round(($utc.UtcDateTime - $boot).TotalSeconds)
$pmicNow = $pmicBoot + $up + $lead
$offset  = $utc.ToUnixTimeSeconds() - $pmicNow
$tz      = [int][TimeZoneInfo]::Local.GetUtcOffset($utc).TotalMinutes
if ([TimeZoneInfo]::Local.IsDaylightSavingTime($utc.LocalDateTime) -and $false) { }

# Only trust a sane date (the firmware default is its build date).
if ($utc.Year -lt 2026) { Log "system time looks wrong ($utc), not saving"; exit 2 }

$line = "$offset $tz`r`n"
mountvol S: /S | Out-Null
try {
  New-Item -ItemType Directory -Force -Path 'S:\EFI\munch' | Out-Null
  [IO.File]::WriteAllText('S:\EFI\munch\clock.txt', $line)
  Log "saved: $line".Trim()
} finally { mountvol S: /D | Out-Null }
