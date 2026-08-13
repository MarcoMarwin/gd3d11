//--------------------------------------------------------------------------------------
// PointLightShadows.h - Shared pointlight shadow and lighting helpers
//--------------------------------------------------------------------------------------
#ifndef POINT_LIGHT_SHADOWS_H
#define POINT_LIGHT_SHADOWS_H

#if !defined(__cplusplus)

static const int PLS_SHADOW_BLUR_COUNT = 8;
static const float2 PLS_SHADOW_BLUR_OFFSETS[PLS_SHADOW_BLUR_COUNT] = {
    float2( 0.076849f, -0.078216f),
    float2(-0.165415f,  0.370808f),
    float2(-0.551062f, -0.407284f),
    float2( 0.449733f, -0.518174f),
    float2( 0.347526f,  0.730303f),
    float2(-0.840654f,  0.134261f),
    float2( 0.896791f,  0.038446f),
    float2(-0.258169f, -0.912648f)
};

float PLS_CalcBlinnPhongLighting( float3 N, float3 H )
{
    return saturate( dot( N, H ) );
}

float PLS_ComputeSpecMod( float3 diffuseColor )
{
    return pow( dot( float3( 0.333f, 0.333f, 0.333f ), diffuseColor ), 2 );
}

float PLS_ComputeRangeFalloff( float distance, float lightRange )
{
    float normalizedDist = saturate( 1.0f - (distance / lightRange) );
    return normalizedDist * (normalizedDist * 0.2f + 0.8f);
}

float PLS_ComputeBacklitVegetationMask(
    float3 diffuseColor,
    float alphaTestedMaterial,
    float twoSidedBacklitMaterial )
{
    float greenLeafMask = saturate( diffuseColor.g * 1.25f - diffuseColor.r * 0.45f - diffuseColor.b * 0.25f );
    float greenDominanceMask = saturate( (diffuseColor.g - max(diffuseColor.r, diffuseColor.b)) * 1.8f + 0.10f );
    return alphaTestedMaterial * (1.0f - saturate(twoSidedBacklitMaterial)) * max(greenLeafMask, greenDominanceMask);
}

float PLS_ComputeThinBacklitNdl(
    float3 lightDirVS,
    float3 normalVS,
    float twoSidedBacklitMaterial )
{
    float front = saturate( dot( lightDirVS, normalVS ) );
    float back = saturate( dot( lightDirVS, -normalVS ) );
    float backBase = back * 0.55f;
    float backTransmission = back * back * 0.25f;
    float twoSided = saturate( max( front, backBase ) + backTransmission );
    return lerp( front, twoSided, saturate(twoSidedBacklitMaterial) );
}


float PLS_ComputeBacklitTransmissionWeight(
    float3 lightDirVS,
    float3 normalVS,
    float3 viewDirVS,
    float shadowGate,
    float vegetationBacklitMask,
    float twoSidedBacklitMaterial,
    float sssEnabled,
    float sssIntensity,
    float lightScale )
{
    float back = saturate( dot( lightDirVS, -normalVS ) );
    float rim = pow( 1.0f - saturate( abs( dot( normalVS, viewDirVS ) ) ), 2.0f );
    float viewBacklight = saturate( dot( lightDirVS, -viewDirVS ) );
    float vegetationCore = viewBacklight * viewBacklight;
    float exceptionCore = back * lerp( 0.25f, 0.75f, rim );
    float materialCore = vegetationCore * saturate(vegetationBacklitMask)
        + exceptionCore * saturate(twoSidedBacklitMaterial);
    return saturate(materialCore * saturate(shadowGate) * saturate(sssEnabled) * saturate(sssIntensity) * lightScale);
}
float PLS_ComputePointLightNdl(
    float3 lightDirVS,
    float3 normalVS,
    float3 lightPosWorld,
    float3 wsPosition,
    float3 wsNormal )
{
    float ndl = saturate( dot( lightDirVS, normalVS ) );

    // A torch lying almost on the floor is physically above the visible flame,
    // but Gothic's light vob can sit close to the ground plane. Give upward
    // surfaces a small local wrap so they still receive warm light.
    float floorMask = smoothstep( 0.58f, 0.86f, wsNormal.y );
    float nearFloorLight = 1.0f - smoothstep( 18.0f, 120.0f, abs( lightPosWorld.y - wsPosition.y ) );
    float floorWrap = floorMask * nearFloorLight * 0.34f;
    return max( ndl, floorWrap );
}

