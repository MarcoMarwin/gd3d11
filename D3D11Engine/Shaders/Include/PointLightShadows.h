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

static const int PLS_FAR_PCF_COUNT = 4;
static const float2 PLS_FAR_PCF_OFFSETS[PLS_FAR_PCF_COUNT] = {
    float2(-0.5f, -0.5f),
    float2( 0.5f, -0.5f),
    float2(-0.5f,  0.5f),
    float2( 0.5f,  0.5f)
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
    float safeDistOriginal = max( distOriginal, 1.0e-4f );
    float3 L = toPixelOriginal / safeDistOriginal;

    // Slope-Scaled Normal Bias
    float nDotL = saturate( dot( N, -L ) );
    float slopeScale = 1.0f - nDotL; 
    // Keep the receiver offset below the size at which interpolated skeletal
    // normals move the projected shadow independently on adjacent triangles.
    // A smaller slope term plus a range-relative cap still prevents acne on
    // static walls without producing polygon-shaped patches on characters.
    float normalOffsetScale = distOriginal * 0.006f * (slopeScale + 0.15f);
    normalOffsetScale = min( normalOffsetScale, max( lightRange, 0.0f ) * 0.002f );
    float3 biasedWsPosition = wsPosition + N * normalOffsetScale;

    // Recalculate vectors
    float3 toPixel = biasedWsPosition - lightPosWorld;
    float distance = length( toPixel );
    dir = distance > 1.0e-4f ? toPixel / distance : float3( 0.0f, 1.0f, 0.0f );

    float zFar = max( lightRange * 2.0f, 1.0e-4f );
    compareDistance = distance / zFar;
    float distance01 = saturate( compareDistance );
    float depthCurve = distance01 * distance01;

    fixedBias = lerp( 0.002f, 0.008f, depthCurve );

    // ShadowSoftness represents the angular radius of the point-light source.
    // The actual filter radius is derived from blocker/receiver separation below.
    // Keep the near-light radius large enough to span the 128px Medium cube
    // footprint; otherwise all eight taps collapse into the same comparison.
    float sourceAngularRadius = lerp( 0.010f, 0.022f, distance01 );
    // A negative value is an internal quality marker for distant lights. Its
    // magnitude remains the user-selected softness; only the sampling method
    // changes from PCSS to the cheaper stable PCF path below.
    fixedBlurScale = sourceAngularRadius * clamp( abs( shadowSoftness ), 0.0f, 4.0f );

    up = abs( dir.y ) < 0.999f ? float3( 0, 1, 0 ) : float3( 1, 0, 0 );
    right = normalize( cross( up, dir ) );
    up = cross( dir, right );

    sinA = 0.0f;
    cosA = 1.0f;
}

