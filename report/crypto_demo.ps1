# 加密验证抓包流程：
#   1) 启动 A(房主) 与 B；B 先发一条明文文本（加密关闭）
#   2) A、B 依次勾选"加密发送"（房主分发组密钥）
#   3) B 再发一条文本（此时为 AES-256-GCM 密文）
#   全程由 tshark 在多播出接口抓包，供报告对比明文/密文载荷
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "mcast-chat\mcast-chat.exe"
$cap = Join-Path $PSScriptRoot "_crypto.pcapng"
$tshark = "C:\Program Files\Wireshark\tshark.exe"

Get-Process mcast-chat -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500

# 找多播出接口：224/4 路由所在网卡 → 对应 tshark 设备
$ifidx = (Get-NetRoute -DestinationPrefix "224.0.0.0/4" |
          Sort-Object RouteMetric | Select-Object -First 1).InterfaceIndex
$alias = (Get-NetAdapter -InterfaceIndex $ifidx).InterfaceAlias
Write-Host "[demo] multicast iface: $alias (idx=$ifidx)"
$dev = ($(& $tshark -D) | Where-Object { $_ -match [regex]::Escape($alias) } |
        Select-Object -First 1) -replace '^(\d+)\..*$','$1'
if (-not $dev) { Write-Host "[demo] ERR: capture device not found"; exit 1 }
Write-Host "[demo] capture device #$dev"

# UI 自动化辅助
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class WE {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", EntryPoint="SendMessageW")] public static extern IntPtr SM(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll", EntryPoint="SendMessageW", CharSet=CharSet.Unicode)] public static extern IntPtr SMT(IntPtr h, uint m, IntPtr w, string l);
  [DllImport("user32.dll", EntryPoint="SendMessageW", CharSet=CharSet.Unicode)] public static extern IntPtr SMG(IntPtr h, uint m, IntPtr w, StringBuilder l);
  [DllImport("user32.dll", EntryPoint="PostMessageW")] public static extern bool PM(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  public static IntPtr MainOf(uint pid) {
    IntPtr f = IntPtr.Zero;
    EnumWindows2(delegate(IntPtr h, IntPtr l) {
      uint p; GetWindowThreadProcessId(h, out p);
      if (p == pid) { var sb = new StringBuilder(256); SMG(h, 0x000D, (IntPtr)256, sb);
        if (sb.Length > 0) { f = h; return false; } }
      return true;
    }, IntPtr.Zero);
    return f;
  }
  public delegate bool EnumWindowsProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr l);
  static void EnumWindows2(EnumWindowsProc cb, IntPtr l) { EnumWindows(cb, l); }
  public static IntPtr FindByClass(IntPtr parent, string cls, int index) {
    IntPtr f = IntPtr.Zero; int i = 0;
    EnumChildWindows(parent, delegate(IntPtr h, IntPtr l) {
      var cn = new StringBuilder(256); GetClassName(h, cn, 256);
      if (cn.ToString() == cls) { if (i == index) { f = h; return false; } i++; }
      return true;
    }, IntPtr.Zero);
    return f;
  }
  public static IntPtr FindButtonByText(IntPtr parent, string text) {
    IntPtr f = IntPtr.Zero;
    EnumChildWindows(parent, delegate(IntPtr h, IntPtr l) {
      var cn = new StringBuilder(256); GetClassName(h, cn, 256);
      if (cn.ToString() == "Button") {
        var sb = new StringBuilder(256); SMG(h, 0x000D, (IntPtr)256, sb);
        if (sb.ToString().Contains(text)) { f = h; return false; }
      }
      return true;
    }, IntPtr.Zero);
    return f;
  }
}
"@

function HwndOf($p) { $p.Refresh(); return [WE]::MainOf([uint32]$p.Id) }

# 开始抓包（20s 自动自然退出并落盘；全程流量约 14s）
# 注意：BPF 过滤器含空格，必须自带引号传给 Start-Process
$tsharkProc = Start-Process -FilePath $tshark `
  -ArgumentList "-i", $dev, "-f", "`"udp port 54321`"", "-a", "duration:20", "-w", $cap `
  -WindowStyle Hidden -PassThru
Start-Sleep -Milliseconds 1200

# ① A(房主) + B；B 发明文文本（加密关）
$a = Start-Process -FilePath $exe -ArgumentList "ZhangSan" -PassThru
Start-Sleep -Milliseconds 1200
$b = Start-Process -FilePath $exe -ArgumentList "LiSi","--send","hello-plaintext-12345","--senddelay","2500" -PassThru
Start-Sleep -Seconds 5

# ② 双方勾选"加密发送" → 房主分发组密钥
$ha = HwndOf $a; $hb = HwndOf $b
[WE]::SM([WE]::FindButtonByText($ha, "加密"), 0xF5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 900
[WE]::SM([WE]::FindButtonByText($hb, "加密"), 0xF5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
Start-Sleep -Seconds 2

# ③ B 发送加密文本（UI 输入 + 点击发送）
$input = [WE]::FindByClass($hb, "Edit", 1)
$btn = [WE]::FindByClass($hb, "Button", 0)
[WE]::SMT($input, 0x000C, [IntPtr]::Zero, "secret-cipher-6789") | Out-Null
[WE]::SM($btn, 0xF5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
Start-Sleep -Seconds 3

# 收尾：等 tshark 到时自然退出（强制杀会丢 pcapng 尾块）
Start-Sleep -Seconds 2
if (-not $tsharkProc.HasExited) { $tsharkProc.WaitForExit(25000) | Out-Null }
Stop-Process -Id $a.Id,$b.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 600
if (Test-Path $cap) { Write-Host "[demo] capture OK: $cap ($((Get-Item $cap).Length) bytes)" }
else { Write-Host "[demo] ERR: no capture"; exit 1 }
