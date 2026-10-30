param(
  [string]$Exe = "mcast-chat.exe"
)
# M2 smoke test: join/leave/kick/host/self-exit + M1 regression (echo & multicast text).
# Control text is read/written via WM_GETTEXT/WM_SETTEXT (GetWindowText is a stale
# cross-process cache). Requires UTF-8 BOM for Chinese literals in PS 5.1.
$ErrorActionPreference = 'Stop'
$exePath = (Resolve-Path $Exe).Path

# Start clean: kill leftover instances from aborted runs (they would join the group
# and skew member counts / host election).
Get-Process mcast-chat -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500

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
  [DllImport("user32.dll", EntryPoint="PostMessageW")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="SendMessageW")] public static extern IntPtr SendMessageText(IntPtr h, uint m, IntPtr w, string l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="SendMessageW")] public static extern IntPtr SendMessageGetText(IntPtr h, uint m, IntPtr w, StringBuilder l);
  [DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr h, int i);

  public static IntPtr MainWindowOf(uint pid) {
    IntPtr found = IntPtr.Zero;
    EnumWindows(delegate(IntPtr h, IntPtr l) {
      uint p; GetWindowThreadProcessId(h, out p);
      if (p == pid) {
        var sb = new StringBuilder(256);
        SendMessageGetText(h, 0x000D, (IntPtr)256, sb);
        if (sb.Length > 0) { found = h; return false; }
      }
      return true;
    }, IntPtr.Zero);
    return found;
  }
  public static string ReadText(IntPtr h) {
    IntPtr len = SendMessage(h, 0x000E, IntPtr.Zero, IntPtr.Zero);
    int n = (int)len + 2;
    var sb = new StringBuilder(n);
    SendMessageGetText(h, 0x000D, (IntPtr)n, sb);
    return sb.ToString();
  }
  public static void WriteText(IntPtr h, string s) { SendMessageText(h, 0x000C, IntPtr.Zero, s); }
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
  public static IntPtr FindButtonByText(IntPtr parent, string text) {
    IntPtr found = IntPtr.Zero;
    EnumChildWindows(parent, delegate(IntPtr h, IntPtr l) {
      var cn = new StringBuilder(256); GetClassName(h, cn, 256);
      if (cn.ToString() == "Button") {
        var sb = new StringBuilder(256);
        SendMessageGetText(h, 0x000D, (IntPtr)256, sb);
        if (sb.ToString().Contains(text)) { found = h; return false; }
      }
      return true;
    }, IntPtr.Zero);
    return found;
  }
}
"@

$script:fails = 0
function Check($cond, $label) {
  if ($cond) { Write-Host "[smoke] PASS: $label" }
  else { Write-Host "[smoke] FAIL: $label"; $script:fails++ }
}
function HwndOf($p) { $p.Refresh(); return [WinEnum]::MainWindowOf([uint32]$p.Id) }
function ChatOf($h) {
  $c = [WinEnum]::FindChatEdit($h)
  if ($c -eq [IntPtr]::Zero) { return "" }
  return [WinEnum]::ReadText($c)
}
function StatusOf($h) {
  $s = [WinEnum]::FindByClass($h, "msctls_statusbar32", 0)
  if ($s -eq [IntPtr]::Zero) { return "" }
  return [WinEnum]::ReadText($s)
}

