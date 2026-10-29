# Generate embedded bitmap font header: CJK12 (Fusion Pixel 12px monospaced) + NUM36/NUM28/NUM20 (JetBrains Mono Bold)
# Scans simulation/*.html + every .ino under firmware/Countdown to collect CJK chars.
# NOTE: kept English-only on purpose (Windows PowerShell 5.1 reads BOM-less .ps1 as ANSI).

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

# Paths are resolved relative to this script -> works on any clone location
$root    = Split-Path -Parent $PSScriptRoot
$outPath = Join-Path $root "firmware/Countdown/font_cn12.h"

# Character sources: simulation HTML + all .ino under firmware/Countdown
$srcFiles = @()
$srcFiles += Get-ChildItem -Path (Join-Path $root "simulation") -Filter *.html -File | ForEach-Object { $_.FullName }
$srcFiles += Get-ChildItem -Path (Join-Path $root "firmware/Countdown") -Filter *.ino -File | ForEach-Object { $_.FullName }
$cjkSet = New-Object System.Collections.Generic.HashSet[System.Char]
foreach ($f in $srcFiles) {
    if (-not (Test-Path $f)) { continue }
    $s = [System.IO.File]::ReadAllText($f, [System.Text.Encoding]::UTF8)
    foreach ($m in [regex]::Matches($s, '[\u4e00-\u9fff]')) { [void]$cjkSet.Add($m.Value) }
}
$cjk = @($cjkSet) | Sort-Object
$extraCodes = @(0x00B0, 0x00B7, 0x00B1, 0x25B2, 0x2039, 0x300C, 0x300D,
                0x5B8C, 0x6210, 0x5237, 0x793A, 0x610F, 0x914D, 0x7F51,
                0x7EED, 0x7248, 0x672C, 0xFF1A, 0xFF08, 0xFF09)
$chars = New-Object System.Collections.Generic.List[System.Char]
for ($c = 32; $c -le 126; $c++) { $chars.Add([char]$c) }
foreach ($e in $extraCodes) { if (-not $chars.Contains([char]$e)) { $chars.Add([char]$e) } }
foreach ($c in $cjk) { if (-not $chars.Contains([char]$c)) { $chars.Add([char]$c) } }
Write-Output ("charset size: " + $chars.Count + " (cjk " + $cjk.Count + ")")

# ---- 2. glyph renderer ----
function Render-Glyph {
    param(
        [char]$ch, [System.Drawing.Font]$font, [int]$cw, [int]$chh,
        [bool]$crop
    )
    $bmp = New-Object System.Drawing.Bitmap($cw, $chh)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.Clear([System.Drawing.Color]::White)
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::SingleBitPerPixel
    $sf = [System.Drawing.StringFormat]::GenericTypographic
    $br = [System.Drawing.Brushes]::Black
    $g.DrawString([string]$ch, $font, $br, 0, 0, $sf)
    $g.Dispose()

    $minX = 9999; $maxX = -1; $minY = 9999; $maxY = -1
    $grid = New-Object 'bool[,]' $cw, $chh
    for ($y = 0; $y -lt $chh; $y++) {
        for ($x = 0; $x -lt $cw; $x++) {
            $p = $bmp.GetPixel($x, $y)
            $dark = ($p.R -lt 128)
            $grid[$x, $y] = $dark
            if ($dark) {
                if ($x -lt $minX) { $minX = $x }
                if ($x -gt $maxX) { $maxX = $x }
                if ($y -lt $minY) { $minY = $y }
                if ($y -gt $maxY) { $maxY = $y }
            }
        }
    }
    $bmp.Dispose()

    if ($maxX -lt 0) {
        return @{ w = $cw; h = 0; yoff = 0; bits = @(); empty = $true }
    }
    if (-not $crop) {
        $x0 = 0; $x1 = $cw - 1; $y0 = 0; $y1 = $chh - 1
        $w = $cw; $h = $chh; $yoff = 0
    } else {
        $x0 = $minX; $x1 = $maxX; $y0 = $minY; $y1 = $maxY
        $w = $x1 - $x0 + 1; $h = $y1 - $y0 + 1; $yoff = $y0
    }
    $rowsArr = @()
    for ($y = $y0; $y -le $y1; $y++) {
        $v = 0
        for ($x = $x0; $x -le $x1; $x++) {
            if ($grid[$x, $y]) { $v = $v -bor (1 -shl (($x - $x0))) }
        }
        $rowsArr += $v
    }
    return @{ w = $w; h = $h; yoff = $yoff; bits = $rowsArr; empty = $false }
}

