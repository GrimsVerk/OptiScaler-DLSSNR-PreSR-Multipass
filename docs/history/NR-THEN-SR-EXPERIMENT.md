# History: the "NR then SR" fork (September 2026)

Before this fork existed, the same author kept a separate fork, `GrimsVerk/OptiScaler_NR_then_SR`.
It was a fork of Dagherbou's `OptiScaler_DLSSNR`, branch `dlss-neural-rendering`. Its description
was: "an experiment trying to do the neural rendering before super resolution to decrease frametime
values and vram usage". This fork replaced it, and the old fork is deleted once this document is
merged.

This page is the complete record of what that fork did. It does not link to the old fork, because
those links stop working once the fork is deleted. Everything worth keeping is quoted here or stored
next to this page:

| Path | What it is |
|---|---|
| [`nr-then-sr/patches/stage1-before-upscale.patch`](nr-then-sr/patches/stage1-before-upscale.patch) | The Stage 1 code as last shipped on the old fork's default branch (after the rebase, code only) |
| [`nr-then-sr/patches/stage1-experiments.patch`](nr-then-sr/patches/stage1-experiments.patch) | The Stage 1 code before the rebase, plus every experiment that was tried on it and set aside: the ghost probe, un-jitter, and the DLAA pre-pass |
| [`nr-then-sr/tools/`](nr-then-sr/tools/) | The three PowerShell scripts used to measure captures: byte-compare, PNG view, and temporal stability |
| [`nr-then-sr/archive/`](nr-then-sr/archive/) | The old design note, verbatim, in both versions, and the read-me that shipped in the release zip |

Both patches apply cleanly to commits that are in this repository's history, from Dagherbou's
upstream:

```
git switch -c try-stage1 97376162       # "DLSS-NR: Vulkan supersampling to 2x + honest ms readout"
git apply docs/history/nr-then-sr/patches/stage1-before-upscale.patch

git switch -c try-stage1-exp 21274132   # "DLSS-NR: multi-point anchoring + native-Vulkan resize/device-change crash fixes"
git apply docs/history/nr-then-sr/patches/stage1-experiments.patch
# then regenerate shaders/dlssnr/precompile/DlssNr_Shader.h from dlssnr.hlsl (fxc cs_5_0 +
# shader_tools/create_header.py); the generated header and .cso are left out of the patch
```

## In one paragraph

The old fork added one control, `[DlssNr] Stage`. `0` ran the model after the upscaler, as upstream
did. `1` ran it before the upscaler, on the game's render-size colour. The model then works on fewer
pixels, so its cost falls with the square of the upscaling ratio. In Star Wars Jedi: Survivor at 4K
DLSS Performance on an RTX 5080, the model's cost fell from **11.7 ms to 3.6 ms per frame**. The
picture cost was a fuzzy, unstable edge on moving characters. The cause was measured: the model
re-decides every edge on a jittered, aliased frame. Three fixes were built and measured: a motion
vector and history probe, un-jittering the model's input, and showing the model a DLAA of the frame.
The probe changed nothing. Un-jittering removed about a quarter of the wobble, which the eye could
not see. DLAA removed half of it, but the tester preferred the raw render by eye. So Stage 1 shipped
with the raw render and no fix. The advice was to lower Detail strength, which scales the artefact
down. This fork took over with `RunBeforeSR`, multipass, and later "Denoise first". Denoise first is on this fork's `denoise-first` and `v0.3-dev` branches and on upstream's `main`, but not yet on this fork's `main`. Denoise first is
the DLAA pre-pass idea rebuilt with more options.

## Timeline and what existed

All the work was done on 3 to 7 September 2026 and tested in one game.

| Old branch or tag | What it held |
|---|---|
| `nr-before-upscale` (default branch) | 7 own commits on Dagherbou `97376162`: Stage 1, the frame-generation and capture fixes, the read-me, the 2026-09-07 rebase notes, and the retest |
| `nr-before-upscale-pre-rebase` | The same first 5 commits on the older base `21274132` |
| tag `v1.0.0-nr-before-upscale` | The tip of the pre-rebase branch. The only release was built from it (see "Release binary" below) |
| `nr-stage1-tested-and-discarded` | Stage 1 plus 11 experiment commits: probe, un-jitter, DLAA pre-pass, measurements, verdicts |
| `dlss-neural-rendering`, `dlssnr-pr`, `input-stuck-key`, `master`, and tags `v0.1.0-dlssnr` … `v0.2.0-patch1`, `SPLIT-WORKS`, `SAFE-REVERT`, `DLSSNR-BASELINE` | Upstream's, unchanged. No own work. The same tags still exist on Dagherbou's repository |

