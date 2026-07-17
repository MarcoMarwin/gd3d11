SamplerState SS_Linear : register( s0 );
Texture2D TX_Raw : register( t0 );
Texture2D TX_History : register( t1 );
Texture2D TX_Depth : register( t2 );
Texture2D TX_Normals : register( t3 );
Texture2D TX_Velocity : register( t4 );
Texture2D TX_PrevDepth : register( t5 );
Texture2D TX_Material : register( t6 );

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
struct PS_OUTPUT { float4 Lighting : SV_TARGET0; float4 Depth : SV_TARGET1; };

float ViewZ(float depth) { return SSL_ProjParams.z / (depth - SSL_ProjParams.w); }
float IsContactReceiverExcluded(float2 uv)
{
    float2 materialInfo = TX_Material.SampleLevel(SS_Linear, saturate(uv), 0).rg;
    float alphaTested = materialInfo.y < 0.0f ? 1.0f : 0.0f;
    float npc = (materialInfo.x < -0.5f && materialInfo.x > -2.0f) ? 1.0f : 0.0f;
    return max(alphaTested, npc);
}

float4 NeighborhoodCurrent(float2 uv, out float4 minV, out float4 maxV)
{
    float centerDepth = TX_Depth.SampleLevel(SS_Linear, uv, 0).r;
    float centerZ = ViewZ(centerDepth);
    float2 centerNormal = TX_Normals.SampleLevel(SS_Linear, uv, 0).xy;
    float4 sum = 0.0f;
    float weightSum = 0.0f;
    minV = 100000.0f;
    maxV = -100000.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y) {
        [unroll]
        for (int x = -1; x <= 1; ++x) {
            float2 suv = saturate(uv + float2(x, y) * SSL_InvResolution);
            float d = TX_Depth.SampleLevel(SS_Linear, suv, 0).r;
            float dz = abs(ViewZ(d) - centerZ);
            float2 sampleNormal = TX_Normals.SampleLevel(SS_Linear, suv, 0).xy;
            float normalWeight = pow(1.0f - saturate(length(centerNormal - sampleNormal) * 2.0f), 8.0f);
            float w = exp(-dz * 0.015f) * normalWeight * (x == 0 && y == 0 ? 2.0f : 1.0f);
            float4 v = TX_Raw.SampleLevel(SS_Linear, suv, 0);
            if (normalWeight > 0.5f) {
                minV = min(minV, v);
                maxV = max(maxV, v);
            }
            sum += v * w;
            weightSum += w;
        }
    }
    return sum / max(weightSum, 0.0001f);
}

float SoftContact(float2 uv, float currentAlpha, float excludedReceiver)
{
    if (excludedReceiver > 0.5f)
        return 0.0f;

    float centerDepth = TX_Depth.SampleLevel(SS_Linear, uv, 0).r;
    float centerZ = ViewZ(centerDepth);
    float2 centerNormal = TX_Normals.SampleLevel(SS_Linear, uv, 0).xy;
    float sum = currentAlpha * 1.25f;
    float weightSum = 1.25f;
    float maxContact = currentAlpha;
    static const float2 offsets[4] = {
        float2(-1.0f, 0.0f),
        float2(1.0f, 0.0f),
        float2(0.0f, -1.0f),
        float2(0.0f, 1.0f)
    };
    [unroll]
    for (int i = 0; i < 4; ++i) {
        float2 sampleUV = saturate(uv + offsets[i] * SSL_InvResolution * 2.0f);
        if (IsContactReceiverExcluded(sampleUV) > 0.5f)
            continue;
        float sampleDepth = TX_Depth.SampleLevel(SS_Linear, sampleUV, 0).r;
        float dz = abs(ViewZ(sampleDepth) - centerZ);
        float2 sampleNormal = TX_Normals.SampleLevel(SS_Linear, sampleUV, 0).xy;
        float normalWeight = pow(1.0f - saturate(length(centerNormal - sampleNormal) * 1.6f), 6.0f);
        float w = exp(-dz * 0.010f) * normalWeight * 0.95f;
        float sampleContact = TX_Raw.SampleLevel(SS_Linear, sampleUV, 0).a;
        sum += sampleContact * w;
        weightSum += w;
        maxContact = max(maxContact, sampleContact * saturate(normalWeight));
    }
    float filtered = sum / max(weightSum, 0.0001f);
    return lerp(filtered, maxContact, 0.35f);
}

