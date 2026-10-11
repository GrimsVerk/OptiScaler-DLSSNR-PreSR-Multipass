"""Add-vs-ratio comparison of HUD-less NR test recordings, read frame by frame (well under 1 GB of RAM).

Recordings come from the fork's HUD-less test recorder ([DlssNr] HudlessTestControl=true, then record=<name>,
frames=<n>, crops=x,y,w,h;... in nr-test-control.txt). Each is <dir>/<name>.bin + <dir>/<name>.json; a frame is a
FrameHeader, then per crop three colour blocks and the motion vectors under the crop. The JSON's "blocks" names them:
  dual recording (recordBoth=1): [before, add, ratio] - both apply modes computed from the SAME frame;
  ordinary recording:            [before, nr, adjusted] - "adjusted" is the shown result of the selected mode.

Modes:
  dual <dir> <name>                one dual recording: exact same-frame parity
  pair <dir> <add_name> <ratio_name>  two ordinary recordings (scene may differ between them: parity is indicative)

Per crop it reports parity (mean / max |add - ratio| and PSNR; same-frame in dual mode, of the frame means in pair
mode), temporal stability per mode (per-pixel temporal std over the sequence: mean and 99th percentile; sparkles =
pixels whose frame-to-frame change exceeds --sparkle) with the picture before NR as the baseline, and whether NR was
applied (each mode's result vs the picture before NR of the same frame). Values are 8-bit code units on RGB.
--skip drops the first frames (smoothing warm-up after recordBoth was switched on).
"""
import argparse
import json
import os
import struct
import sys

import numpy as np

HEADER_FORMAT = "<IIQdff6I5I6ffI"  # DlssNr::HudlessTest::FrameHeader (packed)
HEADER_FIELDS = ["magic", "index", "gameFrame", "milliseconds", "mvScaleX", "mvScaleY", "outputWidth", "outputHeight",
                 "guideX", "guideY", "guideWidth", "guideHeight", "reset", "historyValid", "temporal", "adaptive",
                 "smoothing", "stillStrength", "motionStrength", "motionStart", "motionFull", "frames", "tolerance",
                 "reject", "bandSplit"]
MAGIC = 0x46524C48  # "HLRF"


class Recording:
    """Recording read one frame at a time (plain file reads, so memory stays at one frame); crops as float32 RGB."""

    def __init__(self, directory, name):
        self.name = name
        self.meta = json.load(open(os.path.join(directory, name + ".json")))
        self.blocks = self.meta.get("blocks", ["before", "nr", "adjusted"])
        path = os.path.join(directory, name + ".bin")
        self.file = open(path, "rb")
        self.size = os.path.getsize(path)
        m = self.meta
        self.hb, self.cb, self.mb = m["header_bytes"], m["colour_bytes"], m["motion_bytes"]
        if struct.calcsize(HEADER_FORMAT) != self.hb:
            sys.exit(f"{name}: header is {self.hb} bytes, script expects {struct.calcsize(HEADER_FORMAT)}")
        self.stride = self.hb + m["frame_bytes"]
        self.count = self.size // self.stride
        self.crops = [c["rect"] for c in m["crops"]]
        self.guides = [c["guide"] for c in m["crops"]]
        self.fmt = m["colour_format"]
        if self.count == 0:
            sys.exit(f"{name}: no complete frames")

    def decode(self, raw, w, h):
        if self.fmt in (27, 28, 29, 87, 88, 90, 91):
            return np.asarray(raw).reshape(h, w, 4)[..., :3].astype(np.float32)
        if self.fmt in (23, 24, 25):
            v = np.asarray(raw).view(np.uint32).reshape(h, w)
            return np.stack([(v >> s) & 0x3FF for s in (0, 10, 20)], axis=-1).astype(np.float32) * (255.0 / 1023.0)
        sys.exit(f"unsupported colour format {self.fmt}")

    def frame(self, f):
        self.file.seek(f * self.stride)
        frame = np.frombuffer(self.file.read(self.stride), dtype=np.uint8)
        header = dict(zip(HEADER_FIELDS, struct.unpack_from(HEADER_FORMAT, frame[:self.hb].tobytes())))
        if header["magic"] != MAGIC:
            sys.exit(f"{self.name}: frame {f} has a bad magic number")
        offset = self.hb
        crops = []
        for rect, guide in zip(self.crops, self.guides):
            w, h = rect[2], rect[3]
            n = w * h * self.cb
            blocks = {}
            for label in self.blocks:
                blocks[label] = self.decode(frame[offset:offset + n], w, h)
                offset += n
            offset += guide[2] * guide[3] * self.mb
            crops.append(blocks)
        return header, crops