The old fork had no issues, pull requests, discussions, wiki pages, Pages site, or Actions runs or
artifacts.

## What Stage 1 was and how it worked

```
Stage 0:  game colour (render size) -> upscaler -> output (display size) -> NR edits output in place
Stage 1:  game colour (render size) -> NR edits a private copy -> upscaler reads the copy -> output
```

The main difference from this fork's `RunBeforeSR`: **the game's own colour buffer was never
written.** The model's edit went into a private copy (`g_pre.scratch`). The copy was then swapped
into the NGX parameter block as `Color`, but only for the duration of the upscaler's evaluate. A
small RAII scope did the swap around each D3D12 upscaler evaluate. There were four call sites:
native DLSS, OptiScaler's own upscalers, the D3D11-on-12 bridge, and the Vulkan-on-12 bridge.

```cpp
bool declined = false;
{
    DlssNr::ScopedPreUpscale pre(cmdList, params, /*applies=*/ !rayReconstruction);
    result = upscaler->Evaluate(cmdList, params);
    declined = pre.Declined();
}
DlssNr::EvaluateAfterUpscale(cmdList, params, queue, declined);
```

What the scope did, step by step:

1. Read `Color`, `Depth` and `MotionVectors` from the block. It remembered whether `Color` was stored
   typed or untyped, because NVIDIA's block keeps the two slots apart and OptiScaler's does not.
2. Sized the copy to the **render subrect** (`DLSS.Render.Subrect.Dimensions`), bounded by the colour
   texture.
3. **Declined** the evaluate, handing it to the after-upscale path instead, in three cases: Ray
   Reconstruction (the colour is undenoised), a colour subrect that does not start at the origin,
   and an sRGB colour format (it has no UAV, and its typed twin would be read back as linear).
4. Allocated the copy at the colour format's typed twin, with UAV and RT allowed. When the size
   changed, the old copy was parked rather than released, because the upscaler might still be
   reading it.
5. Moved the game's colour into `NON_PIXEL_SHADER_RESOURCE`, or from `ColorResourceBarrier` if that
   was set. Then it ran `Dispatch(cmd, colour, depth, mv, scratch, frame)` and moved the colour back.
6. Swapped the copy in only if the resolve actually wrote it. While the model was still being built,
   or on a skipped frame, the upscaler read the game's own colour, exactly as without the pass.
7. The destructor put the original `Color` back.

`Dispatch` needed little change, because it already took colour and output separately. The encode
and the meter now read `colour`, and the resolve wrote `output`. The UAV/SRV barriers were made only
when the two were the same resource. `EvaluateAfterUpscale` stood down on Stage 1 unless the scope
had declined that same evaluate, so the model never ran twice in a frame.

Is this worth porting? This fork's `RunBeforeSR` edits `Color` in place, and copies back from scratch
when `Color` has no UAV flag. If a game reads its pre-upscale colour again after the upscale, an
in-place edit leaks into that read. Possible readers are its own TAA history, a HUD-less capture, or
a second consumer. The swap approach cannot have that problem. It costs one render-size texture and
one parameter-block swap. See `ScopedPreUpscale` in `stage1-before-upscale.patch`.

## First test: numbers and verdict (2026-09-03)

The setup was Star Wars Jedi: Survivor on D3D12 with an RTX 5080, driver 616.56 and model
310.8.0.0. The game ran at 4K windowed, DLSS Performance (1920x1080 render), with the game's own
DLSS frame generation on. Stage was toggled live in the overlay.

| | Stage 0 (after) | Stage 1 (before) |
|---|---|---|
| Model resolution | 3840x2160 | 1920x1080 |
| Model cost per frame | 11.7 ms | 3.6 ms |
| Pass total per frame | 12.1 ms | 3.7 ms |

At DLSS Balanced (2260x1272 render) the model cost 4.5 ms. Switching between Quality, Balanced and
Performance with Stage 1 on reallocated the copy and rebuilt the model, without a crash. The colour
was `R11G11B10_FLOAT`, and it allocated with UAV and RT on the first try.

