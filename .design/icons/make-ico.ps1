param(
    # Repository root. Passed in rather than written here: this file stays pure
    # ASCII so PowerShell 5.1 reads it correctly whatever the checkout path is.
    [Parameter(Mandatory = $true)][string]$Repo,
    [string]$WorkDir
)

# Rasterises the hand-tuned SVGs and assembles a multi-size .ico.
#
# Each frame comes from the cut drawn for that size where one exists; the rest
# come from the 256 master. Rendering is headless Edge: the SVG is inlined as a
# data: URL (a file:// page cannot load a sibling file:// image), the page
# background is transparent, and the shot is taken in a roomy window and then
# cropped, so no size runs into a minimum-window limit.

$ErrorActionPreference = "Stop"
$iconDir = Join-Path $Repo ".design\icons"
if ([string]::IsNullOrEmpty($WorkDir)) { $WorkDir = Join-Path $PSScriptRoot "ico" }
$work = $WorkDir
$icoOut = Join-Path $Repo "yip-app\Assets\yip.ico"
$edge = "C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"

$frames = @(
    @{ size = 16;  src = "yip-16.svg" },
    @{ size = 20;  src = "yip-256.svg" },
    @{ size = 24;  src = "yip-24.svg" },
    @{ size = 32;  src = "yip-32.svg" },
    @{ size = 40;  src = "yip-256.svg" },
    @{ size = 48;  src = "yip-48.svg" },
    @{ size = 64;  src = "yip-256.svg" },
    @{ size = 128; src = "yip-256.svg" },
    @{ size = 256; src = "yip-256.svg" }
)

New-Item -ItemType Directory -Force -Path $work | Out-Null
New-Item -ItemType Directory -Force -Path (Split-Path $icoOut) | Out-Null
Add-Type -AssemblyName System.Drawing

$shell = 300   # window size to shoot into; every frame is cropped from its corner

foreach ($f in $frames) {
    $size = $f.size
    $svg = [Convert]::ToBase64String([System.IO.File]::ReadAllBytes((Join-Path $iconDir $f.src)))
    $html = '<!doctype html><meta charset="utf-8">' +
            '<style>html,body{margin:0;padding:0;background:transparent}' +
            'img{display:block;width:' + $size + 'px;height:' + $size + 'px}</style>' +
            '<img src="data:image/svg+xml;base64,' + $svg + '">'
    $page = Join-Path $work "frame-$size.html"
    Set-Content -Path $page -Value $html -Encoding utf8

    $raw = Join-Path $work "raw-$size.png"
    $edgeArgs = @("--headless=new", "--disable-gpu", "--hide-scrollbars", "--no-first-run",
        "--force-device-scale-factor=1", "--user-data-dir=$work\profile-$size",
        "--default-background-color=00000000", "--window-size=$shell,$shell",
        "--virtual-time-budget=5000", "--screenshot=$raw", ([uri]$page).AbsoluteUri)
    Start-Process -FilePath $edge -ArgumentList $edgeArgs -Wait -WindowStyle Hidden
    if (-not (Test-Path $raw)) { throw "no render for size $size" }

    $full = [System.Drawing.Bitmap]::FromFile($raw)
    try {
        $rect = New-Object System.Drawing.Rectangle 0, 0, $size, $size
        $crop = $full.Clone($rect, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        try { $crop.Save((Join-Path $work "yip-$size.png"), [System.Drawing.Imaging.ImageFormat]::Png) }
        finally { $crop.Dispose() }
    } finally { $full.Dispose() }
}

# --- Assemble the .ico ---
# Frames up to 128 go in as 32-bit DIBs (widest tool support, rc.exe included);
# 256 goes in PNG-compressed, which is the standard for that size.

function Get-DibBytes([string]$pngPath) {
    $bmp = [System.Drawing.Bitmap]::FromFile($pngPath)
    try {
        $w = $bmp.Width; $h = $bmp.Height
        $ms = New-Object System.IO.MemoryStream
        $bw = New-Object System.IO.BinaryWriter($ms)
        $bw.Write([uint32]40); $bw.Write([int32]$w); $bw.Write([int32]($h * 2))
        $bw.Write([uint16]1); $bw.Write([uint16]32); $bw.Write([uint32]0)
        $bw.Write([uint32]($w * $h * 4))
        $bw.Write([int32]0); $bw.Write([int32]0); $bw.Write([uint32]0); $bw.Write([uint32]0)
        $rect = New-Object System.Drawing.Rectangle 0, 0, $w, $h
        $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
            [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        try {
            $row = New-Object byte[] ($w * 4)
            for ($y = $h - 1; $y -ge 0; $y--) {
                $ptr = [IntPtr]::Add($data.Scan0, $y * $data.Stride)
                [System.Runtime.InteropServices.Marshal]::Copy($ptr, $row, 0, $w * 4)
                $bw.Write($row)
            }
        } finally { $bmp.UnlockBits($data) }
        # AND mask: 1bpp, rows padded to 4 bytes, all zero - alpha does the work.
        $stride = [math]::Floor(($w + 31) / 32) * 4
        $blank = New-Object byte[] $stride
        for ($y = 0; $y -lt $h; $y++) { $bw.Write($blank) }
        $bw.Flush()
        return , $ms.ToArray()
    } finally { $bmp.Dispose() }
}

$images = @()
foreach ($f in $frames) {
    $png = Join-Path $work "yip-$($f.size).png"
    if ($f.size -eq 256) { $payload = [System.IO.File]::ReadAllBytes($png) }
    else { $payload = Get-DibBytes $png }
    $images += , @{ size = $f.size; bytes = $payload }
}

$ms = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter($ms)
$bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$images.Count)
$offset = 6 + 16 * $images.Count
foreach ($img in $images) {
    $dim = $img.size; if ($dim -ge 256) { $dim = 0 }
    $bw.Write([byte]$dim); $bw.Write([byte]$dim); $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([uint16]1); $bw.Write([uint16]32)
    $bw.Write([uint32]$img.bytes.Length); $bw.Write([uint32]$offset)
    $offset += $img.bytes.Length
}
foreach ($img in $images) { $bw.Write([byte[]]$img.bytes) }
$bw.Flush()
[System.IO.File]::WriteAllBytes($icoOut, $ms.ToArray())
$bw.Dispose(); $ms.Dispose()

# --- Report what went in ---
foreach ($f in $frames) {
    $bmp = [System.Drawing.Bitmap]::FromFile((Join-Path $work "yip-$($f.size).png"))
    try {
        $mid = [int]($f.size / 2)
        "{0,3}px from {1,-14} {2}x{3}  corner A={4}  centre={5}" -f $f.size, $f.src, $bmp.Width, $bmp.Height,
            $bmp.GetPixel(0, 0).A, $bmp.GetPixel($mid, $mid).Name
    } finally { $bmp.Dispose() }
}
"ico: $icoOut  $((Get-Item $icoOut).Length) bytes, $($images.Count) frames"


