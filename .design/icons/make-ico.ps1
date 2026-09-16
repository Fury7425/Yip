param(
    # Repository root. Passed in rather than written here: this file stays pure
    # ASCII so PowerShell 5.1 reads it correctly whatever the checkout path is.
    [Parameter(Mandatory = $true)][string]$Repo,
    [string]$WorkDir
)

# Rasterises the hand-tuned SVGs and assembles the app .ico plus the two tray
# .ico files.
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
$assetDir = Join-Path $Repo "yip-app\Assets"
$edge = "C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"

$appFrames = @(
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

# The notification area asks for 16 px at 100% scaling and 20 / 24 / 32 at
# 125 / 150 / 200%. There is one tray cut per state and it scales cleanly, so
# every frame comes from it rather than from the tile master - a tile at 16 px
# is a muddy square, which is why these glyphs were drawn in the first place.
$trayIdleFrames = @(
    @{ size = 16; src = "tray-idle-16.svg" },
    @{ size = 20; src = "tray-idle-16.svg" },
    @{ size = 24; src = "tray-idle-16.svg" },
    @{ size = 32; src = "tray-idle-16.svg" }
)
$trayRecFrames = @(
    @{ size = 16; src = "tray-recording-16.svg" },
    @{ size = 20; src = "tray-recording-16.svg" },
    @{ size = 24; src = "tray-recording-16.svg" },
    @{ size = 32; src = "tray-recording-16.svg" }
)

New-Item -ItemType Directory -Force -Path $work | Out-Null
New-Item -ItemType Directory -Force -Path $assetDir | Out-Null
Add-Type -AssemblyName System.Drawing

$shell = 300   # window size to shoot into; every frame is cropped from its corner

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

# Renders every frame of one icon and writes the assembled .ico. $name only
# names the intermediates under $work, so two icons that ask for the same pixel
# size do not overwrite each other's PNGs.
function Build-Ico([string]$name, [array]$frames, [string]$out) {
    foreach ($f in $frames) {
        $size = $f.size
        $svg = [Convert]::ToBase64String([System.IO.File]::ReadAllBytes((Join-Path $iconDir $f.src)))
        $html = '<!doctype html><meta charset="utf-8">' +
                '<style>html,body{margin:0;padding:0;background:transparent}' +
                'img{display:block;width:' + $size + 'px;height:' + $size + 'px}</style>' +
                '<img src="data:image/svg+xml;base64,' + $svg + '">'
        $page = Join-Path $work "$name-frame-$size.html"
        Set-Content -Path $page -Value $html -Encoding utf8

        $raw = Join-Path $work "$name-raw-$size.png"
        $edgeArgs = @("--headless=new", "--disable-gpu", "--hide-scrollbars", "--no-first-run",
            "--force-device-scale-factor=1", "--user-data-dir=$work\profile-$size",
            "--default-background-color=00000000", "--window-size=$shell,$shell",
            "--virtual-time-budget=5000", "--screenshot=$raw", ([uri]$page).AbsoluteUri)
        Start-Process -FilePath $edge -ArgumentList $edgeArgs -Wait -WindowStyle Hidden
        if (-not (Test-Path $raw)) { throw "no render for $name size $size" }

        $full = [System.Drawing.Bitmap]::FromFile($raw)
        try {
            $rect = New-Object System.Drawing.Rectangle 0, 0, $size, $size
            $crop = $full.Clone($rect, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
            try { $crop.Save((Join-Path $work "$name-$size.png"), [System.Drawing.Imaging.ImageFormat]::Png) }
            finally { $crop.Dispose() }
        } finally { $full.Dispose() }
    }

    # --- Assemble the .ico ---
    # Frames up to 128 go in as 32-bit DIBs (widest tool support, rc.exe
    # included); 256 goes in PNG-compressed, the standard for that size.

    $images = @()
    foreach ($f in $frames) {
        $png = Join-Path $work "$name-$($f.size).png"
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
    [System.IO.File]::WriteAllBytes($out, $ms.ToArray())
    $bw.Dispose(); $ms.Dispose()

    # --- Report what went in ---
    foreach ($f in $frames) {
        $bmp = [System.Drawing.Bitmap]::FromFile((Join-Path $work "$name-$($f.size).png"))
        try {
            $mid = [int]($f.size / 2)
            "{0,3}px from {1,-22} {2}x{3}  corner A={4}  centre={5}" -f $f.size, $f.src, $bmp.Width, $bmp.Height,
                $bmp.GetPixel(0, 0).A, $bmp.GetPixel($mid, $mid).Name
        } finally { $bmp.Dispose() }
    }
    "ico: $out  $((Get-Item $out).Length) bytes, $($images.Count) frames"
    ""
}

Build-Ico "yip" $appFrames (Join-Path $assetDir "yip.ico")
Build-Ico "tray-idle" $trayIdleFrames (Join-Path $assetDir "tray-idle.ico")
Build-Ico "tray-recording" $trayRecFrames (Join-Path $assetDir "tray-recording.ico")
