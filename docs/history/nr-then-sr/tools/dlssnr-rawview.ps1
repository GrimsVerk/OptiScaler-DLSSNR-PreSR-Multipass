# Convert R11G11B10_FLOAT raw captures (before/after pair) to PNGs with a shared exposure, plus a
# 2x centre crop of each and an amplified difference image.
param(
    # The dlssnr-capture folder beside OptiScaler in the game directory. Originally measured in
    # Jedi Survivor: <game>\SwGame\Binaries\Win64\dlssnr-capture
    [Parameter(Mandatory = $true)][string]$Dir,
    [int]$Frame = 3,
    [string]$Out = ".\dlssnr-view",
    [int]$W = 1920, [int]$H = 1080,
    [int]$CropX = 660, [int]$CropY = 340, [int]$CropW = 600, [int]$CropH = 400
)
$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force $Out | Out-Null
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @"
using System;
public static class R11 {
    static float F(uint m, uint e, int mbits) {
        float man = m / (float)(1 << mbits);
        if (e == 0) return man * (float)Math.Pow(2, -14);
        if (e == 31) return 65504f;
        return (1f + man) * (float)Math.Pow(2, (int)e - 15);
    }
    public static void Decode(byte[] raw, int w, int h, int pitch, float[] rgb) {
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
            uint v = BitConverter.ToUInt32(raw, y * pitch + x * 4);
            uint r = v & 0x7FF, g = (v >> 11) & 0x7FF, b = (v >> 22) & 0x3FF;
            int i = (y * w + x) * 3;
            rgb[i]   = F(r & 0x3F, r >> 6, 6);
            rgb[i+1] = F(g & 0x3F, g >> 6, 6);
            rgb[i+2] = F(b & 0x1F, b >> 5, 5);
        }
    }
    public static float MeanLuma(float[] rgb) {
        double s = 0; int n = rgb.Length / 3;
        for (int i = 0; i < n; i++) s += Math.Log(1e-6 + 0.2126*rgb[i*3] + 0.7152*rgb[i*3+1] + 0.0722*rgb[i*3+2]);
        return (float)Math.Exp(s / n);
    }
    static byte Enc(float v) {
        v = v / (1f + v);
        v = v <= 0.0031308f ? v * 12.92f : 1.055f * (float)Math.Pow(v, 1/2.4) - 0.055f;
        return (byte)Math.Max(0, Math.Min(255, (int)(v * 255f + 0.5f)));
    }
    // Already display-referred (the encoded proxy): clamp only.
    static byte Raw(float v) { return (byte)Math.Max(0, Math.Min(255, (int)(v * 255f + 0.5f))); }
    // Linear HDR shown the way the pass's encode would show it: sRGB, no Reinhard.
    static byte Srgb(float v) {
        v = Math.Max(0f, Math.Min(1f, v));
        v = v <= 0.0031308f ? v * 12.92f : 1.055f * (float)Math.Pow(v, 1/2.4) - 0.055f;
        return (byte)Math.Max(0, Math.Min(255, (int)(v * 255f + 0.5f)));
    }
    public static byte[] ToBgra(float[] rgb, float scale, int n, int mode) {
        byte[] o = new byte[n * 4];
        for (int i = 0; i < n; i++) {
            float r = rgb[i*3] * scale, g = rgb[i*3+1] * scale, b = rgb[i*3+2] * scale;
            if (mode == 1) { o[i*4+2] = Raw(r); o[i*4+1] = Raw(g); o[i*4] = Raw(b); }
            else if (mode == 2) { o[i*4+2] = Srgb(r); o[i*4+1] = Srgb(g); o[i*4] = Srgb(b); }
            else { o[i*4+2] = Enc(r); o[i*4+1] = Enc(g); o[i*4] = Enc(b); }
            o[i*4+3] = 255;
        }
        return o;
    }
    public static byte[] Diff(float[] a, float[] b, float scale, float gain, int n) {
        byte[] o = new byte[n * 4];
        for (int i = 0; i < n; i++) {
            float d = Math.Abs(a[i*3]-b[i*3]) + Math.Abs(a[i*3+1]-b[i*3+1]) + Math.Abs(a[i*3+2]-b[i*3+2]);
            byte v = Enc(d * scale * gain / 3f);
            o[i*4] = v; o[i*4+1] = v; o[i*4+2] = v; o[i*4+3] = 255;
        }
        return o;
    }
}
"@
function Save-Png([byte[]]$bgra, [int]$w, [int]$h, [string]$path, [int]$cx, [int]$cy, [int]$cw, [int]$ch, [int]$zoom) {
    $bmp = New-Object System.Drawing.Bitmap $w, $h, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $rect = New-Object System.Drawing.Rectangle 0, 0, $w, $h
    $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::WriteOnly, $bmp.PixelFormat)
    [System.Runtime.InteropServices.Marshal]::Copy($bgra, 0, $data.Scan0, $bgra.Length)
    $bmp.UnlockBits($data)
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    if ($cw -gt 0) {
        $crop = New-Object System.Drawing.Bitmap ($cw * $zoom), ($ch * $zoom)
        $g = [System.Drawing.Graphics]::FromImage($crop)
        $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
        $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
        $src = New-Object System.Drawing.Rectangle $cx, $cy, $cw, $ch
        $dst = New-Object System.Drawing.Rectangle 0, 0, ($cw * $zoom), ($ch * $zoom)
        $g.DrawImage($bmp, $dst, $src, [System.Drawing.GraphicsUnit]::Pixel)
        $g.Dispose()
        $crop.Save(($path -replace '\.png$', "_crop.png"), [System.Drawing.Imaging.ImageFormat]::Png)
        $crop.Dispose()
    }
    $bmp.Dispose()
}
$n = $W * $H
$nn = "{0:00}" -f $Frame
$before = New-Object float[] ($n * 3); $after = New-Object float[] ($n * 3)
[R11]::Decode([IO.File]::ReadAllBytes("$Dir\before_$nn.raw"), $W, $H, $W * 4, $before)
[R11]::Decode([IO.File]::ReadAllBytes("$Dir\after_$nn.raw"), $W, $H, $W * 4, $after)
"frame $Frame  mean before(linear)=$([R11]::MeanLuma($before))  mean after(linear)=$([R11]::MeanLuma($after))"
# Since the capture fix both halves are linear HDR (the untouched frame and the edited one). Shown sRGB, no tonemap.
Save-Png ([R11]::ToBgra($before, 1.0, $n, 2)) $W $H "$Out\before_$nn.png" $CropX $CropY $CropW $CropH 2
Save-Png ([R11]::ToBgra($after, 1.0, $n, 2)) $W $H "$Out\after_$nn.png" $CropX $CropY $CropW $CropH 2
Save-Png ([R11]::Diff($before, $after, 1.0, 8, $n)) $W $H "$Out\diff_$nn.png" $CropX $CropY $CropW $CropH 2
Get-ChildItem $Out | Select-Object Name, Length
