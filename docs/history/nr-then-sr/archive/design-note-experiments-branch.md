> **Archived verbatim** from the retired (and since deleted) fork GrimsVerk/OptiScaler_NR_then_SR, file `OptiScaler/dlssnr/design/nr-before-upscale.md` at the tip of the old fork's `nr-stage1-tested-and-discarded` branch (2026-09-03, before the rebase). It has every experiment section with its numbers, but not the rebase and "Parked" sections. Branch names, file paths and code names below refer to that fork and no longer resolve. The summary that stands alone is [../../NR-THEN-SR-EXPERIMENT.md](../../NR-THEN-SR-EXPERIMENT.md); the code is in [../patches](../patches).

# Neural Rendering before the upscaler

Status: BUILT and IN-GAME TESTED (2026-09-03) in Star Wars Jedi: Survivor, D3D12, RTX 5080, driver
616.56, model 310.8.0.0. D3D12 and the two D3D12 bridges only; native Vulkan stays on the
after-upscale path. Ray reconstruction, dynamic resolution and the D3D11 and Vulkan bridges were
out of scope for that test and remain untested.

## What the first test found

Jedi Survivor, 4K windowed, DLSS Performance (1920x1080 render), the game's own DLSS frame
generation on. Stage toggled in the overlay while playing; no restart.

| | Stage 0 (after) | Stage 1 (before) |
|---|---|---|
| Model resolution | 3840x2160 | 1920x1080 |
| Model cost / frame | 11.7 ms | 3.6 ms |
| Pass total / frame | 12.1 ms | 3.7 ms |

At DLSS Balanced (2260x1272) the model cost 4.5 ms. Quality, Balanced and Performance were
switched in the game's menu with stage 1 on: each change reallocated the copy and rebuilt the
model, and nothing crashed. The colour is `R11G11B10_FLOAT`, allocated with UAV + RT first try.

What it looks like: the tester's words were "looks fine", with "some ghosting around the
character's head" as the most noticeable defect, and an "upscaly feeling, same as AI-upscaled
photos", which plain DLSS does not give. A matched capture of the render-size frame shows the
model's edit as a modest contrast and micro-detail lift on an aliased, pre-antialiasing frame; the
upscaler then enlarges that. No tonal shift: the mean luminance of the edit matches the original
to within a few percent.

Two things were wrong and were fixed the same day:

1. **Frame generation evaluates were poisoning the after-upscale gate.** The game's DLSS frame
   generation evaluates on its own command list, interleaved with the upscaler's. The scope was
   being constructed for those evaluates too, declined them because they are not upscales, and
   recorded the decline in a global that the *upscaler's* after-pass then read. The model ran once
   after the upscaler at display size, was rebuilt there, and was rebuilt again at render size
   the next frame. The decline now travels with the scope (`ScopedPreUpscale::Declined()`), the
   caller hands it to `EvaluateAfterUpscale` for the same evaluate, and frame generation evaluates
   get no scope at all.
2. **The capture's "before" was the encoded proxy**, not the untouched frame, so before and after
   were in different spaces and the pair looked like the model had darkened the frame by a stop.
   It records the HDR copy now.

### Where the ghosting and the boiling come from

Four experiments, all in the same session, same scene, judged by eye by the tester:

| Change | Ghosting round the head | Boiling on fine detail |
|---|---|---|
| Detail strength 1.0 -> 0.5 | halves | unchanged |
| Detail strength -> 0 | gone | unchanged |
| Model resolution 100% -> 75% and lower | unchanged (picture softens) | unchanged until the softness hides it |
| Strength 0, stage 0, and NR disabled outright | -- | identical in all three |
| Strength 1.0, stage 0 (after the upscaler) | present, but less | -- |

And one measurement: at detail strength 0 on stage 1, the copy handed to the upscaler was
byte-identical to the game's frame in all eight captured frames (`dlssnr-rawcompare.ps1`, zero
differing pixels). The pass writes nothing it should not.

