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

inline void ResetAllRendererTests() {
    GetRendererTestSettings() = RendererTestSettings{};
}

// END TEMPORARY RENDERER TEST OVERRIDES
