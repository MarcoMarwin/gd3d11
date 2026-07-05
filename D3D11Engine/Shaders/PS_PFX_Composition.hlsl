//--------------------------------------------------------------------------------------
// PostFX Composition Uber Shader
// Merges atmospheric and screen-space lighting effects into a single full-screen pass.
// Permutation macros select HeightFog, GodRays, contact shadows and SSGI.
//--------------------------------------------------------------------------------------

#if COMPOSE_HEIGHTFOG || COMPOSE_CONTACT_SHADOWS || COMPOSE_SSGI
#include <AtmosphericScattering.h>
#endif
#if COMPOSE_CONTACT_SHADOWS || COMPOSE_SSGI
#include "DS_Defines.h"
#endif
#if COMPOSE_HEIGHTFOG
#include "DepthReconstruction.h"
#endif

//--------------------------------------------------------------------------------------
// Constant Buffers
//--------------------------------------------------------------------------------------
#if COMPOSE_HEIGHTFOG
cbuffer PFXBuffer : register( b0 )
{
    float4 HF_ProjParams;
    matrix HF_InvView;
    float3 HF_CameraPosition;
    float HF_FogHeight;

    float HF_HeightFalloff;
    float HF_GlobalDensity;
    float HF_WeightZNear;
    float HF_WeightZFar;

    float3 HF_FogColorMod;
    float HF_pad2;

    float2 HF_ProjAB;
    float2 HF_Pad3;
};

#endif

#if COMPOSE_HEIGHTFOG || COMPOSE_CONTACT_SHADOWS || COMPOSE_SSGI
cbuffer CompositionControl : register( b2 )
{
    float CC_HeightFogEnabled;
    float CC_Pad0;
    float2 CC_InvResolution;

    float4 CC_ProjParams;
    matrix CC_Projection;

    float3 CC_LightDirectionVS;
    float CC_Pad;
};
#endif

//--------------------------------------------------------------------------------------
// Textures and Samplers
//--------------------------------------------------------------------------------------
SamplerState SS_Linear : register( s0 );

Texture2D TX_Backbuffer : register( t0 );

#if COMPOSE_GODRAYS
Texture2D TX_GodRays : register( t1 );
#endif

#if COMPOSE_HEIGHTFOG || COMPOSE_CONTACT_SHADOWS || COMPOSE_SSGI
Texture2D TX_Depth : register( t2 );
#endif

#if COMPOSE_CONTACT_SHADOWS || COMPOSE_SSGI
Texture2D TX_Normals : register( t3 );
Texture2D TX_WaterMask : register( t4 );
#endif

//--------------------------------------------------------------------------------------
// HeightFog helpers (inlined from PS_PFX_Heightfog.hlsl)
//--------------------------------------------------------------------------------------
#if COMPOSE_HEIGHTFOG
float3 VSPositionFromDepth( float depth, float2 vTexCoord )
{
    return ReconstructVSPositionFromDepthReverseZInfinite( depth, vTexCoord, HF_ProjParams.xy );
}

float ComputeVolumetricFog( float3 cameraToWorldPos, float3 posOriginal )
{
    float cVolFogHeightDensityAtViewer = exp( -HF_HeightFalloff );

    float lenOrig = length( posOriginal - HF_CameraPosition );
    float len = length( cameraToWorldPos );
    float fogInt = len * cVolFogHeightDensityAtViewer;
    const float cSlopeThreshold = 0.01;

    float w = saturate( ( lenOrig - HF_WeightZNear ) / ( HF_WeightZFar - HF_WeightZNear ) );

    if ( abs( cameraToWorldPos.y ) > cSlopeThreshold )
    {
        float t = HF_HeightFalloff * cameraToWorldPos.y * w;
        fogInt *= ( abs( t ) > 0.0001 ? ( ( 1.0 - exp( -t ) ) / t ) : 1.0 );
    }

    return exp( -HF_GlobalDensity * w * fogInt );
}

float FogDither(float2 pixelPosition)
{
    float n1 = frac(52.9829189f * frac(dot(pixelPosition, float2(0.06711056f, 0.00583715f))));
    float n2 = frac(52.9829189f * frac(dot(pixelPosition + 37.17f, float2(0.00583715f, 0.06711056f))));
    return n1 + n2 - 1.0f;
}

