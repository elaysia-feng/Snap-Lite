param(
    [Parameter(Mandatory=$true)][string]$InputBase64Path,
    [Parameter(Mandatory=$true)][string]$OutputPath
)

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class SnapLiteNativeIcon {
    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool DestroyIcon(IntPtr hIcon);
}
'@

$base64 = (Get-Content $InputBase64Path -Raw).Trim()
if ([string]::IsNullOrWhiteSpace($base64)) { throw 'Icon source is empty.' }
$pngBytes = [Convert]::FromBase64String($base64)
$stream = New-Object System.IO.MemoryStream(,$pngBytes)
$source = [System.Drawing.Image]::FromStream($stream)
$bitmap = New-Object System.Drawing.Bitmap 128, 128, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$graphics.Clear([System.Drawing.Color]::Transparent)
$graphics.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
$graphics.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
$graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
$graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
$graphics.DrawImage($source, 0, 0, 128, 128)
$graphics.Dispose()

$hIcon = $bitmap.GetHicon()
if ($hIcon -eq [IntPtr]::Zero) { throw 'Failed to create Windows HICON.' }
try {
    $icon = [System.Drawing.Icon]::FromHandle($hIcon)
    $file = [System.IO.File]::Create($OutputPath)
    try {
        $icon.Save($file)
    } finally {
        $file.Dispose()
        $icon.Dispose()
    }
} finally {
    [SnapLiteNativeIcon]::DestroyIcon($hIcon) | Out-Null
    $bitmap.Dispose()
    $source.Dispose()
    $stream.Dispose()
}

if (-not (Test-Path $OutputPath) -or (Get-Item $OutputPath).Length -lt 100) {
    throw 'Generated ICO is invalid.'
}
