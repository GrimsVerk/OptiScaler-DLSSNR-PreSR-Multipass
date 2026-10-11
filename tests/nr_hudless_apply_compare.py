"""Compare two HUD-less test recordings of the same static scene: NR applied as an added difference vs a ratio.

Recordings come from the fork's HUD-less test recorder ([DlssNr] HudlessTestControl=true, then
record=<name>, frames=<n>, crops=x,y,w,h;... in nr-test-control.txt). Each recording is <dir>/<name>.bin plus
<dir>/<name>.json. Every frame holds a FrameHeader, then per crop: the HUD-less picture before NR, NR's output, the
adjusted output (what is shown outside the HUD), then the motion vectors under the crop.

Reports, per crop:
  (a) parity: mean / max absolute difference and PSNR between the two modes' frame-averaged adjusted outputs;
  (b) temporal stability per mode: per-pixel temporal standard deviation over the sequence (mean and 99th
      percentile), and "sparkles": pixels whose frame-to-frame change exceeds a threshold. The game's own picture
      before NR is reported alongside as the baseline the NR change sits on;
  (c) NR applied: each mode's adjusted output differs from the picture before NR (the NR-off image of the same
      frame) by a clear margin.
Values are in 8-bit code units (0..255), on the colour channels.

Usage:
  python nr_hudless_apply_compare.py <dir> <add_name> <ratio_name> [--sparkle 6] [--nr-margin 1.0] [--json out.json]
"""
import argparse
import json
import os
import struct
import sys

import numpy as np

HEADER_FORMAT = "<IIQdff6I5I6ffI"  # matches DlssNr::HudlessTest::FrameHeader (packed)
HEADER_FIELDS = ["magic", "index", "gameFrame", "milliseconds", "mvScaleX", "mvScaleY", "outputWidth", "outputHeight",
                 "guideX", "guideY", "guideWidth", "guideHeight", "reset", "historyValid", "temporal", "adaptive",
                 "smoothing", "stillStrength", "motionStrength", "motionStart", "motionFull", "frames", "tolerance",
                 "reject", "bandSplit"]
MAGIC = 0x46524C48  # "HLRF"


def decode_colour(raw, fmt, w, h):
    """RGB in 8-bit code units, float32, shape (h, w, 3)."""
    if fmt in (27, 28, 29, 87, 88, 90, 91):  # 8-bit RGBA / BGRA (channel order does not matter for these metrics)
        a = np.frombuffer(raw, dtype=np.uint8).reshape(h, w, 4)
        return a[..., :3].astype(np.float32)
    if fmt in (23, 24, 25):  # R10G10B10A2
        v = np.frombuffer(raw, dtype=np.uint32).reshape(h, w)
        rgb = np.stack([(v >> s) & 0x3FF for s in (0, 10, 20)], axis=-1).astype(np.float32)
        return rgb * (255.0 / 1023.0)
    raise SystemExit(f"unsupported colour format {fmt}")


def load(directory, name):
    meta = json.load(open(os.path.join(directory, name + ".json")))
    data = open(os.path.join(directory, name + ".bin"), "rb").read()
    hb, fb, cb, mb = meta["header_bytes"], meta["frame_bytes"], meta["colour_bytes"], meta["motion_bytes"]
    if struct.calcsize(HEADER_FORMAT) != hb:
        raise SystemExit(f"{name}: header is {hb} bytes, script expects {struct.calcsize(HEADER_FORMAT)}")
    stride = hb + fb
    count = len(data) // stride
    if count == 0:
        raise SystemExit(f"{name}: no complete frames")
    crops = [c["rect"] for c in meta["crops"]]
    guides = [c["guide"] for c in meta["crops"]]
    before = [[] for _ in crops]
    nr = [[] for _ in crops]
    adjusted = [[] for _ in crops]
    headers = []
    for f in range(count):
        base = f * stride
        header = dict(zip(HEADER_FIELDS, struct.unpack_from(HEADER_FORMAT, data, base)))
        if header["magic"] != MAGIC:
            raise SystemExit(f"{name}: frame {f} has a bad magic number")
        headers.append(header)
        offset = base + hb
        for c, (rect, guide) in enumerate(zip(crops, guides)):
            w, h = rect[2], rect[3]
            n = w * h * cb
            blocks = []
            for _ in range(3):
                blocks.append(decode_colour(data[offset:offset + n], meta["colour_format"], w, h))
                offset += n
            before[c].append(blocks[0])
            nr[c].append(blocks[1])
            adjusted[c].append(blocks[2])
            offset += guide[2] * guide[3] * mb
    stack = lambda lists: [np.stack(x) for x in lists]
    return {"name": name, "meta": meta, "headers": headers, "crops": crops,
            "before": stack(before), "nr": stack(nr), "adjusted": stack(adjusted)}


