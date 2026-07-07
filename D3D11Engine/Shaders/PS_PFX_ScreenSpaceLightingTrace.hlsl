#include "DS_Defines.h"

SamplerState SS_Linear : register( s0 );
Texture2D TX_Scene : register( t0 );
Texture2D TX_Depth : register( t1 );
Texture2D TX_Normals : register( t2 );
Texture2D TX_WaterMask : register( t3 );
Texture2D TX_Material : register( t4 );

cbuffer ScreenSpaceLightingConstantBuffer : register( b0 )
{
    float4 SSL_ProjParams;
    matrix SSL_Projection;
    matrix SSL_View;
    matrix SSL_InvView;
    float2 SSL_InvResolution;
    float SSL_ContactStrength;
    float SSL_GIStrength;
    float3 SSL_LightDirectionVS;
    float SSL_FrameIndex;
    float SSL_EnableContact;
    float SSL_EnableGI;
    float SSL_HistoryValid;
    float SSL_FSR3Active;
};

struct PS_INPUT { float2 vTexcoord : TEXCOORD0; float3 vEyeRay : TEXCOORD1; float4 vPosition : SV_POSITION; };

float DepthRaw(float2 uv) { return TX_Depth.SampleLevel(SS_Linear, saturate(uv), 0).r; }
float WaterMask(float2 uv) { return TX_WaterMask.SampleLevel(SS_Linear, saturate(uv), 0).r; }
float IsGeometry(float d) { return step(0.000001f, d); }
float3 ViewPosition(float2 uv, float depth)
{
    float viewZ = SSL_ProjParams.z / (depth - SSL_ProjParams.w);
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    return float3(ndc.x * viewZ * SSL_ProjParams.x, ndc.y * viewZ * SSL_ProjParams.y, viewZ);
}
float3 ViewNormal(float2 uv) { return DecodeNormalGBuffer(TX_Normals.SampleLevel(SS_Linear, saturate(uv), 0).xy); }
float2 ProjectView(float3 viewPos, out float valid)
{
    float4 clip = mul(float4(viewPos, 1.0f), SSL_Projection);
    valid = step(0.001f, clip.w);
    float2 uv = clip.xy / max(clip.w, 0.001f) * float2(0.5f, -0.5f) + 0.5f;
    valid *= step(0.0f, uv.x) * step(uv.x, 1.0f) * step(0.0f, uv.y) * step(uv.y, 1.0f);
    return uv;
}
float Hash12(float2 p) { return frac(52.9829189f * frac(dot(p, float2(0.06711056f, 0.00583715f)))); }

bool TraceRay(float3 origin, float3 dir, float maxDistance, int steps, float jitter, float minThickness, float thicknessScale, float maxThickness, out float2 hitUV, out float hitDistance)
{
    hitUV = 0.0f;
    hitDistance = maxDistance;
    float prevDelta = -1.0f;
    [loop]
    for (int i = 0; i < 16; ++i) {
        if (i >= steps) break;
        float t = ((float)i + 1.0f + jitter * 0.12f) / ((float)steps + 0.20f);
        float travel = maxDistance * lerp(t, t * t, 0.45f);
        float3 p = origin + dir * travel;
        if (p.z <= 1.0f) break;
        float valid;
        float2 uv = ProjectView(p, valid);
        if (valid < 0.5f) break;
        if (WaterMask(uv) > 0.02f) { prevDelta = -1.0f; continue; }
        float d = DepthRaw(uv);
        if (IsGeometry(d) < 0.5f) { prevDelta = -1.0f; continue; }
        float sceneZ = ViewPosition(uv, d).z;
        float delta = p.z - sceneZ;
        float thickness = clamp(travel * thicknessScale, minThickness, maxThickness);
        if ((delta > minThickness * 0.35f && delta < thickness) || (prevDelta < -minThickness * 0.35f && delta >= -minThickness * 0.20f && delta < thickness)) {
            hitUV = uv;
            hitDistance = travel;
            return true;
        }
        prevDelta = delta;
    }
    return false;
}

