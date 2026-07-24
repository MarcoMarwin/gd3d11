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
