# gen_bigfont.ps1 - gera main/app/font_big88.c (fonte LVGL so com "-0123456789AFHINST")
# a partir da Montserrat que vem com o LVGL. Usa o GDI+ do Windows, sem instalar nada.
# Uso:  powershell -ExecutionPolicy Bypass -File tools\gen_bigfont.ps1 [-Size 88]
param([int]$Size = 88)
Add-Type -AssemblyName System.Drawing
$root = Split-Path $PSScriptRoot -Parent
$ttf = Join-Path $root "managed_components\lvgl__lvgl\scripts\built_in_font\Montserrat-Medium.ttf"
$out = Join-Path $root "main\app\font_big$Size.c"
$chars = "-0123456789AFHINST"   # em ordem crescente de codigo

$pfc = New-Object System.Drawing.Text.PrivateFontCollection
$pfc.AddFontFile($ttf)
$fam = $pfc.Families[0]
$style = [System.Drawing.FontStyle]::Regular
$font = New-Object System.Drawing.Font($fam, $Size, $style, [System.Drawing.GraphicsUnit]::Pixel)
$ascent = $Size * $fam.GetCellAscent($style) / $fam.GetEmHeight($style)
$fmt = [System.Drawing.StringFormat]::GenericTypographic.Clone()
$fmt.FormatFlags = $fmt.FormatFlags -bor [System.Drawing.StringFormatFlags]::MeasureTrailingSpaces

$pad = 20
$W = $Size * 2; $H = $Size * 2
$glyphs = @()
foreach ($ch in $chars.ToCharArray()) {
    $bmp = New-Object System.Drawing.Bitmap($W, $H)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.Clear([System.Drawing.Color]::Black)
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAlias
    $g.DrawString([string]$ch, $font, [System.Drawing.Brushes]::White, [single]$pad, [single]$pad, $fmt)
    $adv = $g.MeasureString([string]$ch, $font, 10000, $fmt).Width
    $x0 = $W; $y0 = $H; $x1 = -1; $y1 = -1
    $px = New-Object 'int[,]' $H, $W
    for ($y = 0; $y -lt $H; $y++) { for ($x = 0; $x -lt $W; $x++) {
        $v = [int][math]::Floor(($bmp.GetPixel($x, $y).R + 8) / 17)
        $px[$y, $x] = $v
        if ($v -gt 0) { if ($x -lt $x0) { $x0 = $x }; if ($x -gt $x1) { $x1 = $x }; if ($y -lt $y0) { $y0 = $y }; if ($y -gt $y1) { $y1 = $y } }
    } }
    $g.Dispose(); $bmp.Dispose()
    $bw = $x1 - $x0 + 1; $bh = $y1 - $y0 + 1
    $nib = New-Object System.Collections.Generic.List[int]
    for ($y = $y0; $y -le $y1; $y++) { for ($x = $x0; $x -le $x1; $x++) { $pv = $px[$y, $x]; $nib.Add($pv) } }
    if ($nib.Count % 2) { $nib.Add(0) }
    $bytes = New-Object System.Collections.Generic.List[int]
    for ($i = 0; $i -lt $nib.Count; $i += 2) { $bytes.Add($nib[$i] * 16 + $nib[$i + 1]) }
    $glyphs += [pscustomobject]@{ ch = $ch; adv = $adv; w = $bw; h = $bh; ox = $x0 - $pad
        oy = [int][math]::Round($pad + $ascent) - ($y1 + 1); bytes = $bytes }
}

# algarismos com a mesma largura, para o numero nao "dancar"
$maxadv = ($glyphs | Where-Object { $_.ch -match '\d' } | Measure-Object adv -Maximum).Maximum
foreach ($gl in $glyphs) { if ($gl.ch -match '\d') {
    $gl.ox += [int][math]::Round(($maxadv - $gl.adv) / 2); $gl.adv = $maxadv } }

$top = ($glyphs | ForEach-Object { $_.oy + $_.h } | Measure-Object -Maximum).Maximum
$bot = ($glyphs | ForEach-Object { $_.oy } | Measure-Object -Minimum).Minimum
$base = [math]::Max(0, -$bot) + 1
$lineh = $top + $base + 1

$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("// font_big$Size.c - GERADO por tools/gen_bigfont.ps1 (Montserrat Medium $Size px, 4 bpp)")
[void]$sb.AppendLine("// Caracteres: $chars")
[void]$sb.AppendLine('#include "lvgl.h"')
[void]$sb.AppendLine("")
[void]$sb.AppendLine("static const uint8_t glyph_bitmap[] = {")
$idx = 0; $dsc = @()
foreach ($gl in $glyphs) {
    [void]$sb.AppendLine("    /* '$($gl.ch)' */")
    for ($i = 0; $i -lt $gl.bytes.Count; $i += 20) {
        $n = [math]::Min(20, $gl.bytes.Count - $i)
        $line = ($gl.bytes.GetRange($i, $n) | ForEach-Object { "0x{0:x2}" -f $_ }) -join ", "
        [void]$sb.AppendLine("    $line,")
    }
    $dsc += "    {.bitmap_index = $idx, .adv_w = $([int][math]::Round($gl.adv * 16)), .box_w = $($gl.w), .box_h = $($gl.h), .ofs_x = $($gl.ox), .ofs_y = $($gl.oy)},   /* '$($gl.ch)' */"
    $idx += $gl.bytes.Count
}
[void]$sb.AppendLine("};")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {")
[void]$sb.AppendLine("    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0},   /* id 0 reservado */")
foreach ($d in $dsc) { [void]$sb.AppendLine($d) }
[void]$sb.AppendLine("};")
[void]$sb.AppendLine("")
$first = [int]$chars[0]
$ulist = ($chars.ToCharArray() | ForEach-Object { "0x{0:x}" -f ([int]$_ - $first) }) -join ", "
$rlen = [int]$chars[$chars.Length - 1] - $first + 1
[void]$sb.AppendLine("static const uint16_t unicode_list_0[] = { $ulist };")
[void]$sb.AppendLine("")
[void]$sb.AppendLine(@"
static const lv_font_fmt_txt_cmap_t cmaps[] = {
    {
        .range_start = $first, .range_length = $rlen, .glyph_id_start = 1,
        .unicode_list = unicode_list_0, .glyph_id_ofs_list = NULL, .list_length = $($chars.Length),
        .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY
    }
};

static const lv_font_fmt_txt_dsc_t font_dsc = {
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 1,
    .bpp = 4,
    .kern_classes = 0,
    .bitmap_format = 0,
};

const lv_font_t font_big$Size = {
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,
    .line_height = $lineh,
    .base_line = $base,
    .subpx = LV_FONT_SUBPX_NONE,
    .underline_position = -4,
    .underline_thickness = 2,
    .dsc = &font_dsc,
};
"@)
[IO.File]::WriteAllText($out, $sb.ToString())
"gerado $out  ($idx bytes, line_height $lineh, base_line $base)"
$glyphs | Format-Table ch, adv, w, h, ox, oy -AutoSize