float4 ComputeHeightFog( float2 texcoord, float2 pixelPosition )
{
    float expDepth = TX_Depth.Sample( SS_Linear, texcoord ).r;
    float skyPixel = 1.0f - step(0.00001f, expDepth);
    float3 position = VSPositionFromDepth( expDepth, texcoord );
    position = mul( float4( position, 1 ), HF_InvView ).xyz;
    float3 posOriginal = position;
    position -= HF_CameraPosition;
    position.y -= HF_FogHeight;

    float fog = 1.0f - ComputeVolumetricFog( position, posOriginal );
    float fogDistance = length(posOriginal - HF_CameraPosition);
    float stableFadeEnd = max(HF_WeightZFar, 1000.0f);
    float stableFadeStart = max(HF_WeightZNear, stableFadeEnd * 0.82f);
    float stableWorldFade = smoothstep(stableFadeStart, stableFadeEnd, fogDistance);
    float activeWeatherFog = saturate(AC_RainFXWeight);
	float nightTimeBlend = smoothstep(0.0f, 1.0f, saturate(-AC_LightPos.y * 4.0f))
		* saturate(AC_EnableNightAtmosphere);
    float weatherFog = max(fog, stableWorldFade) * activeWeatherFog;
    float dryNightFog = fog * nightTimeBlend * (1.0f - activeWeatherFog);
    fog = max(weatherFog, dryNightFog);
    float fogGradientWeight = saturate(fog * (1.0f - fog) * 4.0f);
    float fogGradientDither = FogDither(pixelPosition) * nightTimeBlend * (2.0f / 255.0f);
    float ditheredFog = saturate(fog + fogGradientDither * fogGradientWeight);
    float3 color = ApplyAtmosphericScatteringGround( position, HF_FogColorMod, true, false );
	float nightFogBrightness = lerp(1.0f, max(0.0f, AC_NightFogBrightness), saturate(AC_EnableNightAtmosphere));
	float3 nightFogColor = float3(0.12f, 0.18f, 0.27f) * nightFogBrightness;
	color = lerp(color, nightFogColor, nightTimeBlend);

	float dayDarknessFactor = max(1.0f, 2.0f - max(0.0f, AC_LightPos.y));
	float darknessFactor = lerp(dayDarknessFactor, 2.5f, nightTimeBlend);
	float maxFogOpacity = lerp(1.0f, 0.85f, nightTimeBlend);
	float skyRainFogAttenuation = lerp(1.0f, 0.42f, skyPixel * activeWeatherFog);

	return float4(saturate(color / darknessFactor), ditheredFog * maxFogOpacity * skyRainFogAttenuation);
}
#endif

//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
struct PS_INPUT
{
    float2 vTexcoord  : TEXCOORD0;
    float3 vEyeRay    : TEXCOORD1;
    float4 vPosition  : SV_POSITION;
};


#if COMPOSE_CONTACT_SHADOWS || COMPOSE_SSGI
float GetDepthRaw(float2 uv)
{
    return TX_Depth.SampleLevel(SS_Linear, saturate(uv), 0).r;
}

float IsGeometryPixel(float depth)
{
    return step(0.000001f, depth);
}

float3 ReconstructViewPosition(float2 uv, float depth)
{
    float viewZ = CC_ProjParams.z / (depth - CC_ProjParams.w);
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    return float3(ndc.x * viewZ * CC_ProjParams.x, ndc.y * viewZ * CC_ProjParams.y, viewZ);
}

float3 GetViewNormal(float2 uv)
{
    return DecodeNormalGBuffer(TX_Normals.SampleLevel(SS_Linear, saturate(uv), 0).xy);
}

float GetWaterMask(float2 uv)
{
    return TX_WaterMask.SampleLevel(SS_Linear, saturate(uv), 0).r;
}

float2 ProjectViewPosition(float3 viewPosition, out float valid)
{
    float4 clip = mul(float4(viewPosition, 1.0f), CC_Projection);
    valid = step(0.001f, clip.w);
    float2 uv = clip.xy / max(clip.w, 0.001f) * float2(0.5f, -0.5f) + 0.5f;
    valid *= step(0.0f, uv.x) * step(uv.x, 1.0f) * step(0.0f, uv.y) * step(uv.y, 1.0f);
    return uv;
}

