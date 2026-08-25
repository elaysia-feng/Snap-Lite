param(
    [Parameter(Mandatory=$true)][string]$InputBase64Path,
    [Parameter(Mandatory=$true)][string]$OutputPath
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName PresentationCore
Add-Type -AssemblyName WindowsBase

$base64 = (Get-Content $InputBase64Path -Raw).Trim()
if ([string]::IsNullOrWhiteSpace($base64)) {
    throw 'Icon PNG source is empty.'
}

$png = [Convert]::FromBase64String($base64)
if ($png.Length -lt 24) {
    throw 'PNG source is too small.'
}

$signature = [byte[]](137,80,78,71,13,10,26,10)
for ($i = 0; $i -lt $signature.Length; $i++) {
    if ($png[$i] -ne $signature[$i]) {
        throw 'Icon source is not a PNG.'
    }
}

function Read-PngU32BE([byte[]]$data, [int]$offset) {
    return ([uint32]$data[$offset] -shl 24) -bor
           ([uint32]$data[$offset + 1] -shl 16) -bor
           ([uint32]$data[$offset + 2] -shl 8) -bor
           [uint32]$data[$offset + 3]
}

$input = New-Object System.IO.MemoryStream(,$png)
try {
    $decoder = [System.Windows.Media.Imaging.PngBitmapDecoder]::new(
        $input,
        [System.Windows.Media.Imaging.BitmapCreateOptions]::PreservePixelFormat,
        [System.Windows.Media.Imaging.BitmapCacheOption]::OnLoad
    )
    $source = $decoder.Frames[0]
} finally {
    $input.Dispose()
}

if (-not $source -or $source.PixelWidth -lt 1 -or $source.PixelHeight -lt 1) {
    throw 'WIC could not decode the icon PNG source.'
}

$sizes = @(16, 32, 48, 128)
$entries = @()

foreach ($size in $sizes) {
    if ($source.PixelWidth -eq $size -and $source.PixelHeight -eq $size) {
        $frame = $source
    } else {
        $scaleX = $size / [double]$source.PixelWidth
        $scaleY = $size / [double]$source.PixelHeight
        $transform = [System.Windows.Media.ScaleTransform]::new($scaleX, $scaleY)
        $frame = [System.Windows.Media.Imaging.TransformedBitmap]::new($source, $transform)
    }

    $encoder = [System.Windows.Media.Imaging.PngBitmapEncoder]::new()
    $encoder.Frames.Add([System.Windows.Media.Imaging.BitmapFrame]::Create($frame))

    $pngStream = New-Object System.IO.MemoryStream
    try {
        $encoder.Save($pngStream)
        $layer = $pngStream.ToArray()
    } finally {
        $pngStream.Dispose()
    }

    $actualWidth = [int](Read-PngU32BE $layer 16)
    $actualHeight = [int](Read-PngU32BE $layer 20)
    if ($actualWidth -ne $size -or $actualHeight -ne $size) {
        throw "Generated PNG layer ${actualWidth}x${actualHeight} does not match requested ${size}x${size}."
    }

    $entries += [PSCustomObject]@{
        Size = $size
        Data = $layer
    }
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
        $dimension = if ($entry.Size -eq 256) { [byte]0 } else { [byte]$entry.Size }
        $writer.Write($dimension)
        $writer.Write($dimension)
        $writer.Write([byte]0)
        $writer.Write([byte]0)
        $writer.Write([uint16]1)
        $writer.Write([uint16]32)
        $writer.Write([uint32]$entry.Data.Length)
        $writer.Write([uint32]$offset)
        $offset += $entry.Data.Length
    }

    foreach ($entry in $entries) {
        $writer.Write([byte[]]$entry.Data)
    }

    $writer.Flush()
    [System.IO.File]::WriteAllBytes($OutputPath, $stream.ToArray())
} finally {
    $writer.Dispose()
    $stream.Dispose()
}

Write-Host "Generated valid multi-size ICO: $($sizes -join ', ') px -> $OutputPath"