float ComputeContact(float2 uv, float depth)
{
    if (SSL_EnableContact < 0.5f || SSL_ContactStrength <= 0.001f || IsGeometry(depth) < 0.5f || WaterMask(uv) > 0.02f) return 0.0f;
    float3 vp = ViewPosition(uv, depth);
    float3 n = ViewNormal(uv);
    float3 l = normalize(SSL_LightDirectionVS);
    float nl = dot(n, l);
    if (nl <= -0.20f) return 0.0f;
    float facing = saturate(nl * 0.85f + 0.15f);

    float materialClass = TX_Material.SampleLevel(SS_Linear, saturate(uv), 0).r;
    float npcMaterial = (materialClass < -0.5f && materialClass > -2.0f) ? 1.0f : 0.0f;
    float contactTracePhase = 0.5f;
    float jitter = contactTracePhase;

    // Keep contact shadows object-readable in the near field and deterministic
    // across frames; FSR3 then receives a stable alpha mask instead of shimmer.
    float viewDistanceFade = 1.0f - smoothstep(1200.0f, 2600.0f, vp.z);
    if (viewDistanceFade <= 0.001f) return 0.0f;
    float worldMaxDistance = clamp(vp.z * 0.0065f, 18.0f, 105.0f);
    float npcMaxDistance = clamp(vp.z * 0.0035f, 10.0f, 38.0f);
    float maxDistance = lerp(worldMaxDistance, npcMaxDistance, npcMaterial);
    float originOffset = lerp(1.2f, 4.0f, npcMaterial);
    float minThickness = lerp(2.2f, 2.8f, npcMaterial);
    float thicknessScale = lerp(0.052f, 0.026f, npcMaterial);
    float maxThickness = lerp(22.0f, 8.0f, npcMaterial);

    float2 hitUV; float hitDistance;
    // Ten samples retain the sign-crossing test for thin occluders while
    // reducing contact-ray work by roughly 29 percent.
    if (!TraceRay(vp + n * originOffset, l, maxDistance, 10, jitter, minThickness, thicknessScale, maxThickness, hitUV, hitDistance)) return 0.0f;
    float3 hn = ViewNormal(hitUV);
    float occluderFacing = saturate(dot(hn, -l));
    float worldNormalGate = lerp(0.68f, 1.0f, occluderFacing);
    float npcNormalGate = lerp(0.40f, 0.92f, occluderFacing);
    float normalGate = lerp(worldNormalGate, npcNormalGate, npcMaterial);
    float distanceFade = 1.0f - smoothstep(maxDistance * 0.38f, maxDistance, hitDistance);
    float npcStrength = lerp(1.0f, 0.55f, npcMaterial);
    return saturate(facing * normalGate * distanceFade * viewDistanceFade * SSL_ContactStrength * 0.88f * npcStrength);
}

float3 ComputeGI(float2 uv, float depth, float3 baseColor)
{
    if (SSL_EnableGI < 0.5f || IsGeometry(depth) < 0.5f || WaterMask(uv) > 0.02f) return 0.0f;
    float3 vp = ViewPosition(uv, depth);
    float3 n = ViewNormal(uv);
    float3 wsN = normalize(mul(float4(n, 0.0f), SSL_InvView).xyz);
    float3 helper = abs(wsN.y) < 0.95f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 t = normalize(cross(helper, wsN));
    float3 b = normalize(cross(wsN, t));
    float maxDistance = clamp(vp.z * 0.065f, 280.0f, 1700.0f);
    float3 sum = 0.0f;
    float wsum = 0.0f;
    int hitCount = 0;
    float2 pixel = floor(uv / SSL_InvResolution);
    float pixelNoise = Hash12(pixel);
    int framePhase = (int)SSL_FrameIndex & 3;

    // Four low-discrepancy rays per frame replace eight independently randomized
    // rays. The four-frame cycle covers sixteen stable directions and lets the
    // temporal pass accumulate a denser result at less than half the ray cost.
    [unroll]
    for (int i = 0; i < 4; ++i) {
        int sampleIndex = i + framePhase * 4;
        float sampleId = (float)sampleIndex;
        float seq = frac(pixelNoise + 0.17f + sampleId * 0.6180339f);
        float cosTheta = lerp(0.26f, 0.84f, seq);
        float sinTheta = sqrt(saturate(1.0f - cosTheta * cosTheta));
        float phi = sampleId * 2.3999632f + pixelNoise * 6.2831853f;
        float3 dirWS = normalize(t * (cos(phi) * sinTheta) + b * (sin(phi) * sinTheta) + wsN * cosTheta);
        float3 dirVS = normalize(mul(float4(dirWS, 0.0f), SSL_View).xyz);
        float2 hitUV; float hitDistance;
        float rayJitter = frac(pixelNoise + sampleId * 0.371f);
        if (TraceRay(vp + n * 4.5f, dirVS, maxDistance, 8, rayJitter, 4.0f, 0.042f, 34.0f, hitUV, hitDistance)) {
            float3 hn = ViewNormal(hitUV);
            float receiver = saturate(dot(n, dirVS));
            float emitter = lerp(0.35f, 1.0f, saturate(dot(hn, -dirVS)));
            float normalizedDistance = hitDistance / maxDistance;
            float distanceWeight = rcp(1.0f + 4.0f * normalizedDistance);
            float rangeFade = 1.0f - smoothstep(0.68f, 1.0f, normalizedDistance);
            float weight = receiver * emitter * distanceWeight * rangeFade;
            float3 src = TX_Scene.SampleLevel(SS_Linear, hitUV, 0).rgb;
            float luma = dot(src, float3(0.2126f, 0.7152f, 0.0722f));
            src = min(src / (1.0f + max(0.0f, luma - 1.0f) * 0.8f), float3(3.0f, 3.0f, 3.0f));
            sum += src * weight;
            wsum += weight;
            hitCount++;
        }
    }
    if (hitCount < 1 || wsum <= 0.035f) return 0.0f;
    float baseLuma = dot(baseColor, float3(0.2126f, 0.7152f, 0.0722f));
    float3 gi = (sum / wsum) * SSL_GIStrength * 0.28f / (1.0f + baseLuma * 0.22f);
    return min(gi, baseColor * 0.45f + 0.16f);
}

float4 PSMain(PS_INPUT input) : SV_TARGET
{
    float2 uv = input.vTexcoord;
    float depth = DepthRaw(uv);
    float3 baseColor = TX_Scene.SampleLevel(SS_Linear, uv, 0).rgb;
    float contact = ComputeContact(uv, depth);
    float3 gi = ComputeGI(uv, depth, baseColor);
    return float4(max(gi, 0.0f), contact);
}