function Pack-Glyphs {
    param($glyphs)
    # MSB-first packing to match TFT_eSPI drawBitmap: bit7 of each byte = leftmost pixel
    # IMPORTANT: empty glyphs (e.g. space) MUST reserve their own zero bytes,
    # otherwise their offset aliases onto the NEXT glyph's data (bug: space rendered
    # as the next char, which was '!' -> every space showed as an exclamation mark).
    $blob = New-Object System.Collections.Generic.List[int]
    $offs = New-Object System.Collections.Generic.List[int]
    foreach ($g in $glyphs) {
        $offs.Add($blob.Count)
        $stride = [math]::Ceiling($g.w / 8.0)
        if ($g.empty) {
            for ($i = 0; $i -lt ($stride * 12); $i++) { $blob.Add(0) }
            continue
        }
        foreach ($row in $g.bits) {
            for ($bi = 0; $bi -lt $stride; $bi++) {
                $byteVal = 0
                for ($k = 0; $k -lt 8; $k++) {
                    $px = $bi * 8 + $k
                    if ($px -lt $g.w -and ((($row -shr $px) -band 1) -eq 1)) {
                        $byteVal = $byteVal -bor (0x80 -shr $k)
                    }
                }
                $blob.Add($byteVal)
            }
        }
    }
    return @{ blob = $blob; offs = $offs }
}

# ---- 2.5 bundled OFL fonts (no system install needed) ----
$fontDir = Join-Path $PSScriptRoot "fonts"
$cjkPath = Join-Path $fontDir "fusion-pixel-12px-monospaced-zh_hans.ttf"
$numPath = Join-Path $fontDir "JetBrainsMono-Bold.ttf"
foreach ($p in @($cjkPath, $numPath)) {
    if (-not (Test-Path $p)) {
        throw "Missing font: $p -- see tools/fonts/README.md"
    }
}
$pfcCjk = New-Object System.Drawing.Text.PrivateFontCollection
$pfcCjk.AddFontFile($cjkPath)
$famCjk = $pfcCjk.Families[0]
$pfcNum = New-Object System.Drawing.Text.PrivateFontCollection
$pfcNum.AddFontFile($numPath)
$famNum = $pfcNum.Families[0]
Write-Output ("CJK font: " + $famCjk.Name + " (12px monospaced)")
Write-Output ("NUM font: " + $famNum.Name + " (bold)")