PS_OUTPUT PSMain(PS_INPUT input)
{
    PS_OUTPUT output;
    float2 uv = input.vTexcoord;
    float excludedContactReceiver = IsContactReceiverExcluded(uv);
    float4 minV, maxV;
    float4 current = NeighborhoodCurrent(uv, minV, maxV);
    [branch]
    if (SSL_EnableContact > 0.5f)
        current.a = SoftContact(uv, current.a, excludedContactReceiver);
    else
        current.a = 0.0f;
    float depth = TX_Depth.SampleLevel(SS_Linear, uv, 0).r;
    float2 velocity = TX_Velocity.SampleLevel(SS_Linear, uv, 0).rg;
    float2 prevUV = uv + velocity;
    float fsr3 = saturate(SSL_FSR3Active);
    float valid = SSL_HistoryValid;
    valid *= step(0.0f, prevUV.x) * step(prevUV.x, 1.0f) * step(0.0f, prevUV.y) * step(prevUV.y, 1.0f);
    float colorValid = valid;
    float contactValid = valid;
    float prevDepth = TX_PrevDepth.SampleLevel(SS_Linear, saturate(prevUV), 0).r;
    float currentViewZ = ViewZ(depth);
    float depthDiff = abs(currentViewZ - ViewZ(prevDepth));
    float depthThreshold = max(12.0f, abs(currentViewZ) * 0.02f);
    colorValid *= 1.0f - smoothstep(depthThreshold, depthThreshold * 4.0f, depthDiff);
    float contactDepthThreshold = lerp(depthThreshold, max(28.0f, abs(currentViewZ) * 0.05f), fsr3);
    contactValid *= 1.0f - smoothstep(contactDepthThreshold, contactDepthThreshold * 5.0f, depthDiff);

    float2 centerNormal = TX_Normals.SampleLevel(SS_Linear, uv, 0).xy;
    float2 reprojectedNormal = TX_Normals.SampleLevel(SS_Linear, saturate(prevUV), 0).xy;
    float normalDiff = length(centerNormal - reprojectedNormal);
    colorValid *= 1.0f - smoothstep(0.10f, 0.35f, normalDiff);
    contactValid *= 1.0f - smoothstep(lerp(0.10f, 0.18f, fsr3), lerp(0.35f, 0.58f, fsr3), normalDiff);

    float4 history = TX_History.SampleLevel(SS_Linear, saturate(prevUV), 0);
    float colorClampMargin = lerp(0.08f, 0.12f, fsr3);
    history.rgb = clamp(history.rgb, minV.rgb - colorClampMargin, maxV.rgb + colorClampMargin);
    float contactClampMargin = lerp(0.06f, 0.16f, fsr3);
    history.a = clamp(history.a, minV.a - contactClampMargin, maxV.a + contactClampMargin);

    float motion = saturate(length(velocity) * 240.0f);
    float historyWeight = lerp(0.92f, 0.55f, motion) * colorValid;
    float contactDelta = abs(current.a - history.a);
    float contactStability = 1.0f - smoothstep(0.05f, 0.30f, contactDelta);
    float contactMotionWeight = lerp(lerp(0.90f, 0.60f, motion), lerp(0.94f, 0.78f, motion), fsr3);
    float fsr3Retention = lerp(0.84f, 0.95f, contactStability);
    float contactHistoryWeight = contactMotionWeight * contactValid * lerp(1.0f, fsr3Retention, fsr3);
    output.Lighting.rgb = lerp(current.rgb, history.rgb, historyWeight);
    output.Lighting.a = lerp(current.a, history.a, contactHistoryWeight);
    output.Lighting.rgb = max(output.Lighting.rgb, 0.0f);
    output.Lighting.a = excludedContactReceiver > 0.5f ? 0.0f : saturate(output.Lighting.a);
    output.Depth = float4(depth, depth, depth, depth);
    return output;
}