class Temporal:
    """Streaming per-pixel temporal statistics (Welford) and frame-to-frame sparkles for one image stream."""

    def __init__(self, shape, sparkle):
        self.n = 0
        self.mean = np.zeros(shape, np.float64)
        self.m2 = np.zeros(shape, np.float64)
        self.previous = None
        self.sparkle = sparkle
        self.sparkles = 0
        self.transitions = 0

    def add(self, x):
        self.n += 1
        delta = x - self.mean
        self.mean += delta / self.n
        self.m2 += delta * (x - self.mean)
        if self.previous is not None:
            self.sparkles += int((np.abs(x - self.previous).max(axis=-1) > self.sparkle).sum())
            self.transitions += 1
        self.previous = x

    def report(self):
        std = np.sqrt(self.m2 / max(self.n, 1)).mean(axis=-1)
        return {"std_mean": float(std.mean()), "std_p99": float(np.percentile(std, 99)),
                "sparkles_per_frame": self.sparkles / max(self.transitions, 1), "sparkles_total": self.sparkles}


class Applied:
    def __init__(self):
        self.total = 0.0
        self.moved = 0.0
        self.n = 0

    def add(self, result, before):
        d = np.abs(result - before)
        self.total += float(d.mean())
        self.moved += float((d.max(axis=-1) > 2.0).mean())
        self.n += 1

    def report(self, margin):
        mean = self.total / max(self.n, 1)
        return {"mean_abs_vs_before": mean, "share_over_2": self.moved / max(self.n, 1), "applied": mean > margin}


def psnr_from(sq, count):
    mse = sq / max(count, 1)
    return float("inf") if mse == 0 else 10.0 * np.log10(255.0 ** 2 / mse)


def print_crop(rect, r):
    print(f"\ncrop {rect}")
    pa = r["parity"]
    print(f"  parity add vs ratio ({pa['kind']}): mean |d| {pa['mean_abs']:.3f}  max |d| {pa['max_abs']:.2f}  "
          f"PSNR {pa['psnr_db']:.2f} dB")
    print("  temporal stability        std mean   std p99   sparkles/frame  sparkles total")
    for label, s in r["stability"].items():
        print(f"    {label:20s}  {s['std_mean']:8.3f}  {s['std_p99']:8.3f}  {s['sparkles_per_frame']:14.1f}"
              f"  {s['sparkles_total']:14d}")
    for label, x in r["nr_applied"].items():
        print(f"  NR applied ({label}): mean |result - before| {x['mean_abs_vs_before']:.3f}, "
              f"{100 * x['share_over_2']:.1f}% of pixels moved > 2 -> {'YES' if x['applied'] else 'NO (check)'}")


