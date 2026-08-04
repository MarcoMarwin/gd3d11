#ifndef GD3D11_SHARED_SSR_CORE_INCLUDED
#define GD3D11_SHARED_SSR_CORE_INCLUDED

struct SSRTraceResult {
    float hit;
    float2 hitUV;
    float confidence;
    float2 skyUV;
    float skyWeight;
};

float SSRCore_CalculateEdgeFade(float2 uv, float edgeWidth) {
    float2 edge = smoothstep(0.0f, edgeWidth, uv) * smoothstep(0.0f, edgeWidth, 1.0f - uv);
    return edge.x * edge.y;
}

bool SSRCore_ProjectWorldToUV(float3 positionWS, float4x4 viewProj, out float2 uv, out float clipW) {
    float4 projected = mul(float4(positionWS, 1.0f), viewProj);
    clipW = projected.w;
    if (projected.w <= 0.0f) {
        uv = float2(0.0f, 0.0f);
        return false;
    }
    projected.xyz /= projected.w;
    uv = projected.xy * float2(0.5f, -0.5f) + 0.5f;
    if (projected.z < 0.0f || projected.z > 1.0f) return false;
    return true;
}

float SSRCore_LoadSceneZ(Texture2D depthTexture, float2 uv, float2 viewportSize, float projectionZ, float projectionW) {
    int2 maxPixel = int2(viewportSize) - int2(1, 1);
    int2 pixel = clamp(int2(uv * viewportSize), int2(0, 0), maxPixel);
    float rawDepth = depthTexture.Load(int3(pixel, 0)).r;
    if (rawDepth <= 1e-7f) return 0.0f;
    float denominator = rawDepth - projectionW;
    if (abs(denominator) <= 1e-7f) return 0.0f;
    return projectionZ / denominator;
}

SSRTraceResult SSRCore_MissResult() {
    SSRTraceResult result;
    result.hit = 0.0f;
    result.hitUV = float2(0.0f, 0.0f);
    result.confidence = 0.0f;
    result.skyUV = float2(0.0f, 0.0f);
    result.skyWeight = 0.0f;
    return result;
}

