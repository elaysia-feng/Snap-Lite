param(
    [Parameter(Mandatory=$true)][string]$InputBase64Path,
    [Parameter(Mandatory=$true)][string]$OutputPath
)

$ErrorActionPreference = 'Stop'

$base64 = (Get-Content $InputBase64Path -Raw).Trim()
if ([string]::IsNullOrWhiteSpace($base64)) { throw 'Icon source is empty.' }
$png = [Convert]::FromBase64String($base64)
if ($png.Length -lt 24) { throw 'PNG source is too small.' }

$signature = [byte[]](137,80,78,71,13,10,26,10)
for ($i = 0; $i -lt $signature.Length; $i++) {
    if ($png[$i] -ne $signature[$i]) { throw 'Icon source is not a PNG.' }
}
function Read-UInt32BE([byte[]]$bytes, [int]$offset) {
    return ([uint32]$bytes[$offset] -shl 24) -bor
           ([uint32]$bytes[$offset + 1] -shl 16) -bor
           ([uint32]$bytes[$offset + 2] -shl 8) -bor
           [uint32]$bytes[$offset + 3]
}
$width = Read-UInt32BE $png 16
$height = Read-UInt32BE $png 20
if ($width -lt 1 -or $height -lt 1 -or $width -gt 256 -or $height -gt 256) {
    throw "PNG dimensions $width x $height are not valid for this ICO source."
}
$widthByte = if ($width -eq 256) { [byte]0 } else { [byte]$width }
$heightByte = if ($height -eq 256) { [byte]0 } else { [byte]$height }

# ICO header + one ICONDIRENTRY + PNG payload. PNG-compressed icon entries are
# natively supported by modern Windows and avoid legacy DIB resource issues.
$headerSize = 6
$entrySize = 16
$offset = $headerSize + $entrySize
$stream = New-Object System.IO.MemoryStream
$writer = New-Object System.IO.BinaryWriter($stream)
try {
    $writer.Write([uint16]0)
    $writer.Write([uint16]1)
    $writer.Write([uint16]1)
    $writer.Write($widthByte)
    $writer.Write($heightByte)
    $writer.Write([byte]0)
    $writer.Write([byte]0)
    $writer.Write([uint16]1)
    $writer.Write([uint16]32)
    $writer.Write([uint32]$png.Length)
    $writer.Write([uint32]$offset)
    $writer.Write($png)
    $writer.Flush()
    [System.IO.File]::WriteAllBytes($OutputPath, $stream.ToArray())
} finally {
    $writer.Dispose()
    $stream.Dispose()
}

if (-not (Test-Path $OutputPath) -or (Get-Item $OutputPath).Length -le $offset) {
    throw 'Generated ICO is invalid.'
}
