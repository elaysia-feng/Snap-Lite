param(
    [Parameter(Mandatory=$true)][string]$InputBase64Path,
    [Parameter(Mandatory=$true)][string]$OutputPath
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$base64 = (Get-Content $InputBase64Path -Raw).Trim()
if ([string]::IsNullOrWhiteSpace($base64)) { throw 'Icon source is empty.' }
$sourceBytes = [Convert]::FromBase64String($base64)
if ($sourceBytes.Length -lt 24) { throw 'PNG source is too small.' }

$input = New-Object System.IO.MemoryStream(,$sourceBytes)
$source = [System.Drawing.Image]::FromStream($input)
$sizes = @(16, 32, 48, 256)
$entries = @()

try {
    foreach ($size in $sizes) {
        $bitmap = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        try {
            $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
            try {
                $graphics.Clear([System.Drawing.Color]::Transparent)
                $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
                $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
                $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
                $graphics.DrawImage($source, 0, 0, $size, $size)
            } finally {
                $graphics.Dispose()
            }

            $pngStream = New-Object System.IO.MemoryStream
            try {
                $bitmap.Save($pngStream, [System.Drawing.Imaging.ImageFormat]::Png)
                $entries += ,@($size, $pngStream.ToArray())
            } finally {
                $pngStream.Dispose()
            }
        } finally {
            $bitmap.Dispose()
        }
    }
} finally {
    $source.Dispose()
    $input.Dispose()
}

$headerSize = 6
$entrySize = 16
$offset = $headerSize + $entrySize * $entries.Count
$stream = New-Object System.IO.MemoryStream
$writer = New-Object System.IO.BinaryWriter($stream)
try {
    $writer.Write([uint16]0)
    $writer.Write([uint16]1)
    $writer.Write([uint16]$entries.Count)

    foreach ($entry in $entries) {
        $size = [int]$entry[0]
        $png = [byte[]]$entry[1]
        $dimension = if ($size -eq 256) { [byte]0 } else { [byte]$size }
        $writer.Write($dimension)
        $writer.Write($dimension)
        $writer.Write([byte]0)
        $writer.Write([byte]0)
        $writer.Write([uint16]1)
        $writer.Write([uint16]32)
        $writer.Write([uint32]$png.Length)
        $writer.Write([uint32]$offset)
        $offset += $png.Length
    }

    foreach ($entry in $entries) {
        $writer.Write([byte[]]$entry[1])
    }

    $writer.Flush()
    [System.IO.File]::WriteAllBytes($OutputPath, $stream.ToArray())
} finally {
    $writer.Dispose()
    $stream.Dispose()
}

if (-not (Test-Path $OutputPath) -or (Get-Item $OutputPath).Length -le ($headerSize + $entrySize * $entries.Count)) {
    throw 'Generated ICO is invalid.'
}