So: the **boiling is DLSS's own** and not this module's business. The **ghost is in the model's
answer** -- it scales with how much of that answer is blended in, it is there on the after-upscale
path as well, and stage 1 makes it bigger because the upscaler enlarges it. Jitter is therefore
not its cause, at most an amplifier, and the motion-vector offset pass is no longer the first
thing to try. The first thing to try is the motion vectors the model is given at all: the ghost
is the signature of a temporal history that is not being moved far enough, or not rejected where
it should be. A scale, sign or unit mismatch between what the game hands DLSS and what the model
expects would produce it on both stages, and would be cheap to probe with a diagnostic multiplier.

That probe is built: `[DlssNr] ProbeMvScaleX/Y` and `ProbeResetEveryFrame`, live in the menu under
"Diagnostics". The test is: detail strength 1, stage 1, a moving character, and one change at a
time -- reset every frame on; then reset off and X and Y both 0; both -1; both 0.5; both 2. Each
answer is one line: does the ghost vanish, shrink, grow, or flip to the other side. If the reset
kills it, the ghost is the history. If a multiplier kills it, that multiplier is the convention
mismatch and becomes the fix. If nothing moves it, the model does this to a single frame and the
remaining lever is detail strength.

**The probe's answer: nothing.** Reset every frame, motion scale 0, -1, 0.5 and 2 -- none of them
changed the artefact, which on a closer look is not a trailing copy but a fuzzy, unstable edge on
the character. So it is not the model's temporal history at all. It is the model re-deciding the
edge on every frame.

**Measured**, in the game's photo mode (frozen world, static camera), eight consecutive frames,
per-pixel temporal standard deviation of luminance, `design/dlssnr-stability.ps1`:

| | Stage 0 (after, 3840x2160) | Stage 1 (before, 2560x1440) |
|---|---|---|
| The frame itself, at edges | 0.0087 | 0.068 |
| The model's edit, at edges | 0.0041 | 0.0151 |
| The frame itself, flat areas | 0.00063 | 0.0019 |
| The model's edit, flat areas | 0.00045 | 0.00126 |

The frame the model sees on stage 1 is eight times less stable at edges than on stage 0, because
it is aliased and jittered, and the model's edit is 3.7 times less stable in turn. The model damps
what it is given -- its edit moves a fifth as much as its input at edges -- but what it is given
moves a lot. The upscaler is built to integrate the frame's own jitter; the edit's wobble is not
jitter, so it cannot, and it lands on screen as the fuzzy edge.

So the jitter *is* the cause after all, through a different mechanism than the design assumed:
not the model's reprojection, but its per-frame spatial answer to an input that shifts under it.
The fix is to show the model a picture that does not shift: resample the colour by the negative
of the game's jitter offset before the encode, so the model works on a stable grid, and resample
its edit back by the jitter before composing it onto the untouched original. The original is never
resampled -- strength 0 stays bit-identical -- and the game's jitter offsets are in the parameter
block already (`Jitter.Offset.X/Y`). The sign convention is engine-dependent; try both.

The zero-code alternative, measured (same spot, same render size, model at 1920x1080 both ways,
luminance compressed x/(1+x) first so the sun cannot dominate, edge medians):

| | Stage 0, working scale 50% | Stage 1, working scale 100% |
|---|---|---|
| The frame the model sees, at edges | 0.0042 | 0.0146 |
| The model's edit, at edges | 0.0016 | 0.0046 |
| The model's edit, flat | 0.0002 | 0.0005 |

Three times less stable on stage 1 for the same model cost, in a frozen scene. In motion the
tester could not tell the two apart by eye, which is consistent: once the content itself moves,
the aliased input changes every frame whatever the jitter does. Un-jittering therefore buys back
the still and slow-moving case, not the fast one.

### Un-jittering, measured

`[DlssNr] Unjitter`, live under the Stage control. Same spot in photo mode, DLSS Performance,
model at 1920x1080, one capture per state:

| Un-jitter | Edit wobble at edges (median) | Edit wobble, flat |
|---|---|---|
| Off | 0.0070 | 0.00054 |
| On, +jitter | 0.0050 | 0.00042 |
| On, -jitter | 0.0066 | 0.00055 |

