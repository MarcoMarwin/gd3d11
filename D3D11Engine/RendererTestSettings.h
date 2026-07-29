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
    bool DisableParticleNightDimming = false;
    bool DisableParticleRainAlphaReduction = false;
    bool DisableTransparentWorldMeshes = false;
    bool DisableTransparentVobMeshes = false;
    bool DisableTransparentDecals = false;
    bool DisableTransparentParticleMeshes = false;
    bool DisableTransparentNormalmaps = false;
    bool DisableTransparentFxMaps = false;
    bool DisableTransparentDisplacementMaps = false;
    bool ForceWhiteTransparentTextureFactor = false;
    float TransparentWorldMeshBrightness = 1.0f;
    float TransparentWorldMeshAlpha = 1.0f;
    bool DisableTransparentVobWindMetadata = false;
    bool DisableTransparentVobWindBuffer = false;
    bool UseBaseTextureForTransparentWorldMeshes = false;
    bool UseNightlyWorldTransparencyTessellationReset = false;
    bool UseNightlyWaterfallTransparencyClassification = false;
    bool EnableSceneWetnessEffects = true;
    float SceneWetnessEffectsStrength = 1.0f;
    bool EnableWetMaterialReflections = true;
    float WetMaterialReflectionsStrength = 1.0f;
    bool EnableProceduralPuddles = true;
    float ProceduralPuddlesStrength = 1.0f;
    bool EnablePuddleReflections = true;
    float PuddleReflectionsStrength = 1.0f;
    bool EnableWetGroundRainImpacts = true;
    float WetGroundRainImpactsStrength = 1.0f;
    bool DisableWetGroundSSR = false;
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
    night.TransparentWorldMeshBrightness = 1.0f;
    night.TransparentWorldMeshAlpha = 1.0f;
    night.DisableTransparentVobWindMetadata = false;
    night.DisableTransparentVobWindBuffer = false;
    night.UseBaseTextureForTransparentWorldMeshes = false;
    night.UseNightlyWorldTransparencyTessellationReset = false;
    night.UseNightlyWaterfallTransparencyClassification = false;
    night.EnableSceneWetnessEffects = true;
    night.SceneWetnessEffectsStrength = 1.0f;
    night.EnableWetMaterialReflections = true;
    night.WetMaterialReflectionsStrength = 1.0f;
    night.EnableProceduralPuddles = true;
    night.ProceduralPuddlesStrength = 1.0f;
    night.EnablePuddleReflections = true;
    night.PuddleReflectionsStrength = 1.0f;
    night.EnableWetGroundRainImpacts = true;
    night.WetGroundRainImpactsStrength = 1.0f;
    night.DisableWetGroundSSR = false;
    night.DisableRegularTransparencyDraw = false;
    night.DisablePortalTransparencyDraw = false;
    night.DisableWaterfallTransparencyDraw = false;
}

inline void ResetAllRendererTests() {
    GetRendererTestSettings() = RendererTestSettings{};
}

// END TEMPORARY RENDERER TEST OVERRIDES
