# Byte-compare before/after raw captures: how many pixels differ, and by how much (in raw R11G11B10 bits).
param(
    # The dlssnr-capture folder beside OptiScaler in the game directory. Originally measured in
    # Jedi Survivor: <game>\SwGame\Binaries\Win64\dlssnr-capture
    [Parameter(Mandatory = $true)][string]$Dir,
    [int[]]$Frames = @(0, 3, 7)
)
Add-Type -TypeDefinition @"
using System;
public static class Cmp {
    public static string Run(byte[] a, byte[] b) {
        if (a.Length != b.Length) return "length differs: " + a.Length + " vs " + b.Length;
        int n = a.Length / 4, diff = 0; long sum = 0; int maxd = 0;
        for (int i = 0; i < n; i++) {
            uint x = BitConverter.ToUInt32(a, i*4), y = BitConverter.ToUInt32(b, i*4);
            if (x != y) {
                diff++;
                int d = Math.Abs((int)(x & 0x7FF) - (int)(y & 0x7FF)) + Math.Abs((int)((x>>11)&0x7FF) - (int)((y>>11)&0x7FF)) + Math.Abs((int)(x>>22) - (int)(y>>22));
                sum += d; if (d > maxd) maxd = d;
            }
        }
        return string.Format("pixels {0}  differing {1} ({2:P3})  mean raw-bit diff {3:F2}  max {4}", n, diff, (double)diff/n, diff>0 ? (double)sum/diff : 0, maxd);
    }
}
"@
foreach ($f in $Frames) {
    $nn = "{0:00}" -f $f
    $a = [IO.File]::ReadAllBytes("$Dir\before_$nn.raw"); $b = [IO.File]::ReadAllBytes("$Dir\after_$nn.raw")
    "frame $nn : " + [Cmp]::Run($a, $b)
}
