Add-Type -AssemblyName System.Drawing,System.Windows.Forms
$name = (Get-CimInstance Win32_Process -Filter "Name='munkiki.exe'" | Select-Object -First 1).ProcessId
$h = (Get-Process -Id $name -Module:$false).MainWindowHandle
if (-not $h) { Write-Error "no window"; exit 1 }
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class W {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
  [StructLayout(LayoutKind.Sequential)] public struct R { public int l,t,r,b; }
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
}
"@
[W]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 300
$rc = New-Object W+R
[W]::GetWindowRect($h, [ref]$rc) | Out-Null
$w = $rc.r - $rc.l; $hh = $rc.b - $rc.t
$bmp = New-Object System.Drawing.Bitmap($w, $hh)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($rc.l, $rc.t, 0, 0, (New-Object System.Drawing.Size($w, $hh)))
$out = Join-Path $env:TEMP "munkiki_shot.png"
$bmp.Save($out)
Write-Host "saved $out ($w x $hh)"
