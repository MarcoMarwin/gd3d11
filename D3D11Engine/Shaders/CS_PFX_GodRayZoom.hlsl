#ifndef VOLUMETRIC_GODRAYS
#define VOLUMETRIC_GODRAYS 0
#endif
#ifndef COMBINE_GODRAYS
#define COMBINE_GODRAYS 0
#endif
#if COMBINE_GODRAYS
Texture2D TX_VolumetricGodRays : register( t0 );
Texture2D TX_RadialGodRays : register( t1 );
RWTexture2D<float4> OutputTexture : register( u0 );
[numthreads(8, 8, 1)]
void CSCombine( uint3 DTid : SV_DispatchThreadID )
{
    uint2 outputSize;
    OutputTexture.GetDimensions( outputSize.x, outputSize.y );
    if ( DTid.x >= outputSize.x || DTid.y >= outputSize.y )
        return;
    uint2 volumetricSize;
    uint2 radialSize;
    TX_VolumetricGodRays.GetDimensions( volumetricSize.x, volumetricSize.y );
    TX_RadialGodRays.GetDimensions( radialSize.x, radialSize.y );
    if ( DTid.x >= volumetricSize.x || DTid.y >= volumetricSize.y || DTid.x >= radialSize.x || DTid.y >= radialSize.y )
    {
        OutputTexture[DTid.xy] = float4( 0.0f, 0.0f, 0.0f, 1.0f );
        return;
    }
    float3 volumetric = max( TX_VolumetricGodRays.Load( int3( DTid.xy, 0 ) ).rgb, 0.0f );
    float3 radial = max( TX_RadialGodRays.Load( int3( DTid.xy, 0 ) ).rgb, 0.0f );
    float3 radialContribution = radial * 0.35f;
    radialContribution = radialContribution / (1.0f + radialContribution);
    OutputTexture[DTid.xy] = float4( volumetric + radialContribution, 1.0f );
}
#elif VOLUMETRIC_GODRAYS
#include "DepthReconstruction.h"
#define MAX_CSM_CASCADES 4
cbuffer GodRayVolumetricConstantBuffer : register( b0 )
{
    float4 GRV_ProjParams;
    float4x4 GRV_InvView;
    float3 GRV_CameraPosition;
    float GRV_MaxDistance;
    float4x4 GRV_ShadowViewProj[MAX_CSM_CASCADES];
    float4 GRV_LightColor;
    float3 GRV_LightDirectionWS;
    float GRV_ShadowmapSize;
    float GRV_FogHeight;
    float GRV_HeightFalloff;
    float GRV_GlobalDensity;
    float GRV_WeightZNear;
    float GRV_WeightZFar;
    float GRV_RainFogHeight;
    float GRV_RainHeightFalloff;
    float GRV_RainGlobalDensity;
    float GRV_RainWeightZNear;
    float GRV_RainWeightZFar;
    float GRV_FogOverride;
    float GRV_RainWeight;
    float GRV_SunVisibility;
    float GRV_Strength;
    uint GRV_FrameIndex;
    uint GRV_NumCascades;
};
Texture2D TX_Depth : register( t0 );
Texture2DArray<float> TX_ShadowmapArray : register( t1 );
Texture2D TX_LowClouds : register( t2 );
SamplerComparisonState SS_Shadow : register( s0 );
SamplerState SS_Clamp : register( s1 );
RWTexture2D<float4> OutputTexture : register( u0 );

float GRV_Noise( uint2 p )
{
    return frac( 52.9829189f * frac( dot( float2( p ) + GRV_FrameIndex, float2( 0.06711056f, 0.00583715f ) ) ) );
}

float GRV_Density( float3 worldPos, float distanceToCamera, float fogHeight, float falloff, float density, float nearDistance, float farDistance )
{
    float heightDensity = exp( -max( worldPos.y - fogHeight, 0.0f ) * max( falloff, 0.000001f ) );
    float distanceWeight = saturate( (distanceToCamera - nearDistance) / max( farDistance - nearDistance, 1.0f ) );
    return max( density, 0.0f ) * heightDensity * distanceWeight;
}