float InterleavedGradientNoise(float2 pixel)
{
    return frac(52.9829189f * frac(dot(pixel, float2(0.06711056f, 0.00583715f))));
}

bool TraceViewSpaceRay(
    float3 rayOrigin,
    float3 rayDirection,
    float maxDistance,
    int stepCount,
    float jitter,
    float minThickness,
    float thicknessScale,
    float maxThickness,
    out float2 hitUV,
    out float hitDistance)
{
    hitUV = 0.0f;
    hitDistance = maxDistance;
    float previousDelta = -1.0f;

    [loop]
    for (int stepIndex = 0; stepIndex < 12; ++stepIndex)
    {
        if (stepIndex >= stepCount)
            break;

        float normalizedStep = ((float)stepIndex + 1.0f + jitter * 0.35f) / ((float)stepCount + 0.35f);
        float travel = maxDistance * normalizedStep * normalizedStep;
        float3 rayPosition = rayOrigin + rayDirection * travel;
        if (rayPosition.z <= 1.0f)
            break;

        float valid;
        float2 rayUV = ProjectViewPosition(rayPosition, valid);
        if (valid < 0.5f)
            break;

        if (GetWaterMask(rayUV) > 0.05f)
        {
            previousDelta = -1.0f;
            continue;
        }

        float sceneDepth = GetDepthRaw(rayUV);
        if (IsGeometryPixel(sceneDepth) < 0.5f)
        {
            previousDelta = -1.0f;
            continue;
        }

        float sceneZ = ReconstructViewPosition(rayUV, sceneDepth).z;
        float depthDelta = rayPosition.z - sceneZ;
        float thickness = clamp(travel * thicknessScale, minThickness, maxThickness);
        bool crossedSurface = depthDelta > minThickness * 0.5f && depthDelta < thickness;
        bool crossedBetweenSteps = previousDelta < -minThickness * 0.5f && depthDelta >= 0.0f && depthDelta < thickness;
        if (crossedSurface || crossedBetweenSteps)
        {
            hitUV = rayUV;
            hitDistance = travel;
            return true;
        }
        previousDelta = depthDelta;
    }
    return false;
}
#endif

#if COMPOSE_CONTACT_SHADOWS
float ComputeContactShadow(float2 uv, float centerDepth)
{
    if (IsGeometryPixel(centerDepth) < 0.5f || GetWaterMask(uv) > 0.05f)
        return 1.0f;

    float3 viewPosition = ReconstructViewPosition(uv, centerDepth);
    float3 viewNormal = GetViewNormal(uv);
    float3 directionToLight = normalize(CC_LightDirectionVS);
    float surfaceFacing = saturate(dot(viewNormal, directionToLight));
    if (surfaceFacing <= 0.001f)
        return 1.0f;

    float jitter = InterleavedGradientNoise(floor(uv / CC_InvResolution));
    float maxDistance = clamp(viewPosition.z * 0.012f, 35.0f, 220.0f);
    float2 hitUV;
    float hitDistance;
    bool hit = TraceViewSpaceRay(
        viewPosition + viewNormal * 4.0f,
        directionToLight,
        maxDistance,
        10,
        jitter,
        2.5f,
        0.035f,
        18.0f,
        hitUV,
        hitDistance);

    if (!hit)
        return 1.0f;

    float distanceFade = 1.0f - smoothstep(maxDistance * 0.22f, maxDistance, hitDistance);
    float shadow = surfaceFacing * distanceFade * saturate(AC_ContactShadowStrength * 0.5f) * 0.42f;
    return 1.0f - saturate(shadow);
}
#endif

