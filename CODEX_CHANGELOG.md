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
