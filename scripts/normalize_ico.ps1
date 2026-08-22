param(
    [Parameter(Mandatory=$true)][string]$InputPath,
    [Parameter(Mandatory=$true)][string]$OutputPath
)

$bytes = [System.IO.File]::ReadAllBytes($InputPath)
if ($bytes.Length -lt 6) { throw 'Invalid ICO header.' }
if ([BitConverter]::ToUInt16($bytes, 0) -ne 0 -or [BitConverter]::ToUInt16($bytes, 2) -ne 1) {
    throw 'Not a Windows ICO file.'
}
$count = [BitConverter]::ToUInt16($bytes, 4)
if ($count -lt 1 -or $bytes.Length -lt (6 + 16 * $count)) {
    throw 'Invalid ICO directory.'
}
for ($i = 0; $i -lt $count; $i++) {
    $entry = 6 + 16 * $i
    $planes = [BitConverter]::ToUInt16($bytes, $entry + 4)
    if ($planes -eq 0) {
        $bytes[$entry + 4] = 1
        $bytes[$entry + 5] = 0
    } elseif ($planes -ne 1) {
        throw "Unexpected ICO planes value $planes at entry $i"
    }
}
[System.IO.File]::WriteAllBytes($OutputPath, $bytes)