float GRV_ShadowVisibility( float3 worldPos )
{
    [unroll]
    for ( uint cascade = 0; cascade < MAX_CSM_CASCADES; ++cascade )
    {
        if ( cascade >= GRV_NumCascades )
            break;
        float4 shadowPosition = mul( float4( worldPos, 1.0f ), GRV_ShadowViewProj[cascade] );
        if ( abs( shadowPosition.w ) <= 0.000001f )
            continue;
        shadowPosition.xyz /= shadowPosition.w;
        float2 uv = shadowPosition.xy * float2( 0.5f, -0.5f ) + 0.5f;
        if ( all( uv >= 0.0f ) && all( uv <= 1.0f ) && shadowPosition.z >= 0.0f && shadowPosition.z <= 1.0f )
            return TX_ShadowmapArray.SampleCmpLevelZero( SS_Shadow, float3( uv, cascade ), shadowPosition.z - 0.0005f );
    }
    return 1.0f;
}

[numthreads(8, 8, 1)]
void CSVolumetric( uint3 DTid : SV_DispatchThreadID )
{
    uint2 outputSize;
    OutputTexture.GetDimensions( outputSize.x, outputSize.y );
    if ( DTid.x >= outputSize.x || DTid.y >= outputSize.y )
        return;
    float2 uv = (float2( DTid.xy ) + 0.5f) / float2( outputSize );
    float depth = TX_Depth.SampleLevel( SS_Clamp, uv, 0 ).r;
    float3 viewPosition = depth > 0.0000001f
        ? ReconstructVSPositionFromDepthReverseZInfinite( depth, uv, GRV_ProjParams.xy )
        : normalize( ReconstructVSPositionFromDepthReverseZInfinite( 1.0f, uv, GRV_ProjParams.xy ) ) * GRV_MaxDistance;
    float viewDistance = min( length( viewPosition ), GRV_MaxDistance );
    float3 viewDirectionWS = normalize( mul( float4( normalize( viewPosition ), 0.0f ), GRV_InvView ).xyz );
    const uint sampleCount = 24;
    float stepLength = viewDistance / sampleCount;
    float jitter = GRV_Noise( DTid.xy );
    float transmittance = 1.0f;
    float3 scattering = 0.0f;
    float cosTheta = saturate( dot( viewDirectionWS, normalize( GRV_LightDirectionWS ) ) );
    float phase = 0.18f + 0.82f * pow( cosTheta, 8.0f );
    float cloudTransmission = 1.0f;
    uint cloudWidth;
    uint cloudHeight;
    TX_LowClouds.GetDimensions( cloudWidth, cloudHeight );
    if ( cloudWidth > 0 && cloudHeight > 0 )
    {
        float cloudAlpha = saturate( TX_LowClouds.SampleLevel( SS_Clamp, uv, 0 ).a );
        cloudTransmission = 1.0f - smoothstep( 0.03f, 0.62f, cloudAlpha );
    }
    [loop]
    for ( uint sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex )
    {
        float distanceToCamera = (sampleIndex + 0.5f + jitter) * stepLength;
        float3 worldPosition = GRV_CameraPosition + viewDirectionWS * distanceToCamera;
        float worldDensity = GRV_Density( worldPosition, distanceToCamera, GRV_FogHeight, GRV_HeightFalloff, GRV_GlobalDensity, GRV_WeightZNear, GRV_WeightZFar );
        float rainDensity = GRV_Density( worldPosition, distanceToCamera, GRV_RainFogHeight, GRV_RainHeightFalloff, GRV_RainGlobalDensity, GRV_RainWeightZNear, GRV_RainWeightZFar );
        float mediumDensity = lerp( worldDensity, rainDensity, saturate( GRV_RainWeight ) );
        float extinction = mediumDensity * stepLength;
        float sampleTransmittance = exp( -extinction );
        float visibility = GRV_ShadowVisibility( worldPosition );
        scattering += transmittance * (1.0f - sampleTransmittance) * visibility;
        transmittance *= sampleTransmittance;
        if ( transmittance < 0.01f )
            break;
    }
    float3 lightColor = max( GRV_LightColor.rgb, 0.0f );
    float3 result = scattering * lightColor * phase * GRV_SunVisibility * cloudTransmission * GRV_Strength;
    OutputTexture[DTid.xy] = float4( result, 1.0f );
}
#else
//--------------------------------------------------------------------------------------
// Compute Shader - God Ray Zoom (Radial Blur) Pass
// Reads mask texture, applies radial blur toward sun center, writes to UAV
//--------------------------------------------------------------------------------------
cbuffer GodRayZoomConstantBuffer : register( b0 )
{
    float GR_Decay;
    float GR_Weight;
    float2 GR_Center;
    float GR_Density;
    float3 GR_ColorMod;
};
SamplerState SS_Linear : register( s0 );
Texture2D TX_Texture0 : register( t0 ); // Mask from pass 1
RWTexture2D<float4> OutputTexture : register( u0 );
float InterleavedGradientNoise( float2 uv ) { float3 magic = float3( 0.06711056f, 0.00583715f, 52.9829189f ); return frac( magic.z * frac( dot( uv, magic.xy ) ) ); }
float LensFlareLuma(float3 color) { return dot(color, float3(0.2126f, 0.7152f, 0.0722f)); }
float LensFlareCircle(float2 uv, float2 position, float radius, float aspect) { float2 delta = uv - position; delta.x *= aspect; return 1.0f - smoothstep(radius * 0.35f, radius, length(delta)); }
float3 BuildLensFlare(float2 uv, float2 sunPosition, float aspect, float sunVisibility) { float2 screenCenter = float2(0.5f, 0.5f); float2 flareAxis = screenCenter - sunPosition; float lookAtSun = 1.0f - smoothstep(0.08f, 0.58f, length(flareAxis)); float flareStrength = saturate(sunVisibility) * lookAtSun * max(GR_Weight, 0.0f) * 0.35f; float sunGlow = LensFlareCircle(uv, sunPosition, 0.052f, aspect); float ghost1 = LensFlareCircle(uv, sunPosition + flareAxis * 0.72f, 0.022f, aspect); float ghost2 = LensFlareCircle(uv, sunPosition + flareAxis * 1.46f, 0.032f, aspect); float3 flareColor = 0.0f; flareColor += sunGlow * float3(1.00f, 0.90f, 0.72f) * 0.34f; flareColor += ghost1 * float3(0.62f, 0.72f, 0.88f) * 0.10f; flareColor += ghost2 * float3(0.72f, 0.68f, 0.62f) * 0.07f; return flareColor * flareStrength; }
[numthreads(8, 8, 1)]
void CSMain( uint3 DTid : SV_DispatchThreadID )
{
    uint2 texSize;
    OutputTexture.GetDimensions( texSize.x, texSize.y );
    if ( DTid.x >= texSize.x || DTid.y >= texSize.y )
        return;
    float2 texcoord = ( float2( DTid.xy ) + 0.5 ) / float2( texSize );
    const int NUM_SAMPLES = 64;
    float2 center = GR_Center;
    float3 color = 0;
    float illumDecay = 1.0f;
    float2 deltaTexCoord = texcoord - center;
    deltaTexCoord *= 1.0f / NUM_SAMPLES * GR_Density;
    float2 uv = texcoord;
    float jitter = InterleavedGradientNoise( float2( DTid.xy ) );
    uv -= deltaTexCoord * jitter;
    [unroll(64)]
    for ( int i = 0; i < NUM_SAMPLES; i++ )
    {
        uv -= deltaTexCoord;
        if ( uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f )
            continue;
        color += TX_Texture0.SampleLevel( SS_Linear, uv, 0 ).rgb * illumDecay * GR_Weight;
        illumDecay *= GR_Decay;
    }
    color /= NUM_SAMPLES;
    float aspect = texSize.y > 0 ? (float)texSize.x / (float)texSize.y : 1.0f;
    float2 texelSize = 1.0f / max( float2(texSize), float2(1.0f, 1.0f));
    float3 sunSample = 0.0f;
    if (center.x >= 0.0f && center.x <= 1.0f && center.y >= 0.0f && center.y <= 1.0f) { sunSample += TX_Texture0.SampleLevel(SS_Linear, center, 0).rgb; sunSample += TX_Texture0.SampleLevel(SS_Linear, center + float2(texelSize.x * 2.0f, 0.0f), 0).rgb; sunSample += TX_Texture0.SampleLevel(SS_Linear, center - float2(texelSize.x * 2.0f, 0.0f), 0).rgb; sunSample += TX_Texture0.SampleLevel(SS_Linear, center + float2(0.0f, texelSize.y * 2.0f), 0).rgb; sunSample += TX_Texture0.SampleLevel(SS_Linear, center - float2(0.0f, texelSize.y * 2.0f), 0).rgb; }
    float sunVisibility = saturate(LensFlareLuma(sunSample / 5.0f) * 3.0f);
    float3 lensFlare = BuildLensFlare(texcoord, center, aspect, sunVisibility);
    OutputTexture[DTid.xy] = float4( color * GR_ColorMod + lensFlare, 1.0f);
}
#endif
