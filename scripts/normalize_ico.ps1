param(
    [Parameter(Mandatory=$true)][string]$InputIconPath,
    [Parameter(Mandatory=$true)][string]$OutputPath
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $InputIconPath)) {
    throw "Icon source does not exist: $InputIconPath"
}

$bytes = [System.IO.File]::ReadAllBytes($InputIconPath)
if ($bytes.Length -lt 22) {
    throw 'ICO source is too small.'
}

function Read-U16([byte[]]$data, [int]$offset) {
    return [BitConverter]::ToUInt16($data, $offset)
}

function Read-U32([byte[]]$data, [int]$offset) {
    return [BitConverter]::ToUInt32($data, $offset)
}

function Read-PngU32BE([byte[]]$data, [int]$offset) {
    return ([uint32]$data[$offset] -shl 24) -bor
           ([uint32]$data[$offset + 1] -shl 16) -bor
           ([uint32]$data[$offset + 2] -shl 8) -bor
           [uint32]$data[$offset + 3]
}

if ((Read-U16 $bytes 0) -ne 0 -or (Read-U16 $bytes 2) -ne 1) {
    throw 'Source is not a Windows ICO file.'
}

$count = Read-U16 $bytes 4
$directoryEnd = 6 + 16 * $count
if ($count -lt 1 -or $bytes.Length -lt $directoryEnd) {
    throw 'ICO directory is invalid.'
}

$pngSignature = [byte[]](137,80,78,71,13,10,26,10)
$entries = @()

for ($i = 0; $i -lt $count; $i++) {
    $entryOffset = 6 + 16 * $i
    $widthByte = $bytes[$entryOffset]
    $heightByte = $bytes[$entryOffset + 1]
    $width = if ($widthByte -eq 0) { 256 } else { [int]$widthByte }
    $height = if ($heightByte -eq 0) { 256 } else { [int]$heightByte }
    $payloadSize = [int](Read-U32 $bytes ($entryOffset + 8))
    $payloadOffset = [int](Read-U32 $bytes ($entryOffset + 12))

    if ($payloadSize -le 0 -or $payloadOffset -lt 0 -or
        ($payloadOffset + $payloadSize) -gt $bytes.Length) {
        Write-Host "Skipping truncated ICO layer: ${width}x${height}"
        continue
    }

    $payload = New-Object byte[] $payloadSize
    [Array]::Copy($bytes, $payloadOffset, $payload, 0, $payloadSize)

    $isPng = $payload.Length -ge 24
    if ($isPng) {
        for ($j = 0; $j -lt $pngSignature.Length; $j++) {
            if ($payload[$j] -ne $pngSignature[$j]) {
                $isPng = $false
                break
            }
        }
    }
    if (-not $isPng) {
        Write-Host "Skipping legacy/non-PNG ICO layer: ${width}x${height}"
        continue
    }

    $pngWidth = [int](Read-PngU32BE $payload 16)
    $pngHeight = [int](Read-PngU32BE $payload 20)
    if ($pngWidth -ne $width -or $pngHeight -ne $height) {
        Write-Host "Skipping mismatched ICO layer: metadata ${width}x${height}, PNG ${pngWidth}x${pngHeight}"
        continue
    }

    # Only keep the complete shell-critical layers. The checked-in source ICO
    # is truncated after these entries, so larger entries must not be copied.
    if ($width -notin @(16, 32) -or $height -ne $width) {
        continue
    }

    $entries += [PSCustomObject]@{
        WidthByte  = $widthByte
        HeightByte = $heightByte
        ColorCount = $bytes[$entryOffset + 2]
        Reserved   = $bytes[$entryOffset + 3]
        Planes     = Read-U16 $bytes ($entryOffset + 4)
        BitCount   = Read-U16 $bytes ($entryOffset + 6)
        Width      = $width
        Height     = $height
        Data       = $payload
    }
}

foreach ($required in @(16, 32)) {
    if (-not ($entries | Where-Object { $_.Width -eq $required -and $_.Height -eq $required })) {
        throw "Required ${required}x${required} PNG icon layer is missing."
    }
}

$entries = @($entries | Sort-Object Width)
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
        $writer.Write([byte]$entry.WidthByte)
        $writer.Write([byte]$entry.HeightByte)
        $writer.Write([byte]$entry.ColorCount)
        $writer.Write([byte]$entry.Reserved)
        $writer.Write([uint16]$entry.Planes)
        $writer.Write([uint16]$entry.BitCount)
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

Write-Host "Rebuilt valid Windows ICO with 16x16 and 32x32 PNG layers: $OutputPath"
