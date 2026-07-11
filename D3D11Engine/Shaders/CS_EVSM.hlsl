// True EVSM2 preprocessing for cascaded world shadows.
// Pass 1 converts depth to stable 32-bit exponential moments and filters horizontally.
// Pass 2 filters vertically into the cascade's distance-scaled moment texture.

cbuffer EVSMFilterConstants : register(b0)
{
    uint EVSM_SourceSize;
    uint EVSM_DestinationSize;
    uint EVSM_CascadeIndex;
    float EVSM_BlurRadius;
};

SamplerState SS_PointClamp : register(s0);
SamplerState SS_LinearClamp : register(s1);
Texture2DArray<float> TX_ShadowDepth : register(t0);
Texture2D<float2> TX_EVSMTemp : register(t1);
RWTexture2D<float2> RW_EVSMTemp : register(u0);
RWTexture2D<float2> RW_EVSMOutput : register(u1);

static const float EVSMExponent = 10.0f;
static const float GaussianWeights[5] =
{
    0.2270270270f,
    0.1945945946f,
    0.1216216216f,
    0.0540540541f,
    0.0162162162f
};

float2 DepthToEVSMMoments(float depth)
{
    depth = saturate(depth);
    float warpedDepth = exp(EVSMExponent * depth);
    return float2(warpedDepth, warpedDepth * warpedDepth);
}

float SampleCascadeDepth(float2 uv)
{
    if (any(uv < 0.0f) || any(uv > 1.0f))
        return 1.0f;

    return TX_ShadowDepth.SampleLevel(
        SS_PointClamp, float3(uv, (float)EVSM_CascadeIndex), 0).r;
}

float2 SampleDepthMoments(float2 uv)
{
    float2 center = DepthToEVSMMoments(SampleCascadeDepth(uv));
    if (EVSM_SourceSize <= EVSM_DestinationSize)
        return center;

    float2 subPixelOffset = 0.25f / (float)EVSM_DestinationSize;
    return 0.25f * (
        DepthToEVSMMoments(SampleCascadeDepth(uv + float2(-subPixelOffset.x, -subPixelOffset.y))) +
        DepthToEVSMMoments(SampleCascadeDepth(uv + float2( subPixelOffset.x, -subPixelOffset.y))) +
        DepthToEVSMMoments(SampleCascadeDepth(uv + float2(-subPixelOffset.x,  subPixelOffset.y))) +
        DepthToEVSMMoments(SampleCascadeDepth(uv + float2( subPixelOffset.x,  subPixelOffset.y))));
}

float2 SampleTemporaryMoments(float2 pixel)
{
    if (any(pixel < 0.0f) || any(pixel >= (float)EVSM_DestinationSize))
        return DepthToEVSMMoments(1.0f);

    float2 uv = (pixel + 0.5f) / (float)EVSM_SourceSize;
    return TX_EVSMTemp.SampleLevel(SS_LinearClamp, uv, 0);
}

[numthreads(8, 8, 1)]
void CSConvertHorizontal(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadId.xy;
    if (any(pixel >= EVSM_DestinationSize))
        return;

    float2 uv = (float2(pixel) + 0.5f) / (float)EVSM_DestinationSize;
    float2 texel = float2(1.0f / (float)EVSM_DestinationSize, 0.0f);
    float2 centerMoments = SampleDepthMoments(uv);
    if (EVSM_BlurRadius <= 0.001f)
    {
        RW_EVSMTemp[pixel] = centerMoments;
        return;
    }

    float2 moments = centerMoments * GaussianWeights[0];

    [unroll]
    for (int i = 1; i < 5; ++i)
    {
        float2 offset = texel * ((float)i * EVSM_BlurRadius);
        moments += SampleDepthMoments(uv - offset) * GaussianWeights[i];
        moments += SampleDepthMoments(uv + offset) * GaussianWeights[i];
    }

    RW_EVSMTemp[pixel] = moments;
}

[numthreads(8, 8, 1)]
void CSVertical(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadId.xy;
    if (any(pixel >= EVSM_DestinationSize))
        return;

    float2 moments = SampleTemporaryMoments(float2(pixel)) * GaussianWeights[0];

    [unroll]
    for (int i = 1; i < 5; ++i)
    {
        float offset = (float)i * EVSM_BlurRadius;
        moments += SampleTemporaryMoments(float2(pixel) + float2(0.0f, -offset)) * GaussianWeights[i];
        moments += SampleTemporaryMoments(float2(pixel) + float2(0.0f, offset)) * GaussianWeights[i];
    }

    RW_EVSMOutput[pixel] = moments;
}