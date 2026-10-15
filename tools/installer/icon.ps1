# Draws the setup's own icon (no third-party marks): a violet rounded square with a ring and an arrow into a tray.
# ICO with PNG images from 16 to 256 px.
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\installer\icon.ps1 -Out <file.ico>
param([Parameter(Mandatory = $true)][string]$Out)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

function Draw([int]$n) {
    $bmp = New-Object System.Drawing.Bitmap $n, $n, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.Clear([System.Drawing.Color]::Transparent)
    $m = [single]($n * 0.04); $s = [single]($n - 2 * $m); $r = [single]($n * 0.22)
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddArc($m, $m, 2 * $r, 2 * $r, 180, 90)
    $path.AddArc($m + $s - 2 * $r, $m, 2 * $r, 2 * $r, 270, 90)
    $path.AddArc($m + $s - 2 * $r, $m + $s - 2 * $r, 2 * $r, 2 * $r, 0, 90)
    $path.AddArc($m, $m + $s - 2 * $r, 2 * $r, 2 * $r, 90, 90)
    $path.CloseFigure()
    $rect = New-Object System.Drawing.RectangleF $m, $m, $s, $s
    $br = New-Object System.Drawing.Drawing2D.LinearGradientBrush $rect, ([System.Drawing.Color]::FromArgb(255, 185, 125, 242)), ([System.Drawing.Color]::FromArgb(255, 74, 36, 128)), 60.0
    $g.FillPath($br, $path)
    $c = [single]($n / 2)
    $rr = [single]($n * 0.30)
    $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(110, 255, 255, 255)), ([single]([Math]::Max(1.0, $n * 0.045)))
    $g.DrawArc($pen, $c - $rr, $c - $rr, 2 * $rr, 2 * $rr, 200, 300)
    $white = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 250, 247, 255))
    $w = [single]($n * 0.10)
    $g.FillRectangle($white, $c - $w / 2, [single]($n * 0.24), $w, [single]($n * 0.26))
    $pts = @(
        (New-Object System.Drawing.PointF ([single]($c - $n * 0.17)), ([single]($n * 0.46))),
        (New-Object System.Drawing.PointF ([single]($c + $n * 0.17)), ([single]($n * 0.46))),
        (New-Object System.Drawing.PointF $c, ([single]($n * 0.64)))
    )
    $g.FillPolygon($white, $pts)
    $g.FillRectangle($white, [single]($n * 0.27), [single]($n * 0.70), [single]($n * 0.46), [single]([Math]::Max(1.0, $n * 0.07)))
    $g.Dispose()
    return $bmp
}

$sizes = 16, 24, 32, 48, 64, 128, 256
$pngs = foreach ($t in $sizes) {
    $b = Draw $t
    $ms = New-Object System.IO.MemoryStream
    $b.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $b.Dispose()
    , $ms.ToArray()
}
$fs = [System.IO.File]::Create($Out)
$bw = New-Object System.IO.BinaryWriter $fs
$bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$sizes.Count)
$off = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $t = $sizes[$i]; $d = $pngs[$i]
    $bw.Write([byte]($t % 256)); $bw.Write([byte]($t % 256)); $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([uint16]1); $bw.Write([uint16]32); $bw.Write([uint32]$d.Length); $bw.Write([uint32]$off)
    $off += $d.Length
}
foreach ($d in $pngs) { $bw.Write($d) }
$bw.Close()