The tester's verdict was "looks fine". The most noticeable defect was "some ghosting around the
character's head". The picture also had an "upscaly feeling, same as AI-upscaled photos", which
plain DLSS does not give. A matched capture showed the edit as a modest lift in contrast and fine
detail on an aliased, pre-antialiasing frame. There was no tonal shift: the mean luminance of the
edit matched the original to within a few percent.

### Two bugs found and fixed that day

These are worth knowing for anyone who touches pre-SR placement.

1. **Frame-generation evaluates poisoned the after-upscale gate.** The game's DLSS-G evaluates run on
   their own command list, interleaved with the upscaler's. The scope was being built for those
   evaluates too. It declined them, because they are not upscales, and stored the decline in a
   global, which the *upscaler's* after-pass then read. The model then ran once after the upscaler
   at display size, was rebuilt there, and was rebuilt again at render size the next frame. The fix
   was to keep the decline on the scope (`Declined()`), pass it to the after-pass for the same
   evaluate, and give FG evaluates no scope at all. *This fork* gates on the feature type
   (`nrUpscale` in `NVNGX_DLSS_Dx12.cpp`), so it does not have this bug. Do not reintroduce a shared
   global between the before and after paths.
2. **The capture's "before" image was the encoded proxy**, not the untouched frame. So the two images
   in a pair were in different spaces, and the model seemed to darken the frame by a stop. The fix
   was to capture the HDR copy.

### The model has no jitter input

`nvngx_dlssnr.dll` (310.8.0.0) exposes 61 `DLSSNR.*` parameter names. They cover colour, depth,
motion vectors, the masks, the subrects, the strengths, `ScalingRatio` and `Reset`. **None of them is
jitter.** Telling the model the jitter is therefore impossible. Anything that copes with jitter has
to happen in our own shaders.

## Where the "ghosting" and the "boiling" came from

Four changes were made in the same session and scene, each judged by eye:

| Change | Ghosting round the head | Boiling on fine detail |
|---|---|---|
| Detail strength 1.0 -> 0.5 | halves | unchanged |
| Detail strength -> 0 | gone | unchanged |
| Model resolution 100% -> 75% and lower | unchanged (picture softens) | unchanged until the softness hides it |
| Strength 0, Stage 0, and NR disabled outright | (none) | identical in all three |
| Strength 1.0, Stage 0 (after the upscaler) | present, but less | (none) |

At strength 0 on Stage 1, the copy given to the upscaler was **byte-identical** to the game's frame
in all 8 captured frames: zero differing pixels (`tools/dlssnr-rawcompare.ps1`). The pass wrote
nothing it should not.

Conclusions:

- **The boiling is DLSS's own**, not NR's.
- **The "ghost" is in the model's answer.** It scales with blend strength, it is also present after
  the upscaler, and Stage 1 enlarges it.

### The probe: history and motion-vector convention, ruled out

Two diagnostics were added, both live in the menu: `ProbeMvScaleX/Y` multiplies the MV scale given
to the model, and `ProbeResetEveryFrame` resets the model's history every frame. The test used
Detail strength 1, Stage 1 and a moving character, with one change at a time.

**Result: nothing changed the artefact.** That covered reset every frame, and MV scale 0, -1, 0.5
and 2. On a closer look the artefact was not a trailing copy but a fuzzy, unstable edge. So it is not
the model's temporal history, and not a mismatch in MV sign, scale or units. **It is the model
re-deciding the edge on every frame.** The probe code is in `stage1-experiments.patch`. It changes
only the final MV scale and the `Reset` value passed to `evaluate`:

```cpp
(g_nr.reset || probeReset) ? 1 : 0, ...
g_nr.guideMvScaleX * mvToWork * probeMvX, g_nr.guideMvScaleY * mvToWork * probeMvY
```

### Measured: temporal stability of the edit

The method used the game's photo mode, with a frozen world and a static camera. For 8 consecutive
captured frames, it computed the per-pixel temporal standard deviation of luminance. It did this for
the frame and for the edit (after minus before), and split the result into edge pixels and flat
pixels (`tools/dlssnr-stability.ps1`).

| | Stage 0 (after, 3840x2160) | Stage 1 (before, 2560x1440) |
|---|---|---|
| Frame, at edges | 0.0087 | 0.068 |
| Edit, at edges | 0.0041 | 0.0151 |
| Frame, flat | 0.00063 | 0.0019 |
| Edit, flat | 0.00045 | 0.00126 |