float PLS_ApplyShadowDistanceFade( float finalShadow, float normalizedDist )
{
    // Keep fade-out for mostly lit samples, but preserve strong occlusion to avoid wall bleed.
    float shadowFade = smoothstep( 0.65f, 0.95f, normalizedDist );
    float fadeWeight = shadowFade * smoothstep( 0.45f, 0.90f, finalShadow );
    return lerp( finalShadow, 1.0f, fadeWeight );
}

float PLS_StableWorldNoise( float3 wsPosition, float3 lightPosWorld )
{
    // Camera-independent interleaved noise. Light-relative world space keeps
    // the pattern attached to the receiver and separates overlapping lights.
    float3 receiver = wsPosition - lightPosWorld;
    return frac( 52.9829189f * frac( dot(
        receiver, float3( 0.06711056f, 0.00583715f, 0.03127194f ) ) ) );
}

float PLS_ComputePointLightNdlBacklit(
    float3 lightDirVS,
    float3 normalVS,
    float3 lightPosWorld,
    float3 wsPosition,
    float3 wsNormal,
    float twoSidedBacklitMaterial,
    float sssEnabled )
{
    float ndl = PLS_ComputePointLightNdl( lightDirVS, normalVS, lightPosWorld, wsPosition, wsNormal );
    float thinNdl = PLS_ComputeThinBacklitNdl( lightDirVS, normalVS, twoSidedBacklitMaterial * saturate(sssEnabled) );
    return max( ndl, thinNdl );
}

float3 PLS_ComputePointLightLighting(
    float3 diffuseColor,
    float3 lightColor,
    float ndl,
    float falloff,
    float spec,
    float specIntensity,
    float specPower,
    float specMod )
{
    float3 specBare = pow( spec, specPower ) * specIntensity * lightColor * falloff;
    float3 specColored = lerp( specBare, specBare * diffuseColor, specMod );

    float3 color = saturate( falloff * ndl * lightColor );
    return color * diffuseColor + specColored;
}

float3 PLS_ComputePointLightLightingBacklit(
    float3 diffuseColor,
    float3 lightColor,
    float ndl,
    float falloff,
    float spec,
    float specIntensity,
    float specPower,
    float specMod,
    float3 lightDirVS,
    float3 normalVS,
    float3 viewDirVS,
    float vegetationBacklitMask,
    float twoSidedBacklitMaterial,
    float sssEnabled,
    float sssIntensity,
    float lightScale )
{
    float3 lighting = PLS_ComputePointLightLighting(
        diffuseColor, lightColor, ndl, falloff, spec, specIntensity, specPower, specMod );
    float transmission = PLS_ComputeBacklitTransmissionWeight(
        lightDirVS, normalVS, viewDirVS, 1.0f, vegetationBacklitMask,
        twoSidedBacklitMaterial, sssEnabled, sssIntensity, lightScale );
    float3 transmissionLighting = diffuseColor * lightColor * falloff * transmission;
    float3 additiveLighting = lighting + transmissionLighting;
    float3 boundedExceptionLighting = max(lighting, transmissionLighting);
    return lerp(additiveLighting, boundedExceptionLighting, saturate(twoSidedBacklitMaterial));
}

void PLS_PrepareShadowSampling(
    float3 wsPosition,
    float3 N, 
    float3 lightPosWorld,
    float lightRange,
    float shadowSoftness,
    out float3 dir,
    out float compareDistance,
    out float fixedBias,
    out float fixedBlurScale,
    out float3 right,
    out float3 up,
    out float sinA,
    out float cosA )
{
    float3 toPixelOriginal = wsPosition - lightPosWorld;
    float distOriginal = length( toPixelOriginal );
    float3 L = toPixelOriginal / distOriginal; 

    // Slope-Scaled Normal Bias
    float nDotL = saturate( dot( N, -L ) );
    float slopeScale = 1.0f - nDotL; 
    float normalOffsetScale = distOriginal * 0.02f * (slopeScale + 0.1f); 
    float3 biasedWsPosition = wsPosition + N * normalOffsetScale;

    // Recalculate vectors
    float3 toPixel = biasedWsPosition - lightPosWorld;
    dir = normalize( toPixel );

    float distance = length( toPixel );
    float zFar = lightRange * 2.0f; 
    compareDistance = distance / zFar;
    float distance01 = saturate( compareDistance );
    float depthCurve = distance01 * distance01;

    fixedBias = lerp( 0.002f, 0.008f, depthCurve );

    float baseBlur = lerp( 0.02f, 0.08f, depthCurve );
    fixedBlurScale = baseBlur * clamp(shadowSoftness, 0.2f, 8.0f);

    up = abs( dir.y ) < 0.999f ? float3( 0, 1, 0 ) : float3( 1, 0, 0 );
    right = normalize( cross( up, dir ) );
    up = cross( dir, right );

    // Rotate the existing eight samples instead of adding more. The stable
    // world-space phase breaks up visible 1/8 coverage bands without temporal
    // shimmer under camera motion.
    float kernelAngle = PLS_StableWorldNoise( wsPosition, lightPosWorld )
        * 6.28318530718f;
    sincos( kernelAngle, sinA, cosA );
}

