#pragma once

#include "pch.h"

// BEGIN TEMPORARY RENDERER TEST OVERRIDES

struct RendererNightTestSettings {
    bool DisableNightAtmosphere = false;
    bool DisableNearNightBrightness = false;
    bool DisableNightFogBrightness = false;
    bool DisableNightDarkening = false;
    bool DisableEnvMapNightFactor = false;
    bool DisableNightRainAdjustments = false;
    bool DisableNightRainMidColor = false;
    bool DisableNightRainFarColor = false;
    bool DisableNightRainSkyColor = false;
    bool DisableNightRainMidInfluence = false;
    bool DisableNightRainWorldHazeStrength = false;
    bool DisableNightRainSkyHazeStrength = false;
    bool DisableNightRainFarMaxLuma = false;
    bool DisableNightRainVeryFarMaxLuma = false;
    bool DisableNightRainVeryFarInfluence = false;
    bool DisableDynamicCloudNightColor = false;
    bool DisableGroundNightContribution = false;
    bool DisableGroundRainAttenuation = false;
    bool UseNightlyGroundRainInput = false;
    bool DisableDecalNightRainLightingScale = false;
    bool DisableTransparentWorldMeshes = false;
    bool DisableTransparentVobMeshes = false;
    bool DisableTransparentDecals = false;
    bool DisableTransparentParticleMeshes = false;
    bool DisableTransparentNormalmaps = false;
    bool DisableTransparentFxMaps = false;
    bool DisableTransparentDisplacementMaps = false;
    bool ForceWhiteTransparentTextureFactor = false;
    bool DisableTransparentVobWindMetadata = false;
    bool DisableTransparentVobWindBuffer = false;
    bool UseBaseTextureForTransparentWorldMeshes = false;
    bool UseNightlyTemporalMatricesForTransparentWorldMeshes = false;
    bool UseNightlyWorldTransparencyTessellationReset = false;
    bool UseNightlyWaterfallTransparencyClassification = false;
    bool UseNightlyPerInstanceBufferForTransparentWorldMeshes = false;
    bool DisableWetSSRBlockerCollection = false;
    bool DisableWetSSRBlockerDraw = false;
    bool DisableRegularTransparencyDraw = false;
    bool DisablePortalTransparencyDraw = false;
    bool DisableWaterfallTransparencyDraw = false;
};

struct RendererTestSettings {
    bool EnableOverrides = false;
    RendererNightTestSettings Night;
};

inline RendererTestSettings& GetRendererTestSettings() {
    static RendererTestSettings settings;
    return settings;
}

inline void ResetRendererNightTests() {
    GetRendererTestSettings().Night = RendererNightTestSettings{};
}

inline void ResetTransparencyStructuralTests() {
    RendererNightTestSettings& night = GetRendererTestSettings().Night;
    night.DisableTransparentWorldMeshes = false;
    night.DisableTransparentVobMeshes = false;
    night.DisableTransparentDecals = false;
    night.DisableTransparentParticleMeshes = false;
    night.DisableTransparentNormalmaps = false;
    night.DisableTransparentFxMaps = false;
    night.DisableTransparentDisplacementMaps = false;
    night.ForceWhiteTransparentTextureFactor = false;
    night.DisableTransparentVobWindMetadata = false;
    night.DisableTransparentVobWindBuffer = false;
    night.UseBaseTextureForTransparentWorldMeshes = false;
    night.UseNightlyTemporalMatricesForTransparentWorldMeshes = false;
    night.UseNightlyWorldTransparencyTessellationReset = false;
    night.UseNightlyWaterfallTransparencyClassification = false;
    night.UseNightlyPerInstanceBufferForTransparentWorldMeshes = false;
    night.DisableWetSSRBlockerCollection = false;
    night.DisableWetSSRBlockerDraw = false;
    night.DisableRegularTransparencyDraw = false;
    night.DisablePortalTransparencyDraw = false;
    night.DisableWaterfallTransparencyDraw = false;
}

inline void ResetAllRendererTests() {
    GetRendererTestSettings() = RendererTestSettings{};
}

// END TEMPORARY RENDERER TEST OVERRIDES
