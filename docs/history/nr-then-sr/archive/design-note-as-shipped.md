> **Archived verbatim** from the retired (and since deleted) fork GrimsVerk/OptiScaler_NR_then_SR, file `OptiScaler/dlssnr/design/nr-before-upscale.md` at the tip of the old fork's `nr-before-upscale` branch (after the 2026-09-07 rebase). It has the rebase and "Parked" sections but only a pointer to the experiments. Branch names, file paths and code names below refer to that fork and no longer resolve. The summary that stands alone is [../../NR-THEN-SR-EXPERIMENT.md](../../NR-THEN-SR-EXPERIMENT.md); the code is in [../patches](../patches).

# Neural Rendering before the upscaler

Status: BUILT and IN-GAME TESTED (2026-09-03) in Star Wars Jedi: Survivor, D3D12, RTX 5080, driver
616.56, model 310.8.0.0. D3D12 and the two D3D12 bridges only; native Vulkan stays on the
after-upscale path. Ray reconstruction, dynamic resolution and the D3D11 and Vulkan bridges were
out of scope for that test and remain untested.

The follow-up experiments on the stage 1 picture -- where its edge instability comes from, a
motion-vector and history probe, un-jittering the model's input, and a DLAA pre-pass for the
model's eyes -- were built, measured in-game, and judged by eye. None of them earned a place
here; the picture the eye preferred was the raw render. They live, with the measurement tools and
every number, on the branch `nr-stage1-tested-and-discarded`, so nobody has to repeat them.

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

## Rebased onto upstream, 2026-09-07

The branch was rebased onto `dlss-neural-rendering` at `97376162` (fifteen commits: the reversible
proxy modes, frame hold, zero-latency GPU exposure, supersampling above 100% with its own down-leg,
the exposure-scan fixes). What had to change to meet them:

- **Exposure texture.** The encode and resolve now take the game's live exposure texture as an
  extra input. Both sides of the upscaler pass it through unchanged: the texture is the game's, at
  1x1, and means the same thing whichever frame the model is shown.
- **Supersampling down-leg.** The resolve reads the native proxy and the model's answer averaged
  back to native when the working scale is above 1.0. Stage 1 keeps that: "native" is then the
  render size, and the answer is averaged back to it before the upscaler sees the copy.
- **Frame hold.** Upstream freezes the encode's input by copying the target aside at hold-on and
  copying it back over the target every held frame. Before the upscaler the encode's input is the
  game's colour, which the pass never writes, so there is nothing to copy back over. Instead the
  capture is taken from the game's colour (moved to COPY_SOURCE for the copy and put back) and,
  while held, the encode reads the held copy in the colour's place. After the upscaler the
  behaviour is byte-identical to upstream. The upscaler still runs on a held stage-1 frame, so
  what it does with a frozen, jittered input is part of the picture -- the hold's menu text says
  the upscaler is not re-run, which is true only after the upscaler.

Retested the same day in Jedi Survivor (D3D12, 4K, DLSS Quality 2560x1440 -> 3840x2160, frame generation
off, stage 1, model resolution 80%): no visual complaint from the tester, and the whole-frame GPU time at
the same spot matched the pre-rebase build, 17.6 ms both. The pass's own readout: 4.38 ms total, 4.19 ms
model, 0.19 ms ours. The hold on stage 1 has not been exercised.

## Parked

Seen during the Jedi Survivor testing, noted by the tester, deliberately not chased yet. Neither
is known to be this module's fault.

- **Latency rose when frame generation was turned off.** The game's own readout: about 50 ms
  with DLSS frame generation on, about 80 ms with it off, and it could be felt. Seen with stage 1
  on. Could be OptiScaler, this branch, or the game; the tester recalls a well-known title
  behaving the same way on its own. Where to start: whether Reflex is still armed once the
  frame-generation evaluates stop (OptiScaler reports frame generation as off after six
  evaluates without it, through `ReflexHooks::setDlssgFrameCount(0)`), and whether the upstream
  build behaves the same in the same spot.
- **VRAM.** The tester watches it while playing and sees it get very close to full on a 16 GB
  card with the pass on. A ReShade route to the same model leaked until the game hitched;
  OptiScaler holds up better but any reduction is wanted. Where to start: stage 1 already cuts
  the model's working set with the upscaling ratio squared; check that parked resources
  (`ParkNrResource`, freed 32 evaluates later) do not pile up across quality-mode rebuilds; list
  what the pass allocates at create -- the guides, output, the two copies, the small proxy, the
  capture buffers, the stage 1 scratch -- and measure a long session on both stages with the
  game's own readout.

Seen in the 7 September logs and traces, after the rebase. None chased; none known to be stage 1's.

- **Exposure scan overflows its cap in Jedi Survivor.** Upstream's scan adopts a 16-byte buffer as
  candidate 1, then reports more than 64 candidates and calls its own filter too loose here; Unreal
  creates many tiny UAV buffers. Upstream's code, but this game is a good test case for it.
- **"D3D12Device created with non-primary GPU"** twice per launch. Probably the integrated GPU
  being enumerated. Harmless as far as anyone can tell.
- **A thread inside the model DLL spins a full core during play**, seen in the CPU traces taken
  for the game's own performance work. Not in the game's thread list. Whether the model runtime
  polls, and whether it does so on stage 0 too, is unknown.
- **About 900 log lines a second at LogLevel 1 during play**, four per present from LocalPresent
  and one per frame from the input health check, written synchronously on the present path.
  Diagnostic level only; the default level does not do this.
