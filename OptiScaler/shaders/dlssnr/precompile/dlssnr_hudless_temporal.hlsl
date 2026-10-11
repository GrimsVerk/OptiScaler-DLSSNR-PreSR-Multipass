// HUD-less finished picture: motion-adaptive NR strength and temporal smoothing of NR's change.
// Runs after NR on the game's HUD-less picture. Works on NR's change (NR output - HUD-less input):
//  - smoothing blends that change with the previous frame's smoothed change, moved along the game's motion
//    vectors. History is limited to the range of the current change around the pixel (tolerance) and dropped
//    where the game's own picture changed under the motion (revealed areas, lighting), so it does not smear;
//  - band split (optional): only the coarse part of the change (5x5 binomial) is smoothed; the fine part is
//    taken from the current frame, so fine detail stays sharp. NR's added flicker is mostly coarse;
//  - motion-adaptive strength scales the result by on-screen motion.
// Bindings follow the shared NR root signature: five SRVs (t0..t4), two UAVs (u0, u1), one constant buffer.
// Keep in step with DlssNr::HudlessTemporal::Constants and the offline model (w3night/fastsim.py).

cbuffer Params : register(b0)
{
    uint gMode;   // unused; keeps the shared layout (the dispatch reads width and height at offsets 8 and 12)
    uint gUnused0;
    uint gWidth;
    uint gHeight;
    uint gGuideX;
    uint gGuideY;
    uint gGuideWidth;
    uint gGuideHeight;
    float gMvToOutX; // game motion * this = displacement to the previous frame, in output pixels
    float gMvToOutY;
    uint gHistoryValid;
    uint gSmoothing;
    float gCurrentWeight; // 1 / frames
    float gTolerance;     // how many standard deviations of the local change history may differ by
    uint gAdaptive;
    float gStillStrength;
    float gMotionStrength;
    float gMotionStart; // output pixels per frame
    float gMotionFull;
    uint gDebugView;
    float gReject;    // picture-change threshold (luma); history fades out from gReject to 2 * gReject, 0 = off
    uint gBandSplit;  // 1: smooth the coarse change only
};

Texture2D<float4> gNr : register(t0);        // NR output of the HUD-less picture
Texture2D<float4> gInput : register(t1);     // the HUD-less picture before NR
Texture2D<float4> gHistory : register(t2);   // previous smoothed (coarse or full) change, signed float
Texture2D<float4> gMotion : register(t3);    // the game's motion vectors (copy)
Texture2D<float4> gPrevInput : register(t4); // previous frame's HUD-less picture before NR
RWTexture2D<float4> gHistoryOut : register(u0); // this frame's smoothed change
RWTexture2D<float4> gOutput : register(u1);     // HUD-less picture with the adjusted change

static const float kEpsilon = 0.5 / 255.0;
static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);
static const float kBinomial[5] = { 1.0 / 16.0, 4.0 / 16.0, 6.0 / 16.0, 4.0 / 16.0, 1.0 / 16.0 };

groupshared float3 sChange[12][12];

float3 Finite3(float3 v)
{
    return float3(isfinite(v.x) ? v.x : 0.0, isfinite(v.y) ? v.y : 0.0, isfinite(v.z) ? v.z : 0.0);
}

float4 CatmullRomWeights(float f)
{
    return float4(f * (-0.5 + f * (1.0 - 0.5 * f)), 1.0 + f * f * (-2.5 + 1.5 * f), f * (0.5 + f * (2.0 - 1.5 * f)),
                  f * f * (-0.5 + 0.5 * f));
}

int2 ClampPixel(int2 p) { return clamp(p, int2(0, 0), int2(gWidth - 1, gHeight - 1)); }

// History at a position in output pixel coordinates (pixel centres at +0.5), 4x4 Catmull-Rom.
float3 SampleHistory(float2 position)
{
    float2 texel = position - 0.5;
    float2 base = floor(texel);
    float2 f = texel - base;
    float4 wx = CatmullRomWeights(f.x);
    float4 wy = CatmullRomWeights(f.y);
    float3 sum = 0.0;
    [unroll] for (int j = 0; j < 4; ++j)
    {
        float3 row = 0.0;
        [unroll] for (int i = 0; i < 4; ++i)
            row += wx[i] * Finite3(gHistory.Load(int3(ClampPixel(int2(base) + int2(i - 1, j - 1)), 0)).rgb);
        sum += wy[j] * row;
    }
    return sum;
}