def psnr(a, b):
    mse = float(np.mean((a - b) ** 2))
    return float("inf") if mse == 0 else 10.0 * np.log10(255.0 ** 2 / mse)


def stability(seq, sparkle):
    std = seq.std(axis=0).mean(axis=-1)  # per pixel, averaged over RGB
    step = np.abs(np.diff(seq, axis=0)).max(axis=-1)  # frame-to-frame change, worst channel
    sparkles = (step > sparkle).sum(axis=(1, 2))  # per transition
    return {"std_mean": float(std.mean()), "std_p99": float(np.percentile(std, 99)),
            "sparkles_per_frame": float(sparkles.mean()), "sparkles_total": int(sparkles.sum())}


def settings(rec):
    h = rec["headers"][0]
    keys = ["temporal", "adaptive", "smoothing", "frames", "tolerance", "reject", "bandSplit", "stillStrength",
            "motionStrength"]
    return {k: h[k] for k in keys} | {"resets": sum(x["reset"] for x in rec["headers"])}


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("directory")
    p.add_argument("add_name")
    p.add_argument("ratio_name")
    p.add_argument("--sparkle", type=float, default=6.0, help="frame-to-frame change (8-bit units) counted as a sparkle")
    p.add_argument("--nr-margin", type=float, default=1.0,
                   help="mean |adjusted - before| (8-bit units) required to count NR as applied")
    p.add_argument("--json", help="also write the results here")
    a = p.parse_args()

    add, ratio = load(a.directory, a.add_name), load(a.directory, a.ratio_name)
    if add["crops"] != ratio["crops"]:
        sys.exit("the two recordings use different crops")
    print(f"{add['name']}: {len(add['headers'])} frames  settings {settings(add)}")
    print(f"{ratio['name']}: {len(ratio['headers'])} frames  settings {settings(ratio)}")
    results = []
    for c, rect in enumerate(add["crops"]):
        r = {"crop": rect}
        A, R = add["adjusted"][c], ratio["adjusted"][c]
        mA, mR = A.mean(axis=0), R.mean(axis=0)
        d = np.abs(mA - mR)
        r["parity"] = {"mean_abs": float(d.mean()), "max_abs": float(d.max()), "psnr_db": psnr(mA, mR)}
        r["stability"] = {"add": stability(A, a.sparkle), "ratio": stability(R, a.sparkle),
                          "before_nr_add_run": stability(add["before"][c], a.sparkle),
                          "before_nr_ratio_run": stability(ratio["before"][c], a.sparkle)}
        applied = {}
        for label, rec in (("add", add), ("ratio", ratio)):
            diff = np.abs(rec["adjusted"][c] - rec["before"][c])
            applied[label] = {"mean_abs_vs_before": float(diff.mean()),
                              "share_over_2": float((diff.max(axis=-1) > 2.0).mean()),
                              "applied": bool(diff.mean() > a.nr_margin)}
        r["nr_applied"] = applied
        results.append(r)

        print(f"\ncrop {rect}")
        pa = r["parity"]
        print(f"  (a) parity add vs ratio (frame means): mean |d| {pa['mean_abs']:.3f}  max |d| {pa['max_abs']:.2f}  "
              f"PSNR {pa['psnr_db']:.2f} dB")
        print("  (b) temporal stability          std mean   std p99   sparkles/frame  sparkles total")
        for label in ("add", "ratio", "before_nr_add_run", "before_nr_ratio_run"):
            s = r["stability"][label]
            print(f"      {label:22s}  {s['std_mean']:8.3f}  {s['std_p99']:8.3f}  {s['sparkles_per_frame']:14.1f}"
                  f"  {s['sparkles_total']:14d}")
        for label in ("add", "ratio"):
            x = applied[label]
            print(f"  (c) NR applied ({label}): mean |adjusted - before| {x['mean_abs_vs_before']:.3f}, "
                  f"{100 * x['share_over_2']:.1f}% of pixels moved > 2  -> {'YES' if x['applied'] else 'NO (check)'}")
    if a.json:
        json.dump({"add": a.add_name, "ratio": a.ratio_name, "results": results}, open(a.json, "w"), indent=2)


if __name__ == "__main__":
    main()
