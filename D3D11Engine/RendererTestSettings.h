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
}

inline void ResetAllRendererTests() {
    GetRendererTestSettings() = RendererTestSettings{};
}

// END TEMPORARY RENDERER TEST OVERRIDES
