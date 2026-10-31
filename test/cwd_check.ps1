# 验证接收路径锚定 exe 目录：实例工作目录强制设为桌面，收到的文件必须出现在
# exe 同目录的 received\ 下，而不是桌面
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "mcast-chat\mcast-chat.exe"
$desktop = [Environment]::GetFolderPath('Desktop')
Get-Process mcast-chat -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500

$leaf = "cwdtest_" + (Get-Random) + ".bin"
$src = Join-Path $desktop $leaf
$bytes = New-Object byte[] 307200
(New-Object Random 9).NextBytes($bytes)
[IO.File]::WriteAllBytes($src, $bytes)

# 两个实例的工作目录都设为桌面（复现用户报告的场景）
$a = Start-Process -FilePath $exe -ArgumentList "ZhangSan" -WorkingDirectory $desktop -PassThru
Start-Sleep -Milliseconds 1500
$b = Start-Process -FilePath $exe -ArgumentList "LiSi","--sendfile",$src -WorkingDirectory $desktop -PassThru

$recvPath = Join-Path (Join-Path $root "mcast-chat\received") $leaf
$done = $false
for ($i = 0; $i -lt 20; $i++) {
  Start-Sleep -Seconds 2
  if (Test-Path $recvPath) { $done = $true; break }
}
$ok1 = $done
$ok2 = Test-Path $recvPath
$bad = Get-ChildItem -Path (Join-Path $desktop "received") -Filter $leaf -ErrorAction SilentlyContinue
$ok3 = ($null -eq $bad)
$ok4 = $false
if ($ok2) {
  $ok4 = ((Get-FileHash $src -Algorithm SHA256).Hash -eq (Get-FileHash $recvPath -Algorithm SHA256).Hash)
}
Stop-Process -Id $a.Id,$b.Id -Force -ErrorAction SilentlyContinue
Remove-Item $src -Force -ErrorAction SilentlyContinue
Write-Host "file-next-to-exe=$ok1 received=$ok2 not-on-desktop=$ok3 sha-match=$ok4"
if ($ok1 -and $ok2 -and $ok3 -and $ok4) { Write-Host "CWD-CHECK PASS"; exit 0 }
Write-Host "CWD-CHECK FAIL"
exit 1