// Previous input luma at a position, bilinear.
float PreviousLuma(float2 position)
{
    float2 texel = clamp(position - 0.5, 0.0, float2(gWidth - 1, gHeight - 1) - 0.001);
    int2 base = int2(floor(texel));
    float2 f = texel - base;
    float a = dot(gPrevInput.Load(int3(base, 0)).rgb, kLuma);
    float b = dot(gPrevInput.Load(int3(base + int2(1, 0), 0)).rgb, kLuma);
    float c = dot(gPrevInput.Load(int3(base + int2(0, 1), 0)).rgb, kLuma);
    float d = dot(gPrevInput.Load(int3(base + int2(1, 1), 0)).rgb, kLuma);
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID, uint3 local : SV_GroupThreadID, uint3 group : SV_GroupID)
{
    // NR's change for this 8x8 tile and a 2-pixel border, once per group.
    int2 tileBase = int2(group.xy) * 8 - 2;
    for (uint i = local.y * 8 + local.x; i < 144; i += 64)
    {
        int2 q = ClampPixel(tileBase + int2(i % 12, i / 12));
        sChange[i / 12][i % 12] = Finite3(gNr.Load(int3(q, 0)).rgb - gInput.Load(int3(q, 0)).rgb);
    }
    GroupMemoryBarrierWithGroupSync();
    if (id.x >= gWidth || id.y >= gHeight)
        return;
    int2 p = int2(id.xy);
    int2 s = int2(local.xy) + 2;
    float4 input = gInput.Load(int3(p, 0));
    float3 change = sChange[s.y][s.x];

    // Coarse part of the change (5x5 binomial); the fine part is change - coarse.
    float3 coarse = change;
    if (gBandSplit != 0)
    {
        coarse = 0.0;
        [unroll] for (int y = 0; y < 5; ++y)
        {
            float3 row = 0.0;
            [unroll] for (int x = 0; x < 5; ++x)
                row += kBinomial[x] * sChange[s.y + y - 2][s.x + x - 2];
            coarse += kBinomial[y] * row;
        }
    }
    float3 band = coarse; // the part that is smoothed
    float3 fine = change - coarse;

    float2 motion = 0.0;
    if (gGuideWidth != 0 && gGuideHeight != 0)
    {
        float2 uv = (float2(p) + 0.5) / float2(gWidth, gHeight);
        uint2 guide = min(uint2(uv * float2(gGuideWidth, gGuideHeight)), uint2(gGuideWidth - 1, gGuideHeight - 1)) +
                      uint2(gGuideX, gGuideY);
        motion = gMotion.Load(int3(guide, 0)).xy * float2(gMvToOutX, gMvToOutY);
        if (!all(isfinite(motion)) || any(abs(motion) > 8192.0))
            motion = 0.0;
    }

    float3 smoothed = band;
    float rejected = 0.0;
    if (gSmoothing != 0 && gHistoryValid != 0)
    {
        float2 previous = float2(p) + 0.5 + motion;
        if (all(previous >= 0.0) && previous.x <= float(gWidth) && previous.y <= float(gHeight))
        {
            // Range of the current change around the pixel (3x3, full band).
            float3 m1 = 0.0, m2 = 0.0;
            [unroll] for (int y = -1; y <= 1; ++y)
                [unroll] for (int x = -1; x <= 1; ++x)
                {
                    float3 c = sChange[s.y + y][s.x + x];
                    m1 += c;
                    m2 += c * c;
                }
            float3 mean = m1 / 9.0;
            float3 sigma = sqrt(max(m2 / 9.0 - mean * mean, 0.0));
            float3 history = clamp(SampleHistory(previous), mean - gTolerance * sigma - kEpsilon,
                                   mean + gTolerance * sigma + kEpsilon);
            float weight = saturate(gCurrentWeight);
            if (gReject > 0.0)
            {
                float changeOfPicture = abs(dot(input.rgb, kLuma) - PreviousLuma(previous));
                rejected = saturate((changeOfPicture - gReject) / gReject);
                weight = lerp(weight, 1.0, rejected);
            }
            smoothed = lerp(history, band, weight);
        }
    }
    gHistoryOut[id.xy] = float4(smoothed, rejected);

    float moving = 0.0;
    float strength = 1.0;
    if (gAdaptive != 0)
    {
        moving = smoothstep(gMotionStart, max(gMotionFull, gMotionStart + 1e-3), length(motion));
        strength = lerp(gStillStrength, gMotionStrength, moving);
    }
    float3 adjusted = smoothed + fine;
    float3 result = saturate(input.rgb + strength * adjusted);
    if (gDebugView == 1)
        result = lerp(input.rgb, float3(1.0, 0.0, 0.0), 0.65 * saturate(length(motion) / max(gMotionFull, 1e-3)));
    else if (gDebugView == 2)
        result = saturate(abs(adjusted - change) * 16.0);
    else if (gDebugView == 3)
        result = lerp(input.rgb, float3(0.0, 1.0, 0.0), 0.65 * rejected);
    gOutput[id.xy] = float4(result, input.a);
}
