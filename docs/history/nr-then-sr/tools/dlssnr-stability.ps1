# How stable is the model's edit across consecutive captured frames of a static scene?
#
# For every pixel, over the N captured frames: the temporal standard deviation of the original
# frame's luminance (jitter and the game's own noise), and of the edit (after - before). Reported
# for all pixels, and separately for edge pixels (high spatial gradient in the original) and flat
# ones. A pass that only adds detail once and holds it has edit-std near zero; a pass that
# re-decides the edge every frame has edit-std comparable to or above the original's own.
param(
    # The dlssnr-capture folder beside OptiScaler in the game directory. Originally measured in
    # Jedi Survivor: <game>\SwGame\Binaries\Win64\dlssnr-capture
    [Parameter(Mandatory = $true)][string]$Dir,
    [string]$Label = "",
    [int]$W = 2560, [int]$H = 1440
)
$ErrorActionPreference = "Stop"
Add-Type -TypeDefinition @"
using System;
public static class Stab {
    static float F(uint m, uint e, int mbits) {
        float man = m / (float)(1 << mbits);
        if (e == 0) return man * (float)Math.Pow(2, -14);
        if (e == 31) return 65504f;
        return (1f + man) * (float)Math.Pow(2, (int)e - 15);
    }
    static float Half(ushort h) {
        int s = (h >> 15) & 1, e = (h >> 10) & 0x1F, m = h & 0x3FF;
        float v;
        if (e == 0) v = m * (float)Math.Pow(2, -24);
        else if (e == 31) v = 65504f;
        else v = (1f + m / 1024f) * (float)Math.Pow(2, e - 15);
        return s == 1 ? -v : v;
    }
    // format 26 = R11G11B10_FLOAT (4 bytes/pixel), format 10 = R16G16B16A16_FLOAT (8 bytes/pixel)
    public static void Luma(byte[] raw, int n, float[] y, int format, int rowPitch, int w) {
        for (int i = 0; i < n; i++) {
            int row = i / w, col = i % w;
            if (format == 10) {
                int o = row * rowPitch + col * 8;
                y[i] = 0.2126f * Half(BitConverter.ToUInt16(raw, o)) + 0.7152f * Half(BitConverter.ToUInt16(raw, o + 2)) + 0.0722f * Half(BitConverter.ToUInt16(raw, o + 4));
            } else {
                uint v = BitConverter.ToUInt32(raw, row * rowPitch + col * 4);
                uint r = v & 0x7FF, g = (v >> 11) & 0x7FF, b = (v >> 22) & 0x3FF;
                y[i] = 0.2126f * F(r & 0x3F, r >> 6, 6) + 0.7152f * F(g & 0x3F, g >> 6, 6) + 0.0722f * F(b & 0x1F, b >> 5, 5);
            }
        }
    }
    // frames: [f][pixel]. Returns: {all_origStd, all_editStd, edge_origStd, edge_editStd, flat_origStd, flat_editStd, edgeFraction, meanLuma}
    public static double[] Run(float[][] before, float[][] after, int w, int h) {
        int n = w * h, fN = before.Length;
        double[] acc = new double[8];
        long edgeCount = 0, flatCount = 0; double meanLuma = 0;
        float[] meanB = new float[n];
        for (int i = 0; i < n; i++) { float s = 0; for (int f = 0; f < fN; f++) s += before[f][i]; meanB[i] = s / fN; meanLuma += meanB[i]; }
        meanLuma /= n;
        // gradient threshold: relative to mean luma
        float thr = (float)(meanLuma * 0.5);
        for (int yy = 1; yy < h - 1; yy++) for (int xx = 1; xx < w - 1; xx++) {
            int i = yy * w + xx;
            float gx = Math.Abs(meanB[i + 1] - meanB[i - 1]), gy = Math.Abs(meanB[i + w] - meanB[i - w]);
            bool edge = (gx + gy) > thr;
            double mb = 0, me = 0;
            for (int f = 0; f < fN; f++) { mb += before[f][i]; me += after[f][i] - before[f][i]; }
            mb /= fN; me /= fN;
            double vb = 0, ve = 0;
            for (int f = 0; f < fN; f++) { double db = before[f][i] - mb, de = (after[f][i] - before[f][i]) - me; vb += db * db; ve += de * de; }
            double sb = Math.Sqrt(vb / fN), se = Math.Sqrt(ve / fN);
            acc[0] += sb; acc[1] += se;
            if (edge) { acc[2] += sb; acc[3] += se; edgeCount++; } else { acc[4] += sb; acc[5] += se; flatCount++; }
        }
        long tot = edgeCount + flatCount;
        return new double[] { acc[0]/tot, acc[1]/tot, edgeCount>0?acc[2]/edgeCount:0, edgeCount>0?acc[3]/edgeCount:0, flatCount>0?acc[4]/flatCount:0, flatCount>0?acc[5]/flatCount:0, (double)edgeCount/tot, meanLuma };
    }
}
"@
# Size and format come from the manifest.
$m = Get-Content "$Dir\manifest.txt"
$mb = [regex]::Match(($m | Where-Object { $_ -like 'before *' }), 'width (\d+) height (\d+) format (\d+) rowPitch (\d+)')
$ma = [regex]::Match(($m | Where-Object { $_ -like 'after *' }), 'width (\d+) height (\d+) format (\d+) rowPitch (\d+)')
$W = [int]$mb.Groups[1].Value; $H = [int]$mb.Groups[2].Value
$fmtB = [int]$mb.Groups[3].Value; $pitchB = [int]$mb.Groups[4].Value
$fmtA = [int]$ma.Groups[3].Value; $pitchA = [int]$ma.Groups[4].Value
$n = $W * $H
$frames = (Get-ChildItem "$Dir\before_*.raw").Count
$before = @(); $after = @()
for ($f = 0; $f -lt $frames; $f++) {
    $nn = "{0:00}" -f $f
    $yb = New-Object float[] $n; $ya = New-Object float[] $n
    [Stab]::Luma([IO.File]::ReadAllBytes("$Dir\before_$nn.raw"), $n, $yb, $fmtB, $pitchB, $W)
    [Stab]::Luma([IO.File]::ReadAllBytes("$Dir\after_$nn.raw"), $n, $ya, $fmtA, $pitchA, $W)
    $before += ,$yb; $after += ,$ya
}
$r = [Stab]::Run([float[][]]$before, [float[][]]$after, $W, $H)
"{0}: {1} frames {2}x{3}, mean luma {4:F4}, edge pixels {5:P1}" -f $Label, $frames, $W, $H, $r[7], $r[6]
"  temporal std of ORIGINAL   all {0:F5}  edge {1:F5}  flat {2:F5}" -f $r[0], $r[2], $r[4]
"  temporal std of THE EDIT   all {0:F5}  edge {1:F5}  flat {2:F5}" -f $r[1], $r[3], $r[5]
"  edit/original ratio        all {0:F2}   edge {1:F2}   flat {2:F2}" -f ($r[1]/[Math]::Max($r[0],1e-9)), ($r[3]/[Math]::Max($r[2],1e-9)), ($r[5]/[Math]::Max($r[4],1e-9))
