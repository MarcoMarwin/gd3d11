// Camera velocity fallback. FSR3 performs its own depth-aware dilation, so the
// producer must provide the exact center-pixel vector without a 3x3 max filter.

cbuffer VelocityConstants : register(b0) {
    float4x4 InvViewProj;      // Current frame's UNJITTERED inverse view-projection
    float4x4 PrevViewProj;     // Previous frame's UNJITTERED view-projection
    float2 JitterOffset;       // Current jitter in UV space
    float2 PrevJitterOffset;   // Previous jitter in UV space
    float2 Resolution;
    float2 Padding;
};

SamplerState SS_Linear : register(s0);
SamplerState SS_Point : register(s1);

Texture2D TX_Depth : register(t0);  // Current frame depth

struct PS_INPUT
{
    float2 vTexcoord   : TEXCOORD0;
    float3 vEyeRay     : TEXCOORD1;
    float4 vPosition   : SV_POSITION;
};

bool IsSkyDepth(float depth) {
    return depth <= 1e-7f;
}

// Reconstruct world position from depth
// Note: This engine uses REVERSED-Z: depth 1 = near, depth 0 = far (sky)
float3 ReconstructWorldPosition(float2 uv, float depth) {
    float4 clipPos = float4(
        uv.x * 2.0 - 1.0,
        (1.0 - uv.y) * 2.0 - 1.0,
        depth,
        1.0
    );

    float4 worldPos = mul(clipPos, InvViewProj);
    return worldPos.xyz / worldPos.w;
}

float2 ProjectToPreviousFrame(float3 worldPos) {
    float4 prevClipPos = mul(float4(worldPos, 1.0), PrevViewProj);
    float2 prevNDC = prevClipPos.xy / prevClipPos.w;
    return float2(prevNDC.x * 0.5 + 0.5, 1.0 - (prevNDC.y * 0.5 + 0.5));
}

float2 CalculateVelocity(float2 texCoord, float depth) {
    if (IsSkyDepth(depth)) {
        return float2(0.0, 0.0);
    }

    float2 currentUV = texCoord - JitterOffset;
    float3 worldPos = ReconstructWorldPosition(currentUV, depth);
    float2 prevUV = ProjectToPreviousFrame(worldPos);
    return prevUV - currentUV;
}

float2 PSMain(PS_INPUT Input) : SV_TARGET {
    float2 texCoord = Input.vTexcoord;
    float depth = TX_Depth.SampleLevel(SS_Point, texCoord, 0).r;
    return CalculateVelocity(texCoord, depth);
}
