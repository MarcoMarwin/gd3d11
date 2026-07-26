## Build 157

- Ambient Particles: Verify completely removed from UI, settings, presets, and original script files (m_bIsAmbientPFX).
- Vegetation Push: Completely decoupled from WindQuality. User value Display/HeroAffectsObjects is now correctly preserved.
- Visual FX Draw Distance: Hardcoded to 10000.0f. Removed obsolete slider, INI loading/saving, and logic decoupling it from OutdoorSmallVobDrawRadius.
- Lens Flare: Procedural Lens Flare 1:1 integrated into GodRay shaders (PS_PFX_GodRayZoom.hlsl and CS_PFX_GodRayZoom.hlsl). Completely driven by Godrays/GodRayStrength without extra settings.


## Build 158

- Ambient Particles: Finalized 1:1 code replacements. Completely removed ambient particles from active code.
- Vegetation Push: HeroAffectsObjects triggers Vertex shader reload and is completely decoupled.
- Atmospheric Scattering: Replaced sun transmission masks to prevent high-energy centers from being visible through cloud layers.
- Water SSR: Added shore-based attenuation to SSR reflections.

## Build 159

- Atmospheric Scattering: Removed artificial sun profile zoning and integrated unified celestial preservation masks for sun and moon.
- Moon Projection: Added AC_MoonScreenPos to atmosphere constant buffers and GSky celestial projection for accurate moon transmission through low clouds.
- Vegetation Wind: Integrated interaction push scale to properly attenuate both current and previous wind offsets for consistent FSR motion vectors.
- UI: Corrected contact shadows tooltip translation and UTF-8 escapes.

## Build 160

- Lens Flare: Removed orange halo ring and ghost rings around the sun, compacting sun glow and preserving godrays.
- Section Draw Distance: Configured presets (Low: 3, Medium: 5, High: 7, Extreme: 9) with Medium (5) as global default.
- F11 Menu & Presets: Integrated Surface Detail, Water Reflections (SSR & strength), Vegetation Push, and Rain Rendering into graphics presets.
- Surface Detail: Simplified UI to a single checkbox automatically controlling normal maps and parallax occlusion mapping.
- Particle FX: Added continuous rain-dependent opacity scaling for smoke/fog (50% -> 70%) and water particles (100% -> 50%) via GetRainFXWeight.

## Build 161

- F11 Menu & UI: Fixed Contrast and Brightness slider width to standardComboWidth and restored exact menu column symmetry.
- Presets: Activated Water Reflections in Low preset (SSR & strength 1.0) and updated Object Draw Distance presets to 3 / 5 / 7 / 9.
- Contact Shadows: Removed FSR 3 composition scale dampening (CC_ContactShadowScale = 1.0f).
- Shadow Map: Neutralized frame-variable FSR 3 projection jitter offset (SQ_JitterOffset = float2(0.0f, 0.0f)) in FillSunCSMConstantBuffer.

## Build 162

- Contact Shadows: Removed diagnostic FSR 3 bypass from IndoorReceiverMask in PS_PFX_ScreenSpaceLightingTrace.hlsl so Contact Shadows target indoor receivers exclusively in all modes.
- Shadow Map: Neutralized frame-variable FSR 3 projection jitter in DrawWorldLights (SQ_JitterOffset = float2(0.0f, 0.0f)).
- Default Settings: Updated default OutdoorSmallVobDrawRadius to 12500.0f (UI level 5) in GothicGraphicsState.h, matching default Object Draw Distance with Medium preset.

## Build 163

- Water Shader: Replaced legacy water murkiness in PS_Water.hlsl with physical volumetric absorption and scattering model (legacyVolume).
- Transparency Rendering: Restored nightly-compatible 3-SRV resource binding and 4-argument BindShaderForTexture call in DrawMeshInfoListAlphablended (D3D11GraphicsEngine.cpp).

## Build 164

- FSR3 Jitter: Restored FSR 3 camera projection jitter in D3D11ShadowMap.cpp to exact Build 160 state.
- Transparency Rendering: Restored original extended resource and shader binding in DrawMeshInfoListAlphablended (D3D11GraphicsEngine.cpp).
- Transparent EnvMap World Surfaces: Reduced initial night opacity factor from 0.1f to 0.05f in ComputeTransparencyTextureFactor (D3D11GraphicsEngine.cpp).

## Build 165

- Diagnostics: Added temporary runtime diagnostic overrides in GSky.cpp and D3D11GraphicsEngine.cpp to isolate and neutralize general night parameters and night rain adjustments at runtime, controlled via a non-modal diagnostic window and toggle button in ImGuiShim.cpp.

## Build 166

- Diagnostics: Extended runtime diagnostic overrides with four targeted toggles for ground night contribution, ground rain attenuation, nightly ground rain input, and decal night/rain lighting scale, controlled via ImGuiShim and evaluated in GSky.cpp, AtmosphericScattering.h, and D3D11GraphicsEngine.cpp.

## Build 167

- Diagnostics: Implemented additional temporary transparency diagnostic package with toggles for path identification (world meshes, VOB meshes, decals, particles), transparent world materials (normalmaps, fx maps, displacement maps, white texture factor), and VOB wind diagnostics (metadata, wind buffer) in D3D11GraphicsEngine.cpp and ImGuiShim.cpp.

## Build 168

- Diagnostics: Extended F11 transparency diagnostic menu with Nightly world transparency comparison tests (base texture fallback, temporal matrices, tessellation reset, waterfall classification, per-instance buffer) and transparency list isolation toggles (wet-SSR blocker collection/draw, regular/portal/waterfall transparency draw) in D3D11GraphicsEngine.cpp and ImGuiShim.cpp.

## Build 169
- Added UseNightlyBlendShaderForTransparentWorldMeshes diagnostic flag and integrated it via SetActivePixelShader( PShaderID::PS_Simple_FF ).
- Re-architected DisableWaterfallTransparencyDraw for early elimination of MT_WaterfallFoam.
- Extracted and isolated DisableNightRainMidColor, FarColor, SkyColor, MidInfluence, WorldHazeStrength, SkyHazeStrength over AC_NightRain configuration.
- Added Particle Lighting Diagnostics (DisableParticleNightDimming, DisableParticleRainAlphaReduction).
- Added Transparent World Mesh Alpha and Night EnvMap Factor controls.

## Build 170 (Korrekturpush)
- Korrekturpush für Build 169: C2679 Compilerfehler in GSky.cpp behoben, indem inkompatible float3-Zuweisungen an AC_NightRain-Farbwerte durch XMFLOAT3 ersetzt wurden.

## Build 171
- Transparent World Meshes: Made PS_Simple_FF shader standard for BLEND and ADD alpha functions; removed diagnostic UseNightlyBlendShaderForTransparentWorldMeshes toggle.
- Transparent World Meshes: Added Transparent World Mesh Brightness multiplier to F11 diagnostics, strictly targeting RGB.
- Diagnostics: Added Disable Wet Ground SSR and Disable Transparent World Mesh Depth/Fog Replay toggles to F11 transparency menu.


## Build 171
- Transparent World Meshes: Made PS_Simple_FF shader standard for BLEND and ADD alpha functions; removed diagnostic UseNightlyBlendShaderForTransparentWorldMeshes toggle.
- Transparent World Meshes: Added Transparent World Mesh Brightness multiplier to F11 diagnostics, strictly targeting RGB.
- Diagnostics: Added Disable Wet Ground SSR and Disable Transparent World Mesh Depth/Fog Replay toggles to F11 transparency menu.