SSRTraceResult SSRCore_TraceWorldRay(
    Texture2D depthTexture,
    float3 originWS, float3 directionWS,
    float4x4 viewProj, float2 viewportSize,
    float projectionZ, float projectionW,
    float maxDistance, int maxSteps, int refineSteps,
    float startBias, float thickness, float edgeFadeWidth)
{
    SSRTraceResult result = SSRCore_MissResult();
    float3 rayDirection = normalize(directionWS);
    float stepLength = maxDistance / max((float)maxSteps, 1.0f);

    float3 previousPosition = originWS + rayDirection * startBias;
    float2 previousUV;
    float previousClipW;
    if (!SSRCore_ProjectWorldToUV(previousPosition, viewProj, previousUV, previousClipW)) return result;

    if (any(previousUV < 0.0f) || any(previousUV > 1.0f)) return result;

    float previousSceneZ = SSRCore_LoadSceneZ(depthTexture, previousUV, viewportSize, projectionZ, projectionW);
    float previousDelta = previousSceneZ > 0.0f ? previousClipW - previousSceneZ : -1.0f;
    float travelled = startBias;

    [loop]
    for (int stepIndex = 0; stepIndex < maxSteps; ++stepIndex) {
        float3 currentPosition = previousPosition + rayDirection * stepLength;
        travelled += stepLength;
        float2 currentUV;
        float currentClipW;
        if (!SSRCore_ProjectWorldToUV(currentPosition, viewProj, currentUV, currentClipW)) return result;
        if (any(currentUV < 0.0f) || any(currentUV > 1.0f)) return result;

        float screenEdgeWeight = SSRCore_CalculateEdgeFade(currentUV, edgeFadeWidth);
        float currentSceneZ = SSRCore_LoadSceneZ(depthTexture, currentUV, viewportSize, projectionZ, projectionW);
        if (currentSceneZ <= 1e-7f) {
            result.skyUV = currentUV;
            result.skyWeight = screenEdgeWeight;
            previousPosition = currentPosition;
            previousDelta = -1.0f;
            continue;
        }

        float currentDelta = currentClipW - currentSceneZ;
        if (previousDelta < 0.0f && currentDelta >= 0.0f && currentSceneZ >= previousClipW - thickness) {
            float3 refinementLow = previousPosition;
            float3 refinementHigh = currentPosition;
            float2 refinedUV = currentUV;
            float refinedGap = currentDelta;

            [unroll]
            for (int refinementStep = 0; refinementStep < refineSteps; ++refinementStep) {
                float3 refinementPosition = (refinementLow + refinementHigh) * 0.5f;
                float2 refinementUV;
                float refinementClipW;
                if (!SSRCore_ProjectWorldToUV(refinementPosition, viewProj, refinementUV, refinementClipW)) {
                    refinementHigh = refinementPosition;
                    continue;
                }
                if (any(refinementUV < 0.0f) || any(refinementUV > 1.0f)) {
                    refinementHigh = refinementPosition;
                    continue;
                }
                float refinementSceneZ = SSRCore_LoadSceneZ(depthTexture, refinementUV, viewportSize, projectionZ, projectionW);
                if (refinementSceneZ <= 1e-7f) {
                    refinementLow = refinementPosition;
                    continue;
                }
                float refinementGap = refinementClipW - refinementSceneZ;
                if (refinementGap >= 0.0f) {
                    refinementHigh = refinementPosition;
                    refinedUV = refinementUV;
                    refinedGap = refinementGap;
                } else {
                    refinementLow = refinementPosition;
                }
            }

            if (refinedGap < thickness) {
                float edgeFade = SSRCore_CalculateEdgeFade(refinedUV, edgeFadeWidth);
                float distanceFade = saturate(1.0f - travelled / maxDistance);
                result.hit = 1.0f;
                result.hitUV = refinedUV;
                result.confidence = saturate(edgeFade * distanceFade);
                return result;
            }
        }
        previousPosition = currentPosition;
        previousDelta = currentDelta;
        previousClipW = currentClipW;
    }
    return result;
}
SSRTraceResult SSRCore_TraceScreenRay(
    Texture2D depthTexture,
    Texture2D blockMaskTexture,
    float3 originWS, float3 directionWS,
    float4x4 viewProj, float2 viewportSize,
    float projectionZ, float projectionW,
    float maxDistance, int maxSteps, int refineSteps,
    float screenStridePixels, float blockMaskScale, float edgeFadeWidth)
{
    SSRTraceResult result = SSRCore_MissResult();
    float3 rayDirection = normalize(directionWS);
    float3 rayEndWS = originWS + rayDirection * maxDistance;
    float2 startUV;
    float startClipW;
    float2 endUV;
    float endClipW;
    if (!SSRCore_ProjectWorldToUV(originWS, viewProj, startUV, startClipW)) return result;
    if (!SSRCore_ProjectWorldToUV(rayEndWS, viewProj, endUV, endClipW)) return result;
    float2 screenDeltaPixels = (endUV - startUV) * viewportSize;
    float screenLengthPixels = length(screenDeltaPixels);
    int screenSteps = min(maxSteps, max(1, (int)ceil(screenLengthPixels / max(screenStridePixels, 0.001f))));
    float traceFractionLimit = min(1.0f, (maxSteps * screenStridePixels) / max(screenLengthPixels, 1.0f));
    float inverseStartW = rcp(startClipW);
    float inverseEndW = rcp(endClipW);
    float previousFraction = 0.0f;
    float previousRayDepth = startClipW;
    [loop]
    for (int screenStep = 1; screenStep <= maxSteps; ++screenStep)
    {
        if (screenStep > screenSteps) break;
        float fraction = traceFractionLimit * ((float)screenStep / (float)screenSteps);
        float2 sampleUV = lerp(startUV, endUV, fraction);
        if (any(sampleUV < 0.0f) || any(sampleUV > 1.0f)) break;
        float inverseRayW = lerp(inverseStartW, inverseEndW, fraction);
        if (inverseRayW <= 1e-7f) break;
        float rayDepth = rcp(inverseRayW);
        float screenWeight = SSRCore_CalculateEdgeFade(sampleUV, edgeFadeWidth);
        float sceneDepth = SSRCore_LoadSceneZ(depthTexture, sampleUV, viewportSize, projectionZ, projectionW);
        if (sceneDepth <= 1e-7f)
        {
            if (screenWeight > result.skyWeight)
            {
                result.skyUV = sampleUV;
                result.skyWeight = screenWeight;
            }
            previousFraction = fraction;
            previousRayDepth = rayDepth;
            continue;
        }
        float segmentNearDepth = min(previousRayDepth, rayDepth);
        float segmentFarDepth = max(previousRayDepth, rayDepth);
        float pixelFootprint = max(abs(rayDepth - previousRayDepth), 1.0f);
        float hitThickness = min(max(2.0f, pixelFootprint * 1.5f), 18.0f);
        bool segmentCrossesScene = sceneDepth >= segmentNearDepth - hitThickness
            && sceneDepth <= segmentFarDepth + hitThickness;
        if (segmentCrossesScene)
        {
            float refinementLow = previousFraction;
            float refinementHigh = fraction;
            float2 refinedUV = sampleUV;
            float refinedRayDepth = rayDepth;
            float refinedSceneDepth = sceneDepth;
            float refinedError = abs(rayDepth - sceneDepth);
            bool refinedValid = true;
            [unroll]
            for (int refinementStep = 0; refinementStep < refineSteps; ++refinementStep)
            {
                float refinementFraction = (refinementLow + refinementHigh) * 0.5f;
                float2 refinementUV = lerp(startUV, endUV, refinementFraction);
                float refinementInverseW = lerp(inverseStartW, inverseEndW, refinementFraction);
                if (refinementInverseW <= 1e-7f)
                {
                    refinedValid = false;
                    break;
                }
                float refinementRayDepth = rcp(refinementInverseW);
                float refinementSceneDepth = SSRCore_LoadSceneZ(depthTexture, refinementUV, viewportSize, projectionZ, projectionW);
                if (refinementSceneDepth <= 1e-7f)
                {
                    refinementLow = refinementFraction;
                    continue;
                }
                float refinementError = abs(refinementRayDepth - refinementSceneDepth);
                if (refinementError < refinedError)
                {
                    refinedError = refinementError;
                    refinedUV = refinementUV;
                    refinedRayDepth = refinementRayDepth;
                    refinedSceneDepth = refinementSceneDepth;
                }
                if (refinementRayDepth > refinementSceneDepth)
                    refinementHigh = refinementFraction;
                else
                    refinementLow = refinementFraction;
            }
            float refinedThickness = min(max(2.0f, abs(refinedRayDepth - previousRayDepth) * 1.5f), 12.0f);
            int2 maxPixel = int2(viewportSize) - int2(1, 1);
            int2 refinedPixel = clamp(int2(refinedUV * viewportSize), int2(0, 0), maxPixel);
            float blocked = saturate(
                blockMaskTexture.Load(int3(refinedPixel, 0)).r / max(blockMaskScale, 1e-7f));
            if (refinedValid && refinedError <= refinedThickness && blocked <= 0.001f)
            {
                result.hit = 1.0f;
                result.hitUV = refinedUV;
                result.confidence = SSRCore_CalculateEdgeFade(refinedUV, edgeFadeWidth);
                return result;
            }
        }
        previousFraction = fraction;
        previousRayDepth = rayDepth;
    }
    return result;
}
#endif