The input on Stage 1 is **8x less stable at edges**, because it is aliased and jittered, and the
edit is **3.7x less stable**. The model does damp its input: the edit moves about a fifth as much as
the input at edges. The upscaler is built to integrate the frame's own jitter, but the edit's wobble
is not jitter, so the upscaler cannot integrate it. It lands on screen as the fuzzy edge.

A second test compared the same model cost both ways: the model at 1920x1080, the same render size,
luminance compressed with x/(1+x) first, and medians at edges. The committed script reports means
without the compression, so these numbers came from a local variant of it.

| | Stage 0, working scale 50% | Stage 1, working scale 100% |
|---|---|---|
| Frame the model sees, at edges | 0.0042 | 0.0146 |
| Edit, at edges | 0.0016 | 0.0046 |
| Edit, flat | 0.0002 | 0.0005 |

**Stage 1 is 3x less stable than Stage 0 at a matching working scale, for the same model cost.** In a
still scene that is easy to see. In motion the tester could not tell the two apart by eye. Once the
content itself moves, the aliased input changes every frame whatever the jitter does.

**Lesson: Stage 0 at a reduced working scale is the baseline any pre-SR scheme has to beat**, at
equal model cost.

### Experiment: un-jitter the model's input (`[DlssNr] Unjitter`, 0 off / 1 +jitter / 2 -jitter)

The encode sampled the frame back by the game's `Jitter.Offset`, so the model sees a stable grid. The
resolve read the model's answer back by the same offset. The original was never resampled, so
strength 0 stayed bit-identical. The HLSL change:

```hlsl
float2 UnjitterOffset()
{
    if (gUnjitter == 0) return float2(0.0, 0.0);
    return float2(gJitterX, gJitterY) * (gUnjitter == 2 ? -1.0 : 1.0);
}
// encode: sample the source at (id.xy + 0.5 + UnjitterOffset()) / sourceSize, bilinear
// resolve: read proxy and model at cmpUv - UnjitterOffset() / float2(gWidth, gHeight)
```

The measurement was in photo mode, DLSS Performance, with the model at 1920x1080:

| Un-jitter | Edit wobble at edges (median) | Edit wobble, flat |
|---|---|---|
| Off | 0.0070 | 0.00054 |
| On, +jitter | 0.0050 | 0.00042 |
| On, -jitter | 0.0066 | 0.00055 |

- **+jitter is the right sign for this Unreal title.** Jedi Survivor reports `Jitter.Offset` of about
  a third of a pixel. This is a data point for the sign convention behind `DenoiseFirstFlipJitter`
  in this fork.
- It removes **about a quarter** of the wobble. In motion the tester could not tell it from off.
- Why it does not reach Stage 0 (0.0016): a jittered raster is not just a shifted picture. The
  rasteriser re-decides the coverage of every aliased edge at each sub-pixel offset, and shifting the
  image back cannot rebuild the edge that the other offset would have drawn.

### Experiment: a DLAA pre-pass for the model's eyes (`[DlssNr] Stage1Input`, 0 raw / 1 DLAA)

The driver's own DLSS ran at 1:1 over the render, with the game's own depth, MVs, jitter, exposure
and creation flags. The DLSS creation flags were `0x49`: HDR, inverted depth, and auto exposure. The
output went to a surface only the model reads. The model saw that antialiased picture. Its edit was
composed onto the raw render, read back by the jitter, and the game's DLSS still got its jittered
frame.

- The resolve carried across **only the model's difference** (`gForeign`), because a ratio between an
  antialiased proxy and an aliased frame produces an edge artefact.
- The feature was created on the game's command list and first evaluated a frame later. It was
  parked on a size change, and switched off for the session, with a logged reason, if the driver
  refused it.
- Its GPU cost was never added to the pass's timing row.

| What the model sees | Edit wobble at edges (median) | Edit wobble, flat |
|---|---|---|
| The raw render | 0.0065 | 0.00052 |
| DLAA of the render | 0.0030 | 0.00025 |
| (Stage 0 at working scale 50%, for reference) | 0.0016 | 0.00020 |

So the DLAA input **halved the wobble**. By eye, in motion, the tester still called it *worse*: "more
smooth and blurry", a few fps lower, while the raw path "is actually clearer". Detail strength 1.5 on
the DLAA input did not win it back: "raw is better, more stable looking in motion, at both
strengths".

