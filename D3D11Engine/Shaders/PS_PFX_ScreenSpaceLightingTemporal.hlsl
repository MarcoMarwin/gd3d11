SamplerState SS_Linear : register( s0 );
Texture2D TX_Raw : register( t0 );
Texture2D TX_History : register( t1 );
Texture2D TX_Depth : register( t2 );
Texture2D TX_Normals : register( t3 );
Texture2D TX_Velocity : register( t4 );
Texture2D TX_PrevDepth : register( t5 );

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

PS_OUTPUT PSMain(PS_INPUT input)
{
    PS_OUTPUT output;
    float2 uv = input.vTexcoord;
    float4 minV, maxV;
    float4 current = NeighborhoodCurrent(uv, minV, maxV);
    float depth = TX_Depth.SampleLevel(SS_Linear, uv, 0).r;
    float2 velocity = TX_Velocity.SampleLevel(SS_Linear, uv, 0).rg;
    float2 prevUV = uv + velocity;
    float valid = SSL_HistoryValid;
    valid *= step(0.0f, prevUV.x) * step(prevUV.x, 1.0f) * step(0.0f, prevUV.y) * step(prevUV.y, 1.0f);
    float prevDepth = TX_PrevDepth.SampleLevel(SS_Linear, saturate(prevUV), 0).r;
    float depthDiff = abs(ViewZ(depth) - ViewZ(prevDepth));
    valid *= 1.0f - smoothstep(40.0f, 180.0f, depthDiff);
    float4 history = TX_History.SampleLevel(SS_Linear, saturate(prevUV), 0);
    history = clamp(history, minV - 0.04f, maxV + 0.04f);
    float motion = saturate(length(velocity) * 240.0f);
    float historyWeight = lerp(0.86f, 0.45f, motion) * valid;
    float fsr3ContactHistoryWeight = lerp(0.90f, 0.30f, motion) * valid;
    float contactHistoryWeight = lerp(historyWeight, fsr3ContactHistoryWeight, step(0.5f, SSL_FSR3Active));
    output.Lighting.rgb = max(lerp(current.rgb, history.rgb, historyWeight), 0.0f);
    output.Lighting.a = saturate(lerp(current.a, history.a, contactHistoryWeight));
    output.Depth = float4(depth, depth, depth, depth);
    return output;
}