+jitter is the right sign for Unreal (the game reports `Jitter.Offset` in the range of a third
of a pixel), and it takes about a quarter of the wobble away. By eye, in movement, the tester
could not tell +jitter from off: a quarter of the static wobble is below what the eye picks out
once the content itself is moving. It does not take it to stage 0's
0.0016, and the reason is that a jittered raster is not merely a shifted picture: the coverage of
every aliased edge is re-decided by the rasteriser at each sub-pixel offset, and a bilinear shift
back cannot rebuild the edge the other offset would have drawn. The remainder is the cost of
showing the model an aliased frame at all. The way to remove it is to show the model an
antialiased one, which on this side of the upscaler means accumulating the proxy over time with
the game's motion vectors and jitter -- a small temporal filter on the model's *input*, not on its
answer, which the README rightly measured as a dead end. That is the next experiment if stage 1
is to be pursued; the alternative is the one already measured above, stage 0 at a working scale
that matches the cost.

### The DLAA pre-pass, measured

`[DlssNr] Stage1Input=1`, live under the un-jitter control. The driver's own DLSS at 1:1 over the
render, fed the game's own depth, motion vectors, jitter, exposure and creation flags, into a
surface only the model reads; the edit is composed onto the raw render, read back by the jitter,
and the game's DLSS still gets its jittered frame. Same spot in photo mode, DLSS Performance, model
at 1920x1080:

| What the model sees | Edit wobble at edges (median) | Edit wobble, flat |
|---|---|---|
| The raw render | 0.0065 | 0.00052 |
| The DLAA of the render | 0.0030 | 0.00025 |
| (Stage 0 at working scale 50%, for reference) | 0.0016 | 0.00020 |

By eye, in movement, the tester called DLAA *worse*: "more smooth and blurry", a few fps lower,
against a raw path that "is actually clearer". The measurement and the eye do not disagree: the
edit on an antialiased input is steadier because it is also gentler -- fewer hard edges to sharpen,
so less added, and what is added is smoother -- and part of what reads as crispness on the raw
path is the instability itself. Detail strength 1.5 on the DLAA input did not buy it back: "raw is
better, more stable looking in motion, at both strengths". So the static measurement and the
moving eye part company here. In motion the DLAA picture carries its own temporal behaviour --
history blending, a little smear on fast edges -- and the model edits that, then the edit is read
back onto the raw frame through a bilinear shift; the result reads as softer and less settled
than the model's own answer on the raw raster. The DLAA input stays in the code as an option,
off by default, with this verdict against it. What the eye prefers on stage 1 is the raw render.