def run_dual(a):
    rec = Recording(a.directory, a.name)
    if rec.blocks != ["before", "add", "ratio"]:
        sys.exit(f"{a.name} is not a dual recording (blocks {rec.blocks}); record it with recordBoth=1")
    frames = range(a.skip, rec.count)
    if len(frames) < 2:
        sys.exit("not enough frames after --skip")
    stats = []
    for rect in rec.crops:
        shape = (rect[3], rect[2], 3)
        stats.append({"temporal": {k: Temporal(shape, a.sparkle) for k in ("add", "ratio", "before")},
                      "applied": {k: Applied() for k in ("add", "ratio")},
                      "abs_sum": 0.0, "sq_sum": 0.0, "max": 0.0, "values": 0})
    resets = 0
    for f in frames:
        header, crops = rec.frame(f)
        resets += header["reset"]
        for st, blocks in zip(stats, crops):
            d = blocks["add"] - blocks["ratio"]
            st["abs_sum"] += float(np.abs(d).sum())
            st["sq_sum"] += float((d.astype(np.float64) ** 2).sum())
            st["max"] = max(st["max"], float(np.abs(d).max()))
            st["values"] += d.size
            for k in ("add", "ratio", "before"):
                st["temporal"][k].add(blocks[k])
            for k in ("add", "ratio"):
                st["applied"][k].add(blocks[k], blocks["before"])
    print(f"{rec.name}: dual recording, frames {a.skip}..{rec.count - 1} ({len(frames)}), history resets {resets}")
    results = []
    for rect, st in zip(rec.crops, stats):
        r = {"crop": rect,
             "parity": {"kind": "same frame", "mean_abs": st["abs_sum"] / st["values"], "max_abs": st["max"],
                        "psnr_db": psnr_from(st["sq_sum"], st["values"])},
             "stability": {k: st["temporal"][k].report() for k in ("add", "ratio", "before")},
             "nr_applied": {k: st["applied"][k].report(a.nr_margin) for k in ("add", "ratio")}}
        results.append(r)
        print_crop(rect, r)
    return results


def run_pair(a):
    recs = {"add": Recording(a.directory, a.add_name), "ratio": Recording(a.directory, a.ratio_name)}
    if recs["add"].crops != recs["ratio"].crops:
        sys.exit("the two recordings use different crops")
    stats = {}
    for label, rec in recs.items():
        per = []
        for rect in rec.crops:
            shape = (rect[3], rect[2], 3)
            per.append({"temporal": Temporal(shape, a.sparkle), "before": Temporal(shape, a.sparkle),
                        "applied": Applied()})
        for f in range(a.skip, rec.count):
            _, crops = rec.frame(f)
            for st, blocks in zip(per, crops):
                st["temporal"].add(blocks["adjusted"])
                st["before"].add(blocks["before"])
                st["applied"].add(blocks["adjusted"], blocks["before"])
        stats[label] = per
        print(f"{rec.name}: {rec.count - a.skip} frames")
    results = []
    for c, rect in enumerate(recs["add"].crops):
        mA, mR = stats["add"][c]["temporal"].mean, stats["ratio"][c]["temporal"].mean
        d = np.abs(mA - mR)
        r = {"crop": rect,
             "parity": {"kind": "frame means, different recordings", "mean_abs": float(d.mean()),
                        "max_abs": float(d.max()), "psnr_db": psnr_from(float(((mA - mR) ** 2).sum()), d.size)},
             "stability": {"add": stats["add"][c]["temporal"].report(), "ratio": stats["ratio"][c]["temporal"].report(),
                           "before (add run)": stats["add"][c]["before"].report(),
                           "before (ratio run)": stats["ratio"][c]["before"].report()},
             "nr_applied": {k: stats[k][c]["applied"].report(a.nr_margin) for k in ("add", "ratio")}}
        results.append(r)
        print_crop(rect, r)
    return results


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="mode", required=True)
    d = sub.add_parser("dual", help="one recordBoth recording")
    d.add_argument("directory")
    d.add_argument("name")
    q = sub.add_parser("pair", help="an add and a ratio recording")
    q.add_argument("directory")
    q.add_argument("add_name")
    q.add_argument("ratio_name")
    for s in (d, q):
        s.add_argument("--skip", type=int, default=0, help="ignore the first N frames")
        s.add_argument("--sparkle", type=float, default=6.0, help="frame-to-frame change counted as a sparkle")
        s.add_argument("--nr-margin", type=float, default=1.0, help="mean |result - before| that counts as applied")
        s.add_argument("--json", help="also write the results here")
    a = p.parse_args()
    results = run_dual(a) if a.mode == "dual" else run_pair(a)
    if a.json:
        json.dump({"mode": a.mode, "results": results}, open(a.json, "w"), indent=2)


if __name__ == "__main__":
    main()