#if COMPOSE_SSGI
float3 ComputeScreenSpaceGILight(float2 uv, float centerDepth, float3 baseColor)
{
    if (IsGeometryPixel(centerDepth) < 0.5f || GetWaterMask(uv) > 0.05f)
        return 0.0f;

    float3 viewPosition = ReconstructViewPosition(uv, centerDepth);
    float3 viewNormal = GetViewNormal(uv);
    float3 helperAxis = abs(viewNormal.z) < 0.98f ? float3(0.0f, 0.0f, 1.0f) : float3(0.0f, 1.0f, 0.0f);
    float3 tangent = normalize(cross(helperAxis, viewNormal));
    float3 bitangent = normalize(cross(viewNormal, tangent));
    float maxDistance = clamp(viewPosition.z * 0.10f, 450.0f, 2200.0f);
    float3 indirectRadiance = 0.0f;
    float weightSum = 0.0f;
    int hitCount = 0;

    [unroll]
    for (int rayIndex = 0; rayIndex < 8; ++rayIndex)
    {
        float sequence = frac(0.23f + (float)rayIndex * 0.6180339f);
        float cosTheta = lerp(0.28f, 0.84f, sequence);
        float sinTheta = sqrt(saturate(1.0f - cosTheta * cosTheta));
        float phi = (float)rayIndex * 2.3999632f;
        float3 rayDirection = normalize(
            tangent * (cos(phi) * sinTheta) +
            bitangent * (sin(phi) * sinTheta) +
            viewNormal * cosTheta);

        float2 hitUV;
        float hitDistance;
        if (TraceViewSpaceRay(
                viewPosition + viewNormal * 7.0f,
                rayDirection,
                maxDistance,
                10,
                sequence,
                6.0f,
                0.055f,
                42.0f,
                hitUV,
                hitDistance))
        {
            float3 hitNormal = GetViewNormal(hitUV);
            float receiverCosine = saturate(dot(viewNormal, rayDirection));
            float emitterCosine = saturate(dot(hitNormal, -rayDirection));
            float distanceWeight = 1.0f / (1.0f + 3.5f * hitDistance / maxDistance);
            float sampleWeight = receiverCosine * emitterCosine * distanceWeight;
            float3 sourceRadiance = TX_Backbuffer.SampleLevel(SS_Linear, hitUV, 0).rgb;
            float sourceLuma = dot(sourceRadiance, float3(0.2126f, 0.7152f, 0.0722f));
            sourceRadiance = min(sourceRadiance / (1.0f + max(0.0f, sourceLuma - 1.0f) * 0.65f), float3(4.0f, 4.0f, 4.0f));
            indirectRadiance += sourceRadiance * sampleWeight;
            weightSum += sampleWeight;
            hitCount++;
        }
    }

    if (hitCount < 2 || weightSum <= 0.001f)
        return 0.0f;

    float baseLuma = dot(baseColor, float3(0.2126f, 0.7152f, 0.0722f));
    float energyControl = rcp(1.0f + baseLuma * 0.22f);
    return (indirectRadiance / weightSum) * min(AC_ScreenSpaceGIStrength, 2.0f) * 0.20f * energyControl;
}
#endif

//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------
float4 PSMain( PS_INPUT Input ) : SV_TARGET
{
    float4 color = TX_Backbuffer.Sample( SS_Linear, Input.vTexcoord );

    // Composition order: HeightFog and screen-space lighting, then GodRays.

#if COMPOSE_CONTACT_SHADOWS || COMPOSE_SSGI
    float compositionDepth = GetDepthRaw(Input.vTexcoord);
#endif

#if COMPOSE_CONTACT_SHADOWS
    color.rgb *= ComputeContactShadow(Input.vTexcoord, compositionDepth);
#endif

#if COMPOSE_SSGI
    color.rgb += ComputeScreenSpaceGILight(Input.vTexcoord, compositionDepth, color.rgb);
#endif

#if COMPOSE_HEIGHTFOG
    [branch] if ( CC_HeightFogEnabled > 0.5f )
    {
        float4 fog = ComputeHeightFog( Input.vTexcoord, Input.vPosition.xy );
        color.rgb = lerp( color.rgb, fog.rgb, fog.a );
        float nightTimeBlend = smoothstep(0.0f, 1.0f, saturate(-AC_LightPos.y * 4.0f));
        float ditherStrength = lerp(1.0f, 2.5f, nightTimeBlend) / 255.0f;
        color.rgb = saturate(color.rgb + FogDither(Input.vPosition.xy) * fog.a * ditherStrength);
    }
#endif

#if COMPOSE_GODRAYS
    float3 godrays = TX_GodRays.Sample( SS_Linear, Input.vTexcoord ).rgb;
    color.rgb += godrays;
#endif


    return color;
}
