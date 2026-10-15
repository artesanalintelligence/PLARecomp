# Captures the client area of a window (by title fragment) with PrintWindow
# (PW_RENDERFULLCONTENT: works even if another window covers it). Meant to take the suyu reference
# while nvnvk draws in its own window. The default title fragment matches PLA.exe's window title.
#   .\winshot.ps1 -Title 'PLARecomp' -Out D:\x\suyu -At 20,30,40 [-Exclude 'nvnvk'] [-TimeoutSec 120]
param(
    [string]$Title = 'PLARecomp',
    [string]$Exclude = 'nvnvk',
    [Parameter(Mandatory)][string]$Out,
    [string]$At = '20,30,40',
    [int]$TimeoutSec = 120
)
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Text; using System.Runtime.InteropServices;
public static class WinShot {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  public static IntPtr Find(string part, string exclude) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, l) => {
      if (!IsWindowVisible(h)) return true;
      var sb = new StringBuilder(512); GetWindowText(h, sb, 512); var t = sb.ToString();
      if (t.Contains(part) && (exclude.Length == 0 || !t.Contains(exclude))) { found = h; return false; }
      return true; }, IntPtr.Zero);
    return found;
  }
}
'@
# Without this, with display scaling (125 %) GetClientRect returns 1024x576 and the shot comes out cropped.
[WinShot]::SetProcessDPIAware() | Out-Null
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$times = @($At -split ',' | ForEach-Object { [double]$_ })
$sw = [Diagnostics.Stopwatch]::StartNew()
$done = @{}
while ($sw.Elapsed.TotalSeconds -lt $TimeoutSec -and $done.Count -lt $times.Count) {
    Start-Sleep -Milliseconds 200
    foreach ($t in $times) {
        if ($done[$t] -or $sw.Elapsed.TotalSeconds -lt $t) { continue }
        $h = [WinShot]::Find($Title, $Exclude)
        if ($h -eq [IntPtr]::Zero) { continue }
        $r = New-Object WinShot+RECT
        [WinShot]::GetClientRect($h, [ref]$r) | Out-Null
        $w = $r.R - $r.L; $hh = $r.B - $r.T
        if ($w -le 0 -or $hh -le 0) { continue }
        $bmp = New-Object Drawing.Bitmap $w, $hh
        $g = [Drawing.Graphics]::FromImage($bmp)
        $dc = $g.GetHdc()
        [WinShot]::PrintWindow($h, $dc, 3) | Out-Null   # PW_CLIENTONLY | PW_RENDERFULLCONTENT
        $g.ReleaseHdc($dc); $g.Dispose()
        $f = Join-Path $Out ("suyu-{0:000}s.png" -f $t)
        $bmp.Save($f, [Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
        Write-Host "winshot: $f (${w}x${hh})"
        $done[$t] = $true
    }
}
