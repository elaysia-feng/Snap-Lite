param(
    [Parameter(Mandatory=$true)]
    [string]$OutputPath
)

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class SnapLiteNativeIcon {
    [DllImport("user32.dll", CharSet = CharSet.Auto)]
    public static extern bool DestroyIcon(IntPtr handle);
}
"@

$size = 256
$bitmap = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g = [System.Drawing.Graphics]::FromImage($bitmap)
$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality

$bounds = New-Object System.Drawing.Rectangle(0, 0, $size, $size)
$bg = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
    $bounds,
    [System.Drawing.Color]::FromArgb(255, 76, 172, 255),
    [System.Drawing.Color]::FromArgb(255, 22, 91, 235),
    45.0
)
$g.FillRectangle($bg, $bounds)

# soft cyan glow
$glow = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(42, 160, 238, 255))
$g.FillEllipse($glow, 20, 18, 210, 210)

# screenshot corner brackets
$framePen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(242, 250, 253, 255), 16)
$framePen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
$framePen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
$g.DrawLine($framePen, 34, 78, 34, 34)
$g.DrawLine($framePen, 34, 34, 78, 34)
$g.DrawLine($framePen, 178, 34, 222, 34)
$g.DrawLine($framePen, 222, 34, 222, 78)
$g.DrawLine($framePen, 34, 178, 34, 222)
$g.DrawLine($framePen, 34, 222, 78, 222)

# anime hair silhouette
$hairPen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(235, 225, 244, 255), 18)
$hairPen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
$hairPen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
$g.DrawBezier($hairPen, 57, 108, 88, 42, 164, 34, 204, 92)
$g.DrawBezier($hairPen, 70, 112, 99, 65, 133, 55, 139, 106)
$g.DrawBezier($hairPen, 188, 74, 210, 113, 203, 166, 178, 196)

# eye white shape
$eyePath = New-Object System.Drawing.Drawing2D.GraphicsPath
$eyePath.StartFigure()
$eyePath.AddBezier(54, 142, 87, 94, 168, 92, 205, 137)
$eyePath.AddBezier(205, 137, 166, 188, 91, 188, 54, 142)
$eyePath.CloseFigure()
$eyeWhite = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(248, 249, 253, 255))
$g.FillPath($eyeWhite, $eyePath)

# iris layers
$irisOuter = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 13, 71, 166))
$irisMid = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 44, 177, 255))
$irisInner = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 80, 235, 255))
$g.FillEllipse($irisOuter, 105, 103, 72, 72)
$g.FillEllipse($irisMid, 112, 111, 58, 58)
$g.FillEllipse($irisInner, 119, 133, 44, 31)
$pupil = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 10, 36, 94))
$g.FillEllipse($pupil, 132, 119, 22, 41)
$shine = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::White)
$g.FillEllipse($shine, 122, 116, 17, 17)
$g.FillEllipse($shine, 151, 139, 8, 8)

# upper lash
$lashPen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(255, 9, 30, 84), 9)
$lashPen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
$lashPen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
$g.DrawBezier($lashPen, 54, 141, 89, 94, 170, 94, 207, 136)

# little capture target + pointer in lower-right
$dashPen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(235, 255, 255, 255), 6)
$dashPen.DashPattern = @(1.2, 1.2)
$g.DrawRectangle($dashPen, 172, 169, 54, 50)
$cursorBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::White)
[System.Drawing.Point[]]$cursor = @(
    (New-Object System.Drawing.Point(202, 188)),
    (New-Object System.Drawing.Point(231, 211)),
    (New-Object System.Drawing.Point(216, 214)),
    (New-Object System.Drawing.Point(226, 233)),
    (New-Object System.Drawing.Point(216, 238)),
    (New-Object System.Drawing.Point(207, 219)),
    (New-Object System.Drawing.Point(197, 231))
)
$g.FillPolygon($cursorBrush, $cursor)

# sparkles
$spark = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(220, 224, 249, 255), 5)
$spark.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
$spark.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
$g.DrawLine($spark, 54, 104, 54, 126)
$g.DrawLine($spark, 43, 115, 65, 115)

$g.Dispose()
$bg.Dispose(); $glow.Dispose(); $framePen.Dispose(); $hairPen.Dispose()
$eyeWhite.Dispose(); $irisOuter.Dispose(); $irisMid.Dispose(); $irisInner.Dispose()
$pupil.Dispose(); $shine.Dispose(); $lashPen.Dispose(); $dashPen.Dispose(); $cursorBrush.Dispose(); $spark.Dispose(); $eyePath.Dispose()

$directory = Split-Path -Parent $OutputPath
if ($directory) { New-Item -ItemType Directory -Force -Path $directory | Out-Null }
$hIcon = $bitmap.GetHicon()
$icon = [System.Drawing.Icon]::FromHandle($hIcon)
$stream = [System.IO.File]::Open($OutputPath, [System.IO.FileMode]::Create)
$icon.Save($stream)
$stream.Dispose()
$icon.Dispose()
[SnapLiteNativeIcon]::DestroyIcon($hIcon) | Out-Null
$bitmap.Dispose()
