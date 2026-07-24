## Build 157

- Ambient Particles: Verify completely removed from UI, settings, presets, and original script files (m_bIsAmbientPFX).
- Vegetation Push: Completely decoupled from WindQuality. User value Display/HeroAffectsObjects is now correctly preserved.
- Visual FX Draw Distance: Hardcoded to 10000.0f. Removed obsolete slider, INI loading/saving, and logic decoupling it from OutdoorSmallVobDrawRadius.
- Lens Flare: Procedural Lens Flare 1:1 integrated into GodRay shaders (PS_PFX_GodRayZoom.hlsl and CS_PFX_GodRayZoom.hlsl). Completely driven by Godrays/GodRayStrength without extra settings.