# ---- 3. CJK12 ----
$font12 = New-Object System.Drawing.Font($famCjk, 12, [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
$g12 = @()
$w12 = @()
foreach ($ch in $chars) {
    if ([int]$ch -lt 128) { $cw = 6 } else { $cw = 12 }
    $g = Render-Glyph -ch $ch -font $font12 -cw $cw -chh 12 -crop $false
    $g12 += $g
    $w12 += $g.w
}
$p12 = Pack-Glyphs $g12
$font12.Dispose()

# ---- 4. NUM fonts ----
function Build-NumFont {
    param([int]$size)
    $font = New-Object System.Drawing.Font($famNum, $size, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
    $set = "0123456789:-."
    $glyphs = @(); $widths = @(); $advs = @()
    $bmpM = New-Object System.Drawing.Bitmap(100, 100)
    $gm = [System.Drawing.Graphics]::FromImage($bmpM)
    $gm.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::SingleBitPerPixel
    $sf = [System.Drawing.StringFormat]::GenericTypographic
    foreach ($ch in $set.ToCharArray()) {
        $adv = [int][math]::Round($gm.MeasureString([string]$ch, $font, 1000, $sf).Width)
        if ($adv -lt 1) { $adv = 1 }
        $g = Render-Glyph -ch $ch -font $font -cw ($size + 16) -chh ($size + 16) -crop $true
        $glyphs += $g; $widths += $g.w; $advs += $adv
    }
    $gm.Dispose(); $bmpM.Dispose(); $font.Dispose()
    $packed = Pack-Glyphs $glyphs
    return @{ set = $set; glyphs = $glyphs; widths = $widths; advs = $advs; blob = $packed.blob; offs = $packed.offs }
}
$n36 = Build-NumFont -size 36
$n28 = Build-NumFont -size 28
$n20 = Build-NumFont -size 20
$pfcCjk.Dispose()
$pfcNum.Dispose()

# ---- 5. samples ----
function Show-Glyph {
    param($g, $label)
    Write-Output ("--- " + $label + "  w=" + $g.w + " h=" + $g.h + " ---")
    if ($g.empty) { Write-Output "(empty)"; return }
    foreach ($row in $g.bits) {
        $s = ""
        for ($x = 0; $x -lt $g.w; $x++) {
            if ((($row -shr $x) -band 1) -eq 1) { $s += "#" } else { $s += "." }
        }
        Write-Output $s
    }
}
Show-Glyph $g12[$chars.IndexOf([char]0x8DDD)] "CJK12 0x8DDD"
Show-Glyph $g12[$chars.IndexOf([char]0x6625)] "CJK12 0x6625"
Show-Glyph $g12[$chars.IndexOf([char]0x41)]   "CJK12 'A'"
Show-Glyph $n36.glyphs[0] "NUM36 '0'"
Show-Glyph $n20.glyphs[0] "NUM20 '0'"

# ---- 6. emit header ----
$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("// ============================================================")
[void]$sb.AppendLine("// font_cn12.h -- auto-generated, do not edit by hand")
[void]$sb.AppendLine("// CJK12: Fusion Pixel 12px monospaced (OFL-1.1), ASCII + CJK subset from sources")
[void]$sb.AppendLine("// NUM36/NUM28/NUM20: JetBrains Mono Bold (OFL-1.1), digits + colon + dot + minus")
[void]$sb.AppendLine("// ============================================================")
[void]$sb.AppendLine("#pragma once")
[void]$sb.AppendLine("#include <Arduino.h>")
[void]$sb.AppendLine("")

function Emit-U16Array($name, $vals) {
    [void]$sb.AppendLine("const uint16_t $name[] PROGMEM = {")
    $line = "  "
    foreach ($v in $vals) {
        $line += ("0x{0:X4}," -f $v)
        if ($line.Length -gt 100) { [void]$sb.AppendLine($line); $line = "  " }
    }
    if ($line.Trim().Length -gt 0) { [void]$sb.AppendLine($line) }
    [void]$sb.AppendLine("};")
}
function Emit-U8Array($name, $vals) {
    [void]$sb.AppendLine("const uint8_t $name[] PROGMEM = {")
    $line = "  "
    foreach ($v in $vals) {
        $line += ("0x{0:X2}," -f $v)
        if ($line.Length -gt 100) { [void]$sb.AppendLine($line); $line = "  " }
    }
    if ($line.Trim().Length -gt 0) { [void]$sb.AppendLine($line) }
    [void]$sb.AppendLine("};")
}

[void]$sb.AppendLine("// ---------- CJK12 ----------")
Emit-U16Array "font12_uni" @($chars | ForEach-Object { [int]$_ })
Emit-U8Array  "font12_w"   $w12
Emit-U16Array "font12_off" @($p12.offs)
Emit-U8Array  "font12_blob" @($p12.blob)
[void]$sb.AppendLine(("const uint16_t font12_count = " + $chars.Count + ";"))
[void]$sb.AppendLine("")
[void]$sb.AppendLine("// ---------- NUM36 ----------")
Emit-U16Array "num36_uni" @($n36.set.ToCharArray() | ForEach-Object { [int]$_ })
Emit-U8Array  "num36_w"   $n36.widths
Emit-U8Array  "num36_h"   @($n36.glyphs | ForEach-Object { $_.h })
Emit-U8Array  "num36_adv" $n36.advs
Emit-U16Array "num36_off" @($n36.offs)
Emit-U8Array  "num36_blob" @($n36.blob)
[void]$sb.AppendLine("")
[void]$sb.AppendLine("// ---------- NUM28 ----------")
Emit-U16Array "num28_uni" @($n28.set.ToCharArray() | ForEach-Object { [int]$_ })
Emit-U8Array  "num28_w"   $n28.widths
Emit-U8Array  "num28_h"   @($n28.glyphs | ForEach-Object { $_.h })
Emit-U8Array  "num28_adv" $n28.advs
Emit-U16Array "num28_off" @($n28.offs)
Emit-U8Array  "num28_blob" @($n28.blob)
[void]$sb.AppendLine("")
[void]$sb.AppendLine("// ---------- NUM20 ----------")
Emit-U16Array "num20_uni" @($n20.set.ToCharArray() | ForEach-Object { [int]$_ })
Emit-U8Array  "num20_w"   $n20.widths
Emit-U8Array  "num20_h"   @($n20.glyphs | ForEach-Object { $_.h })
Emit-U8Array  "num20_adv" $n20.advs
Emit-U16Array "num20_off" @($n20.offs)
Emit-U8Array  "num20_blob" @($n20.blob)

[System.IO.File]::WriteAllText($outPath, $sb.ToString(), (New-Object System.Text.UTF8Encoding($false)))
Write-Output ("written: " + $outPath)
Write-Output ("CJK12 glyphs=" + $chars.Count + " blob=" + $p12.blob.Count + "B")
Write-Output ("NUM36 blob=" + $n36.blob.Count + "B  NUM20 blob=" + $n20.blob.Count + "B")