The interpretation was that the edit on an antialiased input is steadier partly because it is
gentler: there are fewer hard edges to sharpen. Part of what reads as crispness on the raw path is
the instability itself. In motion the DLAA picture also brings its own temporal behaviour, with
history blending and a little smear on fast edges. On top of that, the edit was read back onto the
raw frame with a **bilinear** shift. The DLAA input stayed in the code, off by default, with that
verdict against it.

**This fork's "Denoise first" is the same idea, rebuilt.** It lives on the `denoise-first` and `v0.3-dev` branches, and on upstream's `main`. It runs the game's upscaler at 1:1 (RR
where the game has it). Its default second step, `DenoiseFirstStep=2`, shifts the edit by the jitter
onto the raw render. It also adds the things the old experiment lacked: a ratio edit mode, a
Catmull-Rom or Lanczos kernel instead of bilinear, a sign flip, a neighbourhood clamp, and a firefly
guard. The old result is still a useful warning. In a non-path-traced game with plain DLSS, a better
static number did not mean a better picture in motion. Judge pre-SR variants by eye in motion, not
only by photo-mode stability.

### What was proposed and never built

The old fork proposed a temporal accumulator on the model's *input*: accumulate the proxy over time
with the game's MVs and jitter, so the model sees an antialiased frame without a second DLSS. This is
different from a temporal filter on the model's *answer*, which upstream's README had already
measured as a dead end. The old fork suggested it "if stage 1 is to be pursued", and set it against
the measured alternative: Stage 0 at a working scale with matching cost.

## Rebase onto upstream (2026-09-07)

The branch was rebased onto Dagherbou `97376162`, 15 upstream commits later. Those commits added
reversible proxy modes, frame hold, zero-latency GPU exposure, supersampling above 100% with its own
down-leg, and exposure-scan fixes. Fitting Stage 1 to them meant:

- **The exposure texture** is now an input to both the encode and the resolve, passed through
  unchanged on either side of the upscaler.
- **The supersampling down-leg** still applies on Stage 1, where "native" is the render size.
- **Frame hold:** before the upscaler, the encode's input is the game's colour, which the pass never
  writes. So the hold captures from the game's colour, moving it to COPY_SOURCE and back, and the
  encode reads the held copy in its place. The upscaler still runs on a held Stage 1 frame. The
  hold's menu text says the upscaler is not re-run, which is true only on Stage 0. The hold on
  Stage 1 was never exercised.

The retest was Jedi Survivor at 4K, DLSS Quality (2560x1440 -> 3840x2160), FG off, Stage 1, model
resolution 80%. The tester had no visual complaint. Whole-frame GPU time at the same spot was
**17.6 ms on both the pre-rebase and the rebased build**. The pass read 4.38 ms in total: 4.19 ms
model and 0.19 ms our own work.

## Parked: seen, never chased

None of these was known to be caused by Stage 1. They are still worth checking in this fork.

- **Latency rose with frame generation off.** The game's readout showed about 50 ms with DLSS-G on
  and about 80 ms with it off, and the difference could be felt. This was with Stage 1 on. The cause
  could be OptiScaler, the branch, or the game. The place to start: whether Reflex stays armed after
  FG evaluates stop. OptiScaler reports FG as off after 6 evaluates without it, through
  `ReflexHooks::setDlssgFrameCount(0)`, and that code is still in this fork's `NVNGX_DLSS_Dx12.cpp`.
  Then compare an upstream build in the same spot.
- **VRAM near full on a 16 GB card** with the pass on. A ReShade route to the same model leaked until
  the game hitched; OptiScaler held up better. Ideas: check that parked resources (released 32
  evaluates later) do not pile up across quality-mode rebuilds, list everything the pass allocates,
  and measure a long session on both placements.
- **The exposure scan overflowed its cap in Jedi Survivor.** It adopted a 16-byte buffer as candidate
  1, then reported more than 64 candidates and called its own filter too loose. Unreal creates many
  tiny UAV buffers, which makes this game a good test case for the scan.
- **"D3D12Device created with non-primary GPU"** appeared twice per launch, probably from
  enumerating the integrated GPU. It looked harmless.
- **A thread inside the model DLL spun a full core** during play. It showed in CPU traces and was not
  in the game's thread list. It is unknown whether this also happens on Stage 0.
- **About 900 log lines per second at LogLevel 1**: 4 per present from LocalPresent and 1 per frame
  from the input health check, written synchronously on the present path. This happens only at the
  diagnostic level.