Half the wobble gone at edges and in the flat, in one step, with no hand-written temporal filter.
Stage 1 with DLAA now sits at twice stage 0's edge wobble and matches it in the flat. What is left
at edges is the bilinear read-back of the edit onto the jittered raw frame, plus whatever the model
does to a steady picture on its own. The DLAA feature was created by the driver without complaint
(flags 0x49: HDR, inverted depth, auto exposure, as the game's own DLSS is created). Its GPU cost is
not yet in the pass's timing row; it sits outside the bracket.

And one thing the design hoped for is not there: **the model has no jitter parameter.** The DLL
exposes 61 `DLSSNR.*` names -- colour, depth, motion vectors, the masks, the subrects, the
strengths, `ScalingRatio`, `Reset` -- and none of them is jitter. The first fix under "what is
expected to go wrong" is off the table; the motion-vector offset pass is the next one.

## The idea

Today the model runs **after** the upscaler, over the finished display-resolution frame:

    game colour (render size) --> upscaler --> output (display size) --> NR edits output in place

The model's cost and memory scale with the display resolution. At 4K Performance the model is
working on four times the pixels the game rendered.

Stage 1 moves it **before** the upscaler, over the game's render-resolution colour:

    game colour (render size) --> NR edits a private copy --> upscaler reads the copy --> output

The model works on the smaller picture, so its cost and memory fall with the upscaling ratio
squared. What the picture then looks like -- the model was trained on finished frames, this shows
it a jittered, aliased one, and the upscaler enlarges whatever it made of it -- is the open
question, and the reason this is a setting rather than a change.

## What was built

| Piece | Where | What it does |
|---|---|---|
| `[DlssNr] Stage` | `Config.h`, `Config.cpp`, `OptiScaler.ini` | 0 after (default), 1 before |
| Stage combo | `dlssnr/DlssNr_Menu.cpp` | the same control in the menu, greyed on native Vulkan |
| `DlssNr::ScopedPreUpscale` | `dlssnr/DlssNrFeature_Dx12.h`, `shaders/dlssnr/DlssNr_Dx12.cpp` | the before-upscale path, as a scope around the upscaler's evaluate |
| `DlssNr_Dx12::Dispatch` | `shaders/dlssnr/DlssNr_Dx12.{h,cpp}` | now reads `colour` and writes `output`, which may differ; returns whether it wrote |
| `DlssNrFrameInfo::OutputState` | `shaders/dlssnr/DlssNr_Common.h` | lets a caller that owns the output say what state it rests in |
| `GatherFrame` | `shaders/dlssnr/DlssNr_Dx12.cpp` | the parameter-block reading both paths share, carved out of `EvaluateAfterUpscale` |
| Four call sites | `inputs/NVNGX_DLSS_Dx12.cpp` (native DLSS and OptiScaler's upscalers), `upscalers/IFeature_Dx11wDx12.cpp`, `upscalers/IFeature_VkwDx12.cpp` | the scope around each evaluate |

### The scope, step by step

`ScopedPreUpscale` is constructed around the upscaler's evaluate and does nothing unless the pass
is enabled and `Stage == 1`.

1. Reads `Color`, `Depth`, `MotionVectors` from the parameter block, remembering whether `Color`
   was stored typed or untyped (NVIDIA's block keeps the two slots apart; OptiScaler's does not).
2. Sizes the working copy to the **render subrect** (`DLSS.Render.Subrect.Dimensions`), bounded by
   the colour texture. A dynamic-resolution game renders into the corner of a larger allocation.
3. Declines to the after-upscale path if: the evaluate is ray reconstruction (undenoised colour),
   the colour subrect base is not the origin, or the colour is an sRGB format (no UAV, and its
   typed twin would be read back as linear).
4. Allocates or reallocates the copy (`g_pre.scratch`, UAV + RT allowed) at the typed twin of the
   colour's format. The old one is parked, not released, because the upscaler may still be reading
   it on the GPU.
5. Moves the game's colour from `ColorResourceBarrier` (if set) to `NON_PIXEL_SHADER_RESOURCE`,
   calls `Dispatch(cmdList, colour, depth, motion, scratch, frame)`, moves it back.
6. If the resolve actually wrote the copy: moves the copy into the state the game's colour rests in
   and writes it into the block as `Color`, the same way (typed or untyped) the original was stored.
   If not -- the model is still being built, or the frame was skipped -- the block is left alone
   and the upscaler reads the game's own colour, exactly as without the pass.
7. The destructor puts the original `Color` back and rests the copy in `UNORDERED_ACCESS`.

`EvaluateAfterUpscale` stands down on stage 1 unless the scope set `g_preUpscaleDeclined` for that
evaluate, so the model never runs twice a frame.

### What `Dispatch` needed

Almost nothing. The pass already took `colour` and `output` separately and then ignored `colour`.
Now the encode and the meter read `colour` (`source`), the resolve writes `output` (`target`), and
the UAV <-> SRV barriers on the target around those reads are made only when the two are the same
resource. When they differ, the source is already in `NON_PIXEL_SHADER_RESOURCE` -- NGX's contract
for every input -- and is never transitioned. The encode reads the source's top-left
`width x height`, which is the subrect; the copy is exactly that size, so every size the pass
derives from `output->GetDesc()` is right without a new field.

Everything else -- the working scale, the exposure meter, the white point sources, the capture, the
compare views, the timing -- works unchanged on either side. They are all functions of `source`,
`target` and the parameter block.

## What is expected to go wrong first, in order

1. **Jitter.** The render-size colour is sub-pixel jittered every frame and the model's temporal
   reprojection has no idea. Motion vectors do not carry jitter. Expect shimmer or a soft crawl on
   fine detail, especially at rest. Things to try, in order:
   - Scan `nvngx_dlssnr.dll` for a `DLSSNR.Jitter*` parameter name (the model has 61 `DLSSNR.*`
     names; the current code writes about 20). If one exists, write `Jitter.Offset.X/Y` from the
     game's block into it before evaluate. That is a two-line change in the forwarder's evaluate.
   - Failing that, offset the motion vectors by `(jitterNow - jitterPrev)` in a small compute pass
     over the motion clone (`ReadableGuide` already clones typeless guides; a typed clone path
     plus one more `DlssNrMode` would do it). Sign convention is engine-dependent; try both.
   - Failing that, the model's working scale below 100% (`WorkingScale`) softens its input and may
     hide the jitter at a further cost in detail.
2. **Dynamic resolution.** The copy is reallocated and the model rebuilt every time the render
   subrect changes. `README.md` warns that rebuilding the feature too often "exhausts the driver's
   latches and the feature stops responding until the process restarts". In a DRS game expect
   exactly that. The fix is the one NGX itself uses: create the feature at the colour texture's
   full size and evaluate at the subrect. Both the forwarder's create and evaluate already take a
   width and height; whether the model honours a smaller evaluate size is untested. If it does,
   size `g_pre.scratch` and the feature to `colourDesc` and pass the subrect only to evaluate.
   Until then, lock the render resolution in DRS games when testing.
3. **The white point.** The exposure texture and pre-exposure the game hands the upscaler describe
   the same colour buffer the model now reads, so they should be *more* right here than after the
   upscaler, not less. But the paper-white slider defaults were tuned on the output. Re-check.
4. **Motion vector scale.** The pass passes the game's `MV.Scale` through unchanged and lets the
   model relate the guide subrect to its output. Before the upscaler the two are the same size, so
   nothing should change. If the model warps, this is the second thing to look at after jitter.
5. **Resource states on the native DLSS route.** The scope assumes the game's colour rests in
   `NON_PIXEL_SHADER_RESOURCE` (or `ColorResourceBarrier` when set). A game that hands DLSS its
   colour in `GENERIC_READ` or `PIXEL|NON_PIXEL` will draw a debug-layer complaint; on NVIDIA
   hardware the read still works. If it visibly does not, set `ColorResourceBarrier` in the ini.
6. **Colour formats.** `R11G11B10_FLOAT`, `R16G16B16A16_FLOAT`, `R8G8B8A8_UNORM`,
   `R10G10B10A2_UNORM` should all allocate with UAV + RT. If `CreatePreUpscaleScratch` falls back
   to the plain scratch and `ColorResourceBarrier` says `RENDER_TARGET`, the transition into that
   state is invalid; the log line "the model works on the ... render-size colour" says which format
   was chosen.

## Test plan

1. Build. `Stage=auto` (0): the pass must be byte-identical to before. `DlssNrEnabled=false`:
   the scope is a no-op on every route.
2. Cyberpunk or any D3D12 DLSS game with a fixed render resolution, `Stage=1`, DLSS Performance.
   Confirm in the log: "DLSS-NR before the upscaler: the model works on WxH", "DLSS-NR running at
   WxH ... before the upscaler", and the after-upscale skip line. Confirm the cost row in the
   timing table drops by roughly the upscaling ratio squared.
3. Toggle `Stage` in the menu while running. The target changes size, the pass rebuilds, no crash.
4. `Compare` mode 2 (wipe) on stage 1 shows the *render-size* frame against the model's edit,
   before the upscaler; that is expected, and it is the honest comparison of what the model did.
5. Take matched captures (`AutoCapture`) on both stages of the same scene and look at fine
   detail at rest and in motion. That is the answer to the question this was built for.
6. A DRS game (or the DRS override in OptiScaler) to reproduce item 2 above and decide whether
   create-at-max is worth doing.
7. A DLSS-RR game: the pass must log the ray-reconstruction decline and run after the upscaler.
8. A D3D11 game and a Vulkan game through the bridges: same log lines, same picture.

## What this deliberately does not do

- No jitter compensation yet. See above; it is the first follow-up.
- No native Vulkan path. `DlssNrFeature_Vk.cpp` would need the same source/target split and a
  scope around the Vulkan evaluate; the D3D12 version is the template.
- No change to the composition. The proxy, the ratio transfer, the hue correction and the guard
  are the same code on both sides. If the model's answer on a jittered frame needs a different
  composition, that is a second experiment, not this one.
