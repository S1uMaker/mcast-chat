param(
  [string]$Exe = "mcast-chat.exe"
)
# Smoke test (final): locate windows by PID, read/write control text via
# WM_GETTEXT / WM_SETTEXT (GetWindowText does NOT read other-process control
# text — it reads a stale system-side cache).
#   Phase 1: type into input, click Send, expect chat to contain it (local echo)
#   Phase 2: instance B --send marker, expect instance A chat to contain it
$ErrorActionPreference = 'Stop'
$exePath = (Resolve-Path $Exe).Path

Add-Type @"
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class WinEnum {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", EntryPoint="SendMessageW")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="SendMessageW")] public static extern IntPtr SendMessageText(IntPtr h, uint m, IntPtr w, string l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="SendMessageW")] public static extern IntPtr SendMessageGetText(IntPtr h, uint m, IntPtr w, StringBuilder l);
  [DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr h, int i);

  public static IntPtr MainWindowOf(uint pid) {
    IntPtr found = IntPtr.Zero;
    EnumWindows(delegate(IntPtr h, IntPtr l) {
      uint p; GetWindowThreadProcessId(h, out p);
      if (p == pid) {
        var sb = new StringBuilder(256);
        SendMessageGetText(h, 0x000D, (IntPtr)256, sb);   // WM_GETTEXT (caption)
        if (sb.Length > 0) { found = h; return false; }
      }
      return true;
    }, IntPtr.Zero);
    return found;
  }
  // control text via WM_GETTEXTLENGTH + WM_GETTEXT (real cross-process read)
  public static string ReadText(IntPtr h) {
    IntPtr len = SendMessage(h, 0x000E, IntPtr.Zero, IntPtr.Zero);  // WM_GETTEXTLENGTH
    int n = (int)len + 2;
    var sb = new StringBuilder(n);
    SendMessageGetText(h, 0x000D, (IntPtr)n, sb);                   // WM_GETTEXT
    return sb.ToString();
  }
  public static void WriteText(IntPtr h, string s) {
    SendMessageText(h, 0x000C, IntPtr.Zero, s);                     // WM_SETTEXT
  }
  // input edit = Edit child WITHOUT ES_READONLY(0x800) and WITHOUT ES_MULTILINE(0x4)
  public static IntPtr FindInputEdit(IntPtr parent) {
    IntPtr found = IntPtr.Zero;
    EnumChildWindows(parent, delegate(IntPtr h, IntPtr l) {
      var cn = new StringBuilder(256); GetClassName(h, cn, 256);
      if (cn.ToString() == "Edit") {
        int st = GetWindowLong(h, -16);
        if ((st & 0x804) == 0) { found = h; return false; }
      }
      return true;
    }, IntPtr.Zero);
    return found;
  }
  public static IntPtr FindChatEdit(IntPtr parent) {
    IntPtr found = IntPtr.Zero;
    EnumChildWindows(parent, delegate(IntPtr h, IntPtr l) {
      var cn = new StringBuilder(256); GetClassName(h, cn, 256);
      if (cn.ToString() == "Edit") {
        int st = GetWindowLong(h, -16);
        if ((st & 0x800) != 0) { found = h; return false; }
      }
      return true;
    }, IntPtr.Zero);
    return found;
  }
  public static IntPtr FindByClass(IntPtr parent, string cls, int index) {
    IntPtr found = IntPtr.Zero; int i = 0;
    EnumChildWindows(parent, delegate(IntPtr h, IntPtr l) {
      var cn = new StringBuilder(256); GetClassName(h, cn, 256);
      if (cn.ToString() == cls) {
        if (i == index) { found = h; return false; }
        i++;
      }
      return true;
    }, IntPtr.Zero);
    return found;
  }
}
"@

function HwndOf($p) {
  $p.Refresh()
  return [WinEnum]::MainWindowOf([uint32]$p.Id)
}

# ---------- Phase 1: local UI path ----------
Write-Host "[smoke] PHASE 1: local echo test"
$a = Start-Process -FilePath $exePath -ArgumentList "ZhangSan" -PassThru
Start-Sleep -Milliseconds 1500
$h = HwndOf $a
$input = [WinEnum]::FindInputEdit($h)
$btn = [WinEnum]::FindByClass($h, "Button", 0)
$chat = [WinEnum]::FindChatEdit($h)
if ($input -eq [IntPtr]::Zero -or $btn -eq [IntPtr]::Zero -or $chat -eq [IntPtr]::Zero) {
  Write-Host "[smoke] FAIL: controls not found"; Stop-Process -Id $a.Id -Force; exit 1
}
[WinEnum]::WriteText($input, "ui-self-test-9999")
$readback = [WinEnum]::ReadText($input)
Write-Host "[smoke] input readback='$readback'"
[WinEnum]::SendMessage($btn, 0xF5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null  # BM_CLICK
Start-Sleep -Seconds 2
$chatText1 = [WinEnum]::ReadText($chat)
Write-Host "[smoke] chat after local send: '$($chatText1 -replace "`r`n", ' | ')'"
$phase1 = $chatText1 -match "ui-self-test-9999" -and $readback -eq "ui-self-test-9999"
Write-Host "[smoke] PHASE 1: $(if ($phase1) { 'PASS' } else { 'FAIL' })"

# ---------- Phase 2: cross-instance multicast delivery ----------
Write-Host "[smoke] PHASE 2: cross-instance test"
$marker = "hello-mcast-e2e-12345"
$b = Start-Process -FilePath $exePath -ArgumentList "LiSi","--send",$marker -PassThru
Start-Sleep -Seconds 4
$chatText2 = [WinEnum]::ReadText($chat)
Write-Host "[smoke] ZhangSan chat final: '$($chatText2 -replace "`r`n", ' | ')'"
$sb = [WinEnum]::FindByClass($h, "msctls_statusbar32", 0)
if ($sb -ne [IntPtr]::Zero) { Write-Host "[smoke] ZhangSan statusbar: '$([WinEnum]::ReadText($sb))'" }
$chatB = [WinEnum]::FindChatEdit((HwndOf $b))
Write-Host "[smoke] LiSi chat (self message must be dropped): '$([WinEnum]::ReadText($chatB))'"
$phase2 = $chatText2 -match $marker -and -not ([WinEnum]::ReadText($chatB) -match $marker)
Write-Host "[smoke] PHASE 2: $(if ($phase2) { 'PASS' } else { 'FAIL' })"

Stop-Process -Id $a.Id -Force -ErrorAction SilentlyContinue
Stop-Process -Id $b.Id -Force -ErrorAction SilentlyContinue

if ($phase1 -and $phase2) { Write-Host "[smoke] RESULT: PASS"; exit 0 }
else { Write-Host "[smoke] RESULT: FAIL"; exit 1 }