## Risks predicted for pre-SR placement, never tested on the old fork

- **Dynamic resolution:** the copy and the model are rebuilt on every subrect change. Upstream's
  README warns that rebuilding the feature too often "exhausts the driver's latches and the feature
  stops responding until the process restarts". The proposed fix: create the feature at the
  texture's full size and evaluate at the subrect. Whether the model honours a smaller evaluate size
  was untested. This fork's `PADDED-PRESR.md` covers padded allocations but still rebuilds when the
  active size changes.
- **White point:** the exposure the game gives the upscaler describes the pre-upscale buffer, so it
  should be *more* correct before the upscaler. But the paper-white defaults were tuned on the
  output.
- **Resource states:** some games pass colour in `GENERIC_READ` or `PIXEL|NON_PIXEL`. The scope
  assumed `NON_PIXEL_SHADER_RESOURCE` or `ColorResourceBarrier`.
- **Formats:** `R11G11B10_FLOAT`, `R16G16B16A16_FLOAT`, `R8G8B8A8_UNORM` and `R10G10B10A2_UNORM` were
  expected to allocate with UAV and RT. Only R11G11B10 was seen in a game.
- Ray Reconstruction, DRS, and the D3D11 and Vulkan bridges were never tested on Stage 1. Native
  Vulkan was never implemented; the menu greyed Stage out there.

## Why it was superseded

- This fork has pre-SR placement (`RunBeforeSR`) for SR and, since v0.7.4, for RR+SR. It also has
  persistent multipass, padded and max-size colour handling, native Vulkan, `ResidualAcrossRR`,
  generate-early/apply-late (`DeferredDLSS`), and, on `v0.3-dev`, Denoise first. The old fork covered only one game,
  D3D12 only, and only the case without RR.
- The old fork's main finding (pre-SR edits are unstable because the model sees a jittered, aliased
  frame) is exactly the problem that Denoise first and the private-upscaler routes here are built to
  get around.

## Lessons

1. Pre-SR halves to quarters the model's cost (11.7 -> 3.6 ms at 4K Performance), but the saving is
   only worth it if the edge stability holds. Compare against **post-SR at a working scale with
   matching cost**, not against post-SR at 100%.
2. The model has no jitter parameter. Jitter has to be handled outside it.
3. Pre-SR edge instability is **not** caused by history or MVs: resetting every frame and changing
   the MV scale did nothing. It comes from the model's per-frame spatial answer to a shifting
   aliased raster.
4. Un-jittering with a bilinear shift buys about 25% in a still scene and nothing visible in motion.
   +jitter was the correct sign in an Unreal title.
5. A clean 1:1 DLAA input halves static wobble but can still look worse in motion, being softer.
   Judge by eye in motion.
6. Never share before/after decisions through a global. DLSS-G evaluates arrive interleaved on their
   own command list.
7. A capture's "before" must be the untouched HDR frame, not the encoded proxy.
8. At strength 0, verify a byte-identical bypass with a raw byte compare. It is a cheap proof that
   the pass writes nothing it should not.

## Release binary (not uploaded here)

The old fork had one release, a pre-release published 2026-09-03:

| Tag | Asset | Size | SHA-256 |
|---|---|---|---|
| `v1.0.0-nr-before-upscale` ("DLSS 5 Neural Rendering before the upscaler, v1.0.0") | `OptiScaler-DLSSNR-v1.0.0-nr-before-upscale.zip` | 131,248,629 bytes | `4c3929562386617f59dea520d8984e2fbb7cdea210a894185a2556a5b43ac4a3` |

The zip was built from the pre-rebase tip. It held OptiScaler with the Stage control,
`setup_windows.bat`, and `READ ME - DLSS Neural Rendering.txt` (archived in `nr-then-sr/archive/`).
It did not include `nvngx_dlssnr.dll`; users supplied their own. It had 4 downloads. Its release
notes said: "RTX 50 series only, DirectX 12 games that use DLSS … Tested in one game so far. Reports
wanted from 12 GB cards at 1440p, and from any game other than Jedi Survivor." No reports came.

## Credits

The Neural Rendering integration that Stage 1 built on is Dagherbou's ("dag"), in OptiScaler. The
colour composition is RenoDX's DLSS 5 addon by clshortfuse, under the MIT licence (see
`Licenses/RenoDX_ATTRIBUTION.txt`). Nothing here is supported or documented by NVIDIA.
