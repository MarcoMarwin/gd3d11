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
#endif