float PLS_SampleShadowCube(
    TextureCube shadowCube,
    SamplerComparisonState samplerState,
    float3 wsPosition,
    float3 N, 
    float3 lightPosWorld,
    float lightRange,
    float shadowSoftness )
{
    float3 dir;
    float compareDistance;
    float fixedBias;
    float fixedBlurScale;
    float3 right;
    float3 up;
    float sinA;
    float cosA;

    PLS_PrepareShadowSampling(
        wsPosition, N, lightPosWorld, lightRange, shadowSoftness,
        dir, compareDistance, fixedBias, fixedBlurScale,
        right, up, sinA, cosA );

    float shd = 0;
    [unroll] for ( int i = 0; i < PLS_SHADOW_BLUR_COUNT; i++ )
    {
        float2 kernel = PLS_SHADOW_BLUR_OFFSETS[i];
        float2 rotatedKernel = float2( kernel.x * cosA - kernel.y * sinA, kernel.x * sinA + kernel.y * cosA );
        float3 perturbedDir = normalize( dir + (right * rotatedKernel.x + up * rotatedKernel.y) * fixedBlurScale );

        shd += shadowCube.SampleCmpLevelZero(
            samplerState, perturbedDir, compareDistance - fixedBias );
    }

    float finalShadow = shd / PLS_SHADOW_BLUR_COUNT;

    // Shadow Distance Fading
    // Calculate how far we are through the light's actual range (0.0 to 1.0)
    float distanceToLight = length(wsPosition - lightPosWorld);
    float normalizedDist = saturate(distanceToLight / lightRange);
    
    return PLS_ApplyShadowDistanceFade( finalShadow, normalizedDist );
}

float PLS_SampleShadowCubeArray(
    TextureCubeArray shadowCubeArray,
    SamplerComparisonState samplerState,
    float3 wsPosition,
    float3 N, 
    float3 lightPosWorld,
    float lightRange,
    int cubeIndex,
    float shadowSoftness )
{
    float3 dir;
    float compareDistance;
    float fixedBias;
    float fixedBlurScale;
    float3 right;
    float3 up;
    float sinA;
    float cosA;

    PLS_PrepareShadowSampling(
        wsPosition, N, lightPosWorld, lightRange, shadowSoftness,
        dir, compareDistance, fixedBias, fixedBlurScale,
        right, up, sinA, cosA );

    float shd = 0;
    [unroll] for ( int i = 0; i < PLS_SHADOW_BLUR_COUNT; i++ )
    {
        float2 kernel = PLS_SHADOW_BLUR_OFFSETS[i];
        float2 rotatedKernel = float2( kernel.x * cosA - kernel.y * sinA, kernel.x * sinA + kernel.y * cosA );
        float3 perturbedDir = normalize( dir + (right * rotatedKernel.x + up * rotatedKernel.y) * fixedBlurScale );
        float4 sampleCoord = float4( perturbedDir, (float)cubeIndex );

        shd += shadowCubeArray.SampleCmpLevelZero(
            samplerState, sampleCoord, compareDistance - fixedBias );
    }

    float finalShadow = shd / PLS_SHADOW_BLUR_COUNT;

    // Shadow Distance Fading
    float distanceToLight = length(wsPosition - lightPosWorld);
    float normalizedDist = saturate(distanceToLight / lightRange);
    
    return PLS_ApplyShadowDistanceFade( finalShadow, normalizedDist );
}

#endif // !defined(__cplusplus)

#endif // POINT_LIGHT_SHADOWS_H
