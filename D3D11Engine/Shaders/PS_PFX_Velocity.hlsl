// Velocity Buffer Pixel Shader with depth-aware dilation
// Generates screen-space motion vectors from depth buffer reprojection.

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

float2 DilateVelocity3x3(float2 texCoord, float2 pixelSize) {
    float centerDepth = TX_Depth.SampleLevel(SS_Point, texCoord, 0).r;
    float2 centerVelocity = CalculateVelocity(texCoord, centerDepth);
    if (IsSkyDepth(centerDepth)) {
        return centerVelocity;
    }

    float2 bestVelocity = centerVelocity;
    float bestMagnitudeSq = dot(centerVelocity, centerVelocity);
    float depthTolerance = 0.002f + saturate(1.0f - centerDepth) * 0.006f;

    [unroll]
    for (int y = -1; y <= 1; y++) {
        [unroll]
        for (int x = -1; x <= 1; x++) {
            float2 sampleUV = clamp(texCoord + float2(x, y) * pixelSize, pixelSize, 1.0 - pixelSize);
            float depth = TX_Depth.SampleLevel(SS_Point, sampleUV, 0).r;
            if (IsSkyDepth(depth) || abs(depth - centerDepth) > depthTolerance) {
                continue;
            }

            float2 velocity = CalculateVelocity(sampleUV, depth);
            float magnitudeSq = dot(velocity, velocity);
            if (magnitudeSq > bestMagnitudeSq * 1.25f) {
                bestVelocity = velocity;
                bestMagnitudeSq = magnitudeSq;
            }
        }
    }

    return bestVelocity;
}

float2 PSMain(PS_INPUT Input) : SV_TARGET {
    float2 texCoord = Input.vTexcoord;
    float2 pixelSize = 1.0 / Resolution;
    return DilateVelocity3x3(texCoord, pixelSize);
}