$a = $null; $b = $null; $b2 = $null; $b3 = $null; $b4 = $null; $b5 = $null; $b6 = $null
try {
  # ---------- 1. Host starts alone ----------
  Write-Host "[smoke] PHASE 1: host starts alone"
  $a = Start-Process -FilePath $exePath -ArgumentList "ZhangSan","--drop","5" -PassThru
  Start-Sleep -Milliseconds 1500
  $ha = HwndOf $a
  $statusA0 = StatusOf($ha)
  Write-Host "[smoke] A status: $statusA0"
  Check ($statusA0 -match "昵称: ZhangSan") "A shows own nickname"
  Check ($statusA0 -match "房主") "A is host"
  Check ($statusA0 -match "成员: 1") "A member count = 1"

  # ---------- 2. Local echo (M1 regression) ----------
  Write-Host "[smoke] PHASE 2: local echo"
  $input = [WinEnum]::FindInputEdit($ha)
  $btn = [WinEnum]::FindByClass($ha, "Button", 0)
  [WinEnum]::WriteText($input, "ui-self-test-9999")
  [WinEnum]::SendMessage($btn, 0xF5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null  # BM_CLICK
  Start-Sleep -Milliseconds 1500
  Check ((ChatOf $ha) -match "ui-self-test-9999") "local echo in chat area"

  # ---------- 3. Second member joins and sends text ----------
  Write-Host "[smoke] PHASE 3: LiSi joins, sends marker text"
  $marker = "hello-mcast-e2e-12345"
  $b = Start-Process -FilePath $exePath -ArgumentList "LiSi","--send",$marker -PassThru
  Start-Sleep -Seconds 5
  $hb = HwndOf $b
  $chatA = ChatOf $ha
  $statusA = StatusOf($ha)
  $statusB = StatusOf($hb)
  Write-Host "[smoke] A chat: $($chatA -replace "`r`n", ' | ')"
  Write-Host "[smoke] A status: $statusA"
  Write-Host "[smoke] B status: $statusB"
  Check ($chatA -match "LiSi 加入了聊天组") "A sees join notice"
  Check ($chatA -match $marker) "multicast text delivered to A"
  Check ($statusA -match "成员: 2") "A member count = 2"
  Check (($statusB -match "成员: 2") -and -not ($statusB -match "房主")) "B sees 2 members, not host"

  # ---------- 4. Graceful leave (BYE on WM_CLOSE) ----------
  Write-Host "[smoke] PHASE 4: LiSi leaves via WM_CLOSE (BYE)"
  [WinEnum]::SendMessage($hb, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null  # WM_CLOSE
  Start-Sleep -Seconds 2
  Check ($b.HasExited) "LiSi process exited"
  Check ((ChatOf $ha) -match "LiSi 离开了聊天组") "A sees leave notice"
  Check ((StatusOf $ha) -match "成员: 1") "A member count back to 1"

  # ---------- 5. Kick by host ----------
  Write-Host "[smoke] PHASE 5: LiSi rejoins, host kicks him"
  $b2 = Start-Process -FilePath $exePath -ArgumentList "LiSi" -PassThru
  Start-Sleep -Seconds 4
  $hb2 = HwndOf $b2
  Check ((StatusOf $ha) -match "成员: 2") "A member count = 2 after rejoin"
  # 行号 1（按 joinTime 排序：ZhangSan=0, LiSi=1），WM_APP+2 与右键菜单走同一踢人路径。
  # 用 PostMessage：若踢人失败弹模态框，脚本不会被同步调用卡死。
  [WinEnum]::PostMessage($ha, 0x8002, [IntPtr]1, [IntPtr]::Zero) | Out-Null
  Start-Sleep -Seconds 2
  $chatA2 = ChatOf $ha
  $chatB2 = ChatOf $hb2
  Write-Host "[smoke] A chat after kick: $($chatA2 -replace "`r`n", ' | ')"
  Write-Host "[smoke] B chat after kick: $($chatB2 -replace "`r`n", ' | ')"
  Check ($chatA2 -match "LiSi 被移出聊天组") "A sees kick notice"
  Check ((StatusOf $ha) -match "成员: 1") "A member count = 1 after kick"
  Check ($chatB2 -match "你已被房主 ZhangSan 移出聊天组") "kicked member notified"
  $exited = $b2.WaitForExit(6000)
  Check ($exited) "kicked member auto-exits"

  # ---------- 6. Timeout removal (abrupt kill, no BYE) ----------
  Write-Host "[smoke] PHASE 6: LiSi killed abruptly, timeout removal (>20s)"
  $b3 = Start-Process -FilePath $exePath -ArgumentList "LiSi" -PassThru
  Start-Sleep -Seconds 4
  Check ((StatusOf $ha) -match "成员: 2") "A member count = 2 before kill"
  Stop-Process -Id $b3.Id -Force -ErrorAction SilentlyContinue
  Start-Sleep -Seconds 12   # >10s 无心跳 → 离线
  $chatA3 = ChatOf $ha
  Write-Host "[smoke] A chat after 12s: $($chatA3 -replace "`r`n", ' | ')"
  Check ($chatA3 -match "LiSi 离线") "A marks LiSi offline after 10s"
  Start-Sleep -Seconds 10   # >20s → 从成员表移除
  Write-Host "[smoke] A chat after 22s: $((ChatOf $ha) -replace "`r`n", ' | ')"
  Check ((ChatOf $ha) -match "LiSi 离开了聊天组") "A removes LiSi after 20s"
  Check ((StatusOf $ha) -match "成员: 1") "A member count = 1 after timeout"

  # ---------- 7. File transfer with ~20% induced loss (NACK retransmission) ----------
  # A 以 --drop 5 启动：接收时丢弃每第 5 块（≈20% 丢包），必须靠 NACK 重传收全
  Write-Host "[smoke] PHASE 7: file transfer with ~20% induced loss"
  $sampleName = "sample_" + (Get-Random) + ".bin"
  $sample = Join-Path (Get-Location).Path $sampleName
  $bytes = New-Object byte[] 2097152   # 2 MB 随机数据
  (New-Object Random 42).NextBytes($bytes)
  [IO.File]::WriteAllBytes($sample, $bytes)
  $b4 = Start-Process -FilePath $exePath -ArgumentList "WangWu","--sendfile",$sample -PassThru
  $recvDone = $false
  for ($i = 0; $i -lt 30; $i++) {
    Start-Sleep -Seconds 3
    if ((ChatOf $ha) -match ($sampleName + " 接收完成")) { $recvDone = $true; break }
  }
  Write-Host "[smoke] A chat tail: $(((ChatOf $ha) -split "`r`n" | Select-Object -Last 4) -join ' | ')"
  Check $recvDone "file received within 90s despite ~20% loss"
  Check ((ChatOf $ha) -match "请求重传") "NACK retransmission was exercised"
  $recvPath = Join-Path (Join-Path (Get-Location).Path "received") $sampleName
  Check (Test-Path $recvPath) "received file saved under received/"
  if (Test-Path $recvPath) {
    $h1 = (Get-FileHash $sample -Algorithm SHA256).Hash
    $h2 = (Get-FileHash $recvPath -Algorithm SHA256).Hash
    Check ($h1 -eq $h2) "SHA-256 of received file matches source"
  }
  Stop-Process -Id $b4.Id -Force -ErrorAction SilentlyContinue
  Remove-Item $sample -Force -ErrorAction SilentlyContinue

  # ---------- 8. Encrypted end-to-end (RSA-2048 envelope + AES-256-GCM) ----------
  Write-Host "[smoke] PHASE 8: encrypted text & file"
  # 房主 A 打开加密开关 → 生成并分发组密钥
  $encA = [WinEnum]::FindButtonByText($ha, "加密")
  Check ($encA -ne [IntPtr]::Zero) "encrypt checkbox exists"
  [WinEnum]::SendMessage($encA, 0xF5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null  # BM_CLICK
  Start-Sleep -Seconds 1
  Check ((StatusOf $ha) -match "加密:开") "A reports encryption ON"

  # B5 加入 → 房主自动换钥分发（含 B5 公钥信封）→ B5 打开开关 → 加密发送文本
  $marker2 = "enc-marker-8231"
  $b5 = Start-Process -FilePath $exePath -ArgumentList "LiSi","--send",$marker2,"--senddelay","4500" -PassThru
  Start-Sleep -Milliseconds 2500
  $hb5 = HwndOf $b5
  $encB = [WinEnum]::FindButtonByText($hb5, "加密")
  if ($encB -ne [IntPtr]::Zero) { [WinEnum]::SendMessage($encB, 0xF5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null }
  $ok8 = $false
  for ($i = 0; $i -lt 12; $i++) {
    Start-Sleep -Seconds 2
    if ((ChatOf $ha) -match $marker2) { $ok8 = $true; break }
  }
  Write-Host "[smoke] A chat tail: $(((ChatOf $ha) -split "`r`n" | Select-Object -Last 3) -join ' | ')"
  Check $ok8 "encrypted text delivered and decrypted"

  # B6 加入 → 加密发送 512KB 文件（叠加 --drop 5 的 20% 丢包）
  $smallName = "enc_" + (Get-Random) + ".bin"
  $small = Join-Path (Get-Location).Path $smallName
  $sb2 = New-Object byte[] 524288
  (New-Object Random 7).NextBytes($sb2)
  [IO.File]::WriteAllBytes($small, $sb2)
  $b6 = Start-Process -FilePath $exePath -ArgumentList "WangWu","--sendfile",$small,"--sendfiledelay","4000" -PassThru
  $ok9 = $false
  for ($i = 0; $i -lt 20; $i++) {
    Start-Sleep -Seconds 3
    if ((ChatOf $ha) -match ($smallName + " 接收完成")) { $ok9 = $true; break }
  }
  Check $ok9 "encrypted file received within 60s"
  $rp8 = Join-Path (Join-Path (Get-Location).Path "received") $smallName
  if ((Test-Path $rp8) -and $ok9) {
    Check (((Get-FileHash $small -Algorithm SHA256).Hash) -eq ((Get-FileHash $rp8 -Algorithm SHA256).Hash)) "encrypted file SHA-256 matches source"
  }
  Stop-Process -Id $b5.Id -Force -ErrorAction SilentlyContinue
  Stop-Process -Id $b6.Id -Force -ErrorAction SilentlyContinue
  Remove-Item $small -Force -ErrorAction SilentlyContinue
}
finally {
  foreach ($p in @($a, $b, $b2, $b3, $b4, $b5, $b6)) {
    if ($p -ne $null) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
  }
}

if ($script:fails -eq 0) { Write-Host "[smoke] RESULT: PASS"; exit 0 }
Write-Host "[smoke] RESULT: FAIL ($($script:fails) checks failed)"
exit 1
