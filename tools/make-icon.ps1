# Draws res\MeetingRecorder.ico: a red record dot in a speech bubble, at the
# sizes Windows asks for (16 to 256 px, PNG-compressed entries).
#   powershell -ExecutionPolicy Bypass -File tools\make-icon.ps1
# -StoreLogo writes build\store\logo-1080.png for the Microsoft Store listing
# instead.
param([switch]$StoreLogo)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

function New-IconImage([int]$size) {
    $bitmap = New-Object System.Drawing.Bitmap $size, $size
    $g = [System.Drawing.Graphics]::FromImage($bitmap)
    $g.SmoothingMode = 'AntiAlias'
    $g.Clear([System.Drawing.Color]::Transparent)
    $s = $size / 256.0

    # Speech bubble: rounded square with a tail at the bottom left.
    $bubble = New-Object System.Drawing.Drawing2D.GraphicsPath
    $x = 12 * $s; $y = 12 * $s; $w = 232 * $s; $h = 196 * $s; $r = 56 * $s
    $bubble.AddArc($x, $y, 2 * $r, 2 * $r, 180, 90)
    $bubble.AddArc($x + $w - 2 * $r, $y, 2 * $r, 2 * $r, 270, 90)
    $bubble.AddArc($x + $w - 2 * $r, $y + $h - 2 * $r, 2 * $r, 2 * $r, 0, 90)
    $bubble.AddLine($x + $w - $r, $y + $h, 96 * $s, $y + $h)
    $bubble.AddLine(96 * $s, $y + $h, 44 * $s, 248 * $s)
    $bubble.AddLine(44 * $s, 248 * $s, 56 * $s, $y + $h)
    $bubble.AddArc($x, $y + $h - 2 * $r, 2 * $r, 2 * $r, 90, 90)
    $bubble.CloseFigure()
    $g.FillPath((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 30, 41, 59))), $bubble)

    # Record symbol: white ring around a red dot.
    $cx = 128 * $s; $cy = 110 * $s
    $ring = 70 * $s; $dot = 50 * $s
    $g.FillEllipse([System.Drawing.Brushes]::White, $cx - $ring, $cy - $ring, 2 * $ring, 2 * $ring)
    $g.FillEllipse((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 220, 38, 38))),
                   $cx - $dot, $cy - $dot, 2 * $dot, 2 * $dot)
    $g.Dispose()

    $stream = New-Object System.IO.MemoryStream
    $bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
    $bitmap.Dispose()
    return , $stream.ToArray()
}

if ($StoreLogo) {
    $dir = Join-Path (Split-Path -Parent $PSScriptRoot) 'build\store'
    New-Item -ItemType Directory -Force $dir | Out-Null
    $path = Join-Path $dir 'logo-1080.png'
    [System.IO.File]::WriteAllBytes($path, (New-IconImage 1080))
    "Wrote $path"
    return
}

$sizes = 16, 20, 24, 32, 40, 48, 64, 256
$images = foreach ($size in $sizes) { , (New-IconImage $size) }

$out = New-Object System.IO.MemoryStream
$writer = New-Object System.IO.BinaryWriter $out
$writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $dim = if ($sizes[$i] -ge 256) { 0 } else { $sizes[$i] }
    $writer.Write([byte]$dim); $writer.Write([byte]$dim); $writer.Write([byte]0); $writer.Write([byte]0)
    $writer.Write([uint16]1); $writer.Write([uint16]32)
    $writer.Write([uint32]$images[$i].Length); $writer.Write([uint32]$offset)
    $offset += $images[$i].Length
}
foreach ($image in $images) { $writer.Write($image) }
$writer.Flush()

$path = Join-Path (Split-Path -Parent $PSScriptRoot) 'res\MeetingRecorder.ico'
[System.IO.File]::WriteAllBytes($path, $out.ToArray())
"Wrote $path"
