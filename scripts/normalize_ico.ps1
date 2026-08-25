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

# Build a modern ICO containing several directory entries that all reference
# PNG-compressed image data. PNG icon resources are accepted by current rc.exe
# and avoid the legacy-DIB RC2176 failure seen with the checked-in .ico file.
$sizes = @(16, 32, 48, 256)
$headerSize = 6
$entrySize = 16
$offset = $headerSize + $entrySize * $sizes.Count

$stream = New-Object System.IO.MemoryStream
$writer = New-Object System.IO.BinaryWriter($stream)
try {
    $writer.Write([uint16]0)
    $writer.Write([uint16]1)
    $writer.Write([uint16]$sizes.Count)

    foreach ($size in $sizes) {
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

    foreach ($size in $sizes) {
        $writer.Write($png)
    }

    $writer.Flush()
    [System.IO.File]::WriteAllBytes($OutputPath, $stream.ToArray())
} finally {
    $writer.Dispose()
    $stream.Dispose()
}

if (-not (Test-Path $OutputPath) -or (Get-Item $OutputPath).Length -le ($headerSize + $entrySize * $sizes.Count)) {
    throw 'Generated ICO is invalid.'
}