float PLS_SampleShadowCube(
    TextureCube shadowCube,
    SamplerState linearSampler,
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

    if ( shadowSoftness <= 0.01f )
    {
        if ( shadowSoftness < -0.01f )
        {
            float farShadow = 0.0f;
            float filterRadius = fixedBlurScale * 0.70f;
            [unroll] for ( int i = 0; i < PLS_FAR_PCF_COUNT; ++i )
            {
                float2 kernel = PLS_FAR_PCF_OFFSETS[i];
                float3 sampleDir = normalize(
                    dir + (right * kernel.x + up * kernel.y) * filterRadius );
                farShadow += shadowCube.SampleCmpLevelZero(
                    samplerState, sampleDir, compareDistance - fixedBias );
            }
            float normalizedDist = saturate( length( wsPosition - lightPosWorld ) / lightRange );
            return PLS_ApplyShadowDistanceFade(
                farShadow / PLS_FAR_PCF_COUNT, normalizedDist );
        }

        float hardShadow = shadowCube.SampleCmpLevelZero(
            samplerState, dir, compareDistance - fixedBias );
        float normalizedDist = saturate( length( wsPosition - lightPosWorld ) / lightRange );
        return PLS_ApplyShadowDistanceFade( hardShadow, normalizedDist );
    }

    float receiverDepth = compareDistance - fixedBias;
    float blockerDepthSum = 0.0f;
    float blockerCount = 0.0f;
    float centerBlockerDepth = shadowCube.SampleLevel( linearSampler, dir, 0.0f ).r;
    if ( centerBlockerDepth < receiverDepth )
    {
        blockerDepthSum = centerBlockerDepth;
        blockerCount = 1.0f;
    }
    float searchRadius = fixedBlurScale * 0.40f;
    [unroll] for ( int blockerIndex = 0; blockerIndex < 4; blockerIndex++ )
    {
        float2 kernel = PLS_SHADOW_BLUR_OFFSETS[blockerIndex];
        float3 searchDir = normalize( dir + (right * kernel.x + up * kernel.y) * searchRadius );
        float blockerDepth = shadowCube.SampleLevel( linearSampler, searchDir, 0.0f ).r;
        if ( blockerDepth < receiverDepth )
        {
            blockerDepthSum += blockerDepth;
            blockerCount += 1.0f;
        }
    }

    // No occluder in the source footprint: preserve the unshadowed light exactly.
    if ( blockerCount <= 0.0f )
        return 1.0f;

    float averageBlockerDepth = blockerDepthSum / blockerCount;
    float separation = max( receiverDepth - averageBlockerDepth, 0.0f );
    float penumbraRatio = separation / max( averageBlockerDepth, 0.02f );
    // A purely geometric PCSS radius collapses below one cubemap texel at
    // contact, particularly with the 128px Medium shadow tier. Retain a small
    // source-size-dependent contact footprint so Shadow Softness still affects
    // nearby edges without modifying light attenuation or depth comparison.
    // Sparse blocker counts must not switch the full kernel radius abruptly.
    // Keep PCSS contact hardening, but confine it to a smooth, stable range;
    // the user-controlled source radius still determines the overall softness.
    float penumbraWeight = smoothstep( 0.0f, 0.75f, penumbraRatio );
    float filterRadius = fixedBlurScale * lerp( 0.55f, 0.85f, penumbraWeight );

    float shd = 0;
    [unroll] for ( int i = 0; i < PLS_SHADOW_BLUR_COUNT; i++ )
    {
        float2 kernel = PLS_SHADOW_BLUR_OFFSETS[i];
        float2 rotatedKernel = float2( kernel.x * cosA - kernel.y * sinA, kernel.x * sinA + kernel.y * cosA );
        float3 perturbedDir = normalize( dir + (right * rotatedKernel.x + up * rotatedKernel.y) * filterRadius );

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
    SamplerState linearSampler,
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

    if ( shadowSoftness <= 0.01f )
    {
        if ( shadowSoftness < -0.01f )
        {
            float farShadow = 0.0f;
            float filterRadius = fixedBlurScale * 0.70f;
            [unroll] for ( int i = 0; i < PLS_FAR_PCF_COUNT; ++i )
            {
                float2 kernel = PLS_FAR_PCF_OFFSETS[i];
                float3 sampleDir = normalize(
                    dir + (right * kernel.x + up * kernel.y) * filterRadius );
                float4 sampleCoord = float4( sampleDir, (float)cubeIndex );
                farShadow += shadowCubeArray.SampleCmpLevelZero(
                    samplerState, sampleCoord, compareDistance - fixedBias );
            }
            float normalizedDist = saturate( length( wsPosition - lightPosWorld ) / lightRange );
            return PLS_ApplyShadowDistanceFade(
                farShadow / PLS_FAR_PCF_COUNT, normalizedDist );
        }

        float4 sampleCoord = float4( dir, (float)cubeIndex );
        float hardShadow = shadowCubeArray.SampleCmpLevelZero(
            samplerState, sampleCoord, compareDistance - fixedBias );
        float normalizedDist = saturate( length( wsPosition - lightPosWorld ) / lightRange );
        return PLS_ApplyShadowDistanceFade( hardShadow, normalizedDist );
    }

    float receiverDepth = compareDistance - fixedBias;
    float blockerDepthSum = 0.0f;
    float blockerCount = 0.0f;
    float4 centerCoord = float4( dir, (float)cubeIndex );
    float centerBlockerDepth = shadowCubeArray.SampleLevel( linearSampler, centerCoord, 0.0f ).r;
    if ( centerBlockerDepth < receiverDepth )
    {
        blockerDepthSum = centerBlockerDepth;
        blockerCount = 1.0f;
    }
    float searchRadius = fixedBlurScale * 0.40f;
    [unroll] for ( int blockerIndex = 0; blockerIndex < 4; blockerIndex++ )
    {
        float2 kernel = PLS_SHADOW_BLUR_OFFSETS[blockerIndex];
        float3 searchDir = normalize( dir + (right * kernel.x + up * kernel.y) * searchRadius );
        float4 searchCoord = float4( searchDir, (float)cubeIndex );
        float blockerDepth = shadowCubeArray.SampleLevel( linearSampler, searchCoord, 0.0f ).r;
        if ( blockerDepth < receiverDepth )
        {
            blockerDepthSum += blockerDepth;
            blockerCount += 1.0f;
        }
    }

    if ( blockerCount <= 0.0f )
        return 1.0f;

    float averageBlockerDepth = blockerDepthSum / blockerCount;
    float separation = max( receiverDepth - averageBlockerDepth, 0.0f );
    float penumbraRatio = separation / max( averageBlockerDepth, 0.02f );
    float penumbraWeight = smoothstep( 0.0f, 0.75f, penumbraRatio );
    float filterRadius = fixedBlurScale * lerp( 0.55f, 0.85f, penumbraWeight );

    float shd = 0;
    [unroll] for ( int i = 0; i < PLS_SHADOW_BLUR_COUNT; i++ )
    {
        float2 kernel = PLS_SHADOW_BLUR_OFFSETS[i];
        float2 rotatedKernel = float2( kernel.x * cosA - kernel.y * sinA, kernel.x * sinA + kernel.y * cosA );
        float3 perturbedDir = normalize( dir + (right * rotatedKernel.x + up * rotatedKernel.y) * filterRadius );
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
