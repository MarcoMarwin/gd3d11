# Build Changelog

Dokumentation der gepushten Renderer-Builds.







## Build 209
- Korrekturpush: Erster Push für Build 209. Einführung von echten PCSS (Percentage-Closer Soft Shadows) für Pointlights! Der GatherCmp-Ansatz wurde verworfen. Stattdessen nutzt der Shader nun eine 5-Tap Blocker-Suche, um die durchschnittliche Distanz zum Verdeckungsobjekt (Occluder) zu bestimmen. Aus dem Verhältnis von Occluder- zu Receiver-Distanz wird dann dynamisch ein penumbraRatio berechnet. Das sorgt für einen völlig realistischen Schattenwurf: Knackscharfe Kontakt-Schatten direkt am Objekt, die mit zunehmender Distanz butterweich ausfransen. Und das alles hochgradig effizient mit nur 13 Taps (5x Blocker Search via Linear-Sampler + 8x Filter via HW-PCF).- Korrekturpush: Feinschliff für Pointlight PCSS und Night-Lighting in Build 209.
  - Das PCSS-System hat ein Fallback für stark entfernte Lichtquellen erhalten (shadowSoftness < -0.01f). Über negative Softness-Werte im Backend greift der Shader auf einen simplen und extrem schnellen Far-PCF-Pfad zurück. Außerdem wurde das PCSS Kontakt-Hardening überarbeitet, sodass die Kernels bei Kontakt nicht auf 0 kollabieren, was Artefakte verhindert.
  - Der AtmosphericScattering-Shader hat eine subtile Indirect-Night-Illumination (
ightAmbientColor * 0.035f * worldAO) erhalten, um zu verhindern, dass Indoor-Materialien oder abgewandte Flächen in völliger Dunkelheit absaufen, selbst wenn Pointlight-Schatten greifen.
- Regulärer Push: Abschließende Optimierungen in Build 209. Neben den Pointlight-Schatten hat nun auch das Cascaded Shadow Maps (CSM) System für das Sonnenlicht ein massives Update erhalten, speziell für dynamische Objekte wie NPCs. Für animierte Charaktere greift nun ein eigener, ultrastabiler Schatten-Pfad (ComputeCascadedShadowValueCharacter). Dabei wird die Kaskaden-Auswahl anhand der Original-Position berechnet, der Normal-Offset und Depth-Bias aber erst für die *tatsächlich gesampelte* Kaskade ausgewertet. Zudem wird nun die stabilisierte Licht-Richtung (SQ_CascadeLightDirectionWS) pro Kaskade an den Shader übergeben. Das Ergebnis: Perfekt stabile Charakter-Schatten ganz ohne Flimmern, während statische Objekte weiterhin vom performanten regulären Pfad profitieren.
- Korrekturpush: Rollback auf Build 209
  - Build 210 (Wake-System & Ocean Octaves) wurde vorübergehend verworfen.
  - Der Master-Branch wurde via Force-Push wieder auf den stabilen Stand von Build 209 (Stable CSM für NPCs) zurückgesetzt.
- Regulärer Push: Abschluss Build 209 (Stable CSM für NPCs)
## Build 208
- Korrekturpush: Erster Push für Build 208. Radikales Redesign der Wind-Physik für Vegetation (VS_ExInstancedObj.hlsl): Die alte Metronom-Animation wurde durch ein physikalisch plausibleres Modell ersetzt. Die Wind-Phase wird nun räumlich (per World-XZ) statt per Matrix-Hash berechnet, sodass benachbarte Pflanzen gemeinsam im Windstoß wogen. Das Biegungsverhalten berechnet sich adaptiv aus der "Slenderness" (Höhe vs Breite), wodurch kleine Pflanzen vom Boden aus biegen, während Bäume einen steifen Stamm behalten (sanfte kubische Kurve ohne "Scharnier"-Kante). Zudem biegt sich die Pflanze primär in Windrichtung, mit reduzierter seitlicher Trägheit. Detail-Turbulenzen für Blätter werden über den Abstand zum Zentrum (radialPosition) abgeleitet, völlig ohne Vertex-Daten! Zusätzlich wurde in PS_PFX_Velocity.hlsl die teure 3x3 Velocity-Dilation endgültig durch einen direkten Lookup ersetzt.## Build 207
- Korrekturpush: Erster Push für Build 207. Radikale Vereinfachung der City-Window Sky-Protection (PS_Simple): Anstatt einer komplexen UV-Homographie-Berechnung wird nun strikt pro Pixel im Screen-Space geprüft – ist das Fenster-Pixel im unteren Drittel und liegt über dem Himmel-Depth, wird es opak (absolut fehlerfrei und performant). In VS_ExInstanced wurden die Motion Vectors repariert: Statische Instanzen rekonstruieren nun ihre echte World-Space-Position, damit die Kamera-Bewegung für die FSR3-Reprojektion korrekt abgebildet wird. Die 3x3 Velocity-Dilation in PS_MotionBlur wurde zugunsten eines direkten Lookups restlos gestrichen.## Build 206
- Korrekturpush: Erster Push für Build 206. Die Wasser-Render-Logik in D3D11GraphicsEngine und PS_Water.hlsl wurde vereinfacht (kein harter Split mehr für die Geometrie, was Flackern an schmalen Übergängen behebt). CSM-Schatten nutzen in ShadowSampling.h nun einen Dither-Schritt in der Penumbra, um PCF-Banding aufzubrechen. Daytime Gothic-Fog verdeckt Geometrie auf Distanz nun vollständig (PS_PFX_Heightfog und Composition). In PS_Simple wurde der alte Sky-Protection-Fallback wieder aktiviert, falls der Compute-Shader-Cache fehlt.## Build 205
- Korrekturpush: Nachtrag für Build 204. GetGothicTexture() wurde in MyDirectDrawSurface7.h ergänzt, was vom Particle Texture-Swap Mechanismus in GothicAPI zwingend benötigt wird.
## Build 204
- Korrekturpush: Sicherheits-Fix für Constant Buffer Bounds. MAX_SHADER_CB wurde von 6 auf 14 (D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT) erhöht, um Out-of-Bounds Zugriffe beim Reflektieren von Shadern (z.B. durch Cutout-Konstanten an b6) zu verhindern.
## Build 198
- Regulaerer Push: NightFogRainFade Polynom-glaettung in D3D11PfxRenderer.cpp (verhindert stotternde Nebeluebergaenge durch nicht-lineare Interpolation); Anpassung der Fade-Speeds (0.35/0.55) fuer ein fluessigeres Ingame-Erlebnis.

## Build 197
- Regulaerer Push: NightFogRainFade-Tracking in D3D11PfxRenderer und PS_PFX_Composition.hlsl (weicheres Ein-/Ausblenden des Nebels nachts bei Regen); manuelle Anpassungen an PS_PFX_WetGroundSSR.hlsl; Caching-Optimierung in D3D11ShadowMap.cpp (Update-Threshold).

## Build 196
- Regulaerer Push: Pfuetzen-Zeitsteuerung in GothicAPI von Echtzeit auf Ingame-Zeit umgestellt; SSR-Raymarching in SSR.h mit Viewport-Clipping optimiert; WetGroundSSR-Tracing auf 256 Steps/2.0 Stride verfeinert, Wet-Mask-Exposure an RainFXWeight gekoppelt und GetRainExposure auf PCF-Filter umgebaut; ContactShadow-Tageszeitkopplung in D3D11PfxRenderer geloest; PS_ParticleSimple.hlsl manuell aktualisiert.

## Build 195
- Regulaerer Push: SSR-Raymarching in SSR.h zentralisiert; LowCloud-Refinement in PS_PFX_LowCloudComposite.hlsl auf 4 Steps optimiert; AdvancedSettings in ImGuiShim und GSky.cpp entfernt; NPC-Tagging in Diffuse und LightingTrace integriert.

## Build 190
- Regulaerer Push: Low Clouds um SkyClouds-Target in PS_PFX_LowClouds.hlsl und PS_PFX_LowCloudComposite.hlsl erweitert, um Artefakte an Alpha-Test-Silhouetten zu beheben. D3D11PfxRenderer und RenderGraph in D3D11GraphicsEngine.cpp an das neue Target angepasst; E_GodRayMode Persistenz und Menue (GothicAPI.cpp, GothicGraphicsState.h, ImGuiShim.cpp) korrigiert.

## Build 188
- Regulaerer Push: Korrektur der GodRay-Volumen-Berechnung (MaxDistance, LightColor, GlobalDensity, WeightZNear/Far) in D3D11PFX_GodRays.cpp; Anpassung des LightDirection-Skalarprodukts in CS_PFX_GodRayZoom.hlsl; Fallback-Logik fuer fehlende GodRay-Composition in D3D11GraphicsEngine.cpp.

## Build 187
- Regulaerer Push: Neue Vegetationsdichte-Option ueber topologische Nachgruppierung (Disjoint-Set) integriert, um Mesh-Flackern durch gezieltes Entfernen ganzer Aeste/Buesche zu vermeiden. E_GodRayMode fuer volumetrische GodRays eingefuehrt und in die F11-Menue-Presets integriert.

## Build 184
- Regulaerer Push: Korrektur der `ApplyMaterialCompatibility`-Funktion, Ãœberarbeitung der WetGroundSSR-MaterialabhÃ¤ngigkeiten und Integration neuer Texturen/Material-Updates fÃ¼r verbesserte StabilitÃ¤t und Konsistenz.

## Build 183 (Korrekturpush)
- Korrekturpush: DoF-Composite repariert, fehlerhafter SkyEdgeBlur-Rekonstruktionspfad entfernt und durch stabilen early-out fuer Sky-Pixel (sharpColor) ersetzt, um Artefakte an Alpha-Test-Silhouetten zu beheben.

- Regulaerer Push: F11-Menue aufgeraeumt, Preset-Entkopplung fuer Regendarstellung durchgefuehrt, kompakte Anti-Aliasing-Zeile implementiert, VSync/FPS-Limit umstrukturiert und doppelte Rain-Rendering-Zeile entfernt.

## Build 182

- Scene Wetness: Fallback auf distortion.dds ohne AC_RainFXWeight bei deaktivierten Surface Details.
- Wet Material Reflections: Standardstaerke auf 1.5f angehoben.
- Wet Ground SSR: Materialien mit wetGroundSSRStrength 0.0 vollstaendig von prozeduralen Pfuetzen ausgeschlossen (materialPuddleEligibility via step).
- HDR: Dateiname von hdr.h zu HDR.h in Git korrigiert.


- Korrekturpush: Fehlende Struct-Member Definition und Reset-Zuweisung fuer DisableTransparentWorldMeshDepthFogReplay in RendererTestSettings.h sowie zugehoerige F11-Checkbox in ImGuiShim.cpp korrigiert (Fix fuer Compilerfehler C2039/C2737 in Release_G1_AVX2).

## Build 181
- Wet Ground SSR: Constant-Buffer-Layout WetGroundSSRConstantBuffer und F11-Teststaerken (WG_WetMaterialReflectionsStrength, WG_ProceduralPuddlesStrength, WG_PuddleReflectionsStrength, WG_WetGroundRainImpactsStrength) in HLSL (PS_PFX_WetGroundSSR.hlsl) und C++ (ConstantBufferStructs.h, D3D11PfxRenderer.cpp) vollstaendig synchronisiert (288 Bytes) und fuer materialWetMask, puddleMask, rippleDistortion und puddleReflectionBlend angewendet.

## Build 180
- Wet Ground SSR: Sampler s0/s1 werden vor dem DrawFullScreenQuad gesichert und danach wiederhergestellt, um Shadow-Comparison-Sampler Leaks im Transparenz-Pass zu verhindern.
- Rain Shadowmap: FF_AlphaRef auf 0.75f korrigiert, damit Alpha-Test-Vegetation regendurchlÃ¤ssig bleibt.
- Atmospheric Scattering: Einheitliche Umschaltkurve `GetRainCloudTransitionWeight()` eingefÃ¼hrt und Test-Flag `UseNightlyGroundRainInput` restlos entfernt.

## Build 179
- Korrekturpush: Fix GitHub-Buildfehler C2338 (16-byte aligned WetGroundSSRConstantBuffer). Padding auf CPU- und HLSL-Seite von float3 auf float2 korrigiert, um exakt 272 Byte Groesse und 16-Byte-Ausrichtung zu erreichen.
- Atmosphere & Sky: AtmosphericRainWeight-Zustandsmaschine in GSky::RenderSky() und ResetWeatherState() entfernt. Himmel, RainClouds, Ausblendung der Dynamic Clouds, Sonnen-/Mondsichtbarkeit, atmosphÃ¤rische Bodenabdunklung und entfernte Geometrie verwenden nun pro Frame ausschlieÃŸlich den gemeinsamen, bereinigten Wert aus GothicAPI::GetRainFXWeight().

## Build 178
- PfxRenderer: RainFogColor, RainFogDensity und FogRange Parameter Ã¼bergeben.
- Wet Ground SSR: ApplyWetGroundRainHaze fÃ¼r echten volumetrischen Regennebel angepasst.
- GothicAPI: Die DRAGONISLAND Wolkendeaktivierung entfernt.
- GothicAPI: ZehnminÃ¼tige PfÃ¼tzennachwirkung (SceneWetness) nach Regenende eingefÃ¼hrt.
- Water: Prozedurale Regenringe auf horizontalem Ozean- und Legacywasser eingefÃ¼gt, inkl. Unterwasserkorrektur.

## Build 177
- D3D11GraphicsEngine: BeschÃ¤digten duplizierten Block in OnStartWorldRendering vollstÃ¤ndig entfernt.
- Wet Ground SSR: RegenplÃ¤tschern durch weltverankerte RegentropfeneinschlÃ¤ge ersetzt, kÃ¼nstliche Pseudoreflexionen entfernt und korrekte Inaktivierung bei aussetzendem Regen sichergestellt.

## Build 176
- VollstÃ¤ndiger und restloser RÃ¼ckbau der Transparent-World-Coverage-SonderlÃ¶sung fÃ¼r Wet Ground SSR.
- Implementierung der DDA-freien Raymarch-PfÃ¼tzenlogik.

## Build 175
- Integration: Replaced obsolete PS_TransparencyWetMask with PS_TransparentWorldCoverage in Visual Studio project files.
- GothicAPI: Disabled Dynamic Clouds for the DRAGONISLAND world.
- GothicAPI: Large-Area-Particles (mfx_snow_exp, leaves) are now unconditionally added to the rendering list regardless of GetShowVisual() status.

## Build 174
- Wet Ground SSR: Replaced World-Space raymarching with pixel-uniform Screen-Space DDA traversal.
- Wet Ground SSR: Transparent world meshes (including ice) are now reliably excluded via a 3x3 blocker mask evaluation.
- Wet Ground SSR: Added continuous animated rain ripples mapped to Distortion.dds with frac-based tiling.

## Build 173
- Extended IsLargeAreaParticleVob for snow and leaves to include texture names.
- Stripped BOM/invalid bytes from PS_PFX_DoF.hlsl file start.
- Corrected wetUV indentation in PS_PFX_WetGroundSSR.hlsl.

## Build 172
- Renamed DrawWaterfallMask to DrawTransparentWorldWetSSRMask.
- Corrected indentations and logic in the renamed function.

## Build 171
- Transparent World Meshes: Made PS_Simple_FF shader standard for BLEND and ADD alpha functions; removed diagnostic UseNightlyBlendShaderForTransparentWorldMeshes toggle.
- Transparent World Meshes: Added Transparent World Mesh Brightness multiplier to F11 diagnostics, strictly targeting RGB.
- Diagnostics: Added Disable Wet Ground SSR and Disable Transparent World Mesh Depth/Fog Replay toggles to F11 transparency menu.

## Build 171
- Transparent World Meshes: Made PS_Simple_FF shader standard for BLEND and ADD alpha functions; removed diagnostic UseNightlyBlendShaderForTransparentWorldMeshes toggle.
- Transparent World Meshes: Added Transparent World Mesh Brightness multiplier to F11 diagnostics, strictly targeting RGB.
- Diagnostics: Added Disable Wet Ground SSR and Disable Transparent World Mesh Depth/Fog Replay toggles to F11 transparency menu.

### Build 171
- Entkopplung von Wet Ground SSR von den Specular-Werten (eigenstaendiger Wert wetGroundSSRStrength 0.0 - 1.0).
- Ueberarbeitung des Regentropfen-Grizzle-Effekts fuer Wet Ground SSR (kurze statische Impulse statt UV-Verschiebung).

## Build 170 (Korrekturpush)
- Korrekturpush fÃ¼r Build 169: C2679 Compilerfehler in GSky.cpp behoben, indem inkompatible float3-Zuweisungen an AC_NightRain-Farbwerte durch XMFLOAT3 ersetzt wurden.

## Build 169
- Added UseNightlyBlendShaderForTransparentWorldMeshes diagnostic flag and integrated it via SetActivePixelShader( PShaderID::PS_Simple_FF ).
- Re-architected DisableWaterfallTransparencyDraw for early elimination of MT_WaterfallFoam.
- Extracted and isolated DisableNightRainMidColor, FarColor, SkyColor, MidInfluence, WorldHazeStrength, SkyHazeStrength over AC_NightRain configuration.
- Added Particle Lighting Diagnostics (DisableParticleNightDimming, DisableParticleRainAlphaReduction).
- Added Transparent World Mesh Alpha and Night EnvMap Factor controls.

## Build 168

- Diagnostics: Extended F11 transparency diagnostic menu with Nightly world transparency comparison tests (base texture fallback, temporal matrices, tessellation reset, waterfall classification, per-instance buffer) and transparency list isolation toggles (wet-SSR blocker collection/draw, regular/portal/waterfall transparency draw) in D3D11GraphicsEngine.cpp and ImGuiShim.cpp.

## Build 167

- Diagnostics: Implemented additional temporary transparency diagnostic package with toggles for path identification (world meshes, VOB meshes, decals, particles), transparent world materials (normalmaps, fx maps, displacement maps, white texture factor), and VOB wind diagnostics (metadata, wind buffer) in D3D11GraphicsEngine.cpp and ImGuiShim.cpp.

## Build 166

- Diagnostics: Extended runtime diagnostic overrides with four targeted toggles for ground night contribution, ground rain attenuation, nightly ground rain input, and decal night/rain lighting scale, controlled via ImGuiShim and evaluated in GSky.cpp, AtmosphericScattering.h, and D3D11GraphicsEngine.cpp.

## Build 165

- Diagnostics: Added temporary runtime diagnostic overrides in GSky.cpp and D3D11GraphicsEngine.cpp to isolate and neutralize general night parameters and night rain adjustments at runtime, controlled via a non-modal diagnostic window and toggle button in ImGuiShim.cpp.

## Build 164

- FSR3 Jitter: Restored FSR 3 camera projection jitter in D3D11ShadowMap.cpp to exact Build 160 state.
- Transparency Rendering: Restored original extended resource and shader binding in DrawMeshInfoListAlphablended (D3D11GraphicsEngine.cpp).
- Transparent EnvMap World Surfaces: Reduced initial night opacity factor from 0.1f to 0.05f in ComputeTransparencyTextureFactor (D3D11GraphicsEngine.cpp).

## Build 163

- Water Shader: Replaced legacy water murkiness in PS_Water.hlsl with physical volumetric absorption and scattering model (legacyVolume).
- Transparency Rendering: Restored nightly-compatible 3-SRV resource binding and 4-argument BindShaderForTexture call in DrawMeshInfoListAlphablended (D3D11GraphicsEngine.cpp).

## Build 162

- Contact Shadows: Removed diagnostic FSR 3 bypass from IndoorReceiverMask in PS_PFX_ScreenSpaceLightingTrace.hlsl so Contact Shadows target indoor receivers exclusively in all modes.
- Shadow Map: Neutralized frame-variable FSR 3 projection jitter in DrawWorldLights (SQ_JitterOffset = float2(0.0f, 0.0f)).
- Default Settings: Updated default OutdoorSmallVobDrawRadius to 12500.0f (UI level 5) in GothicGraphicsState.h, matching default Object Draw Distance with Medium preset.

## Build 161

- F11 Menu & UI: Fixed Contrast and Brightness slider width to standardComboWidth and restored exact menu column symmetry.
- Presets: Activated Water Reflections in Low preset (SSR & strength 1.0) and updated Object Draw Distance presets to 3 / 5 / 7 / 9.
- Contact Shadows: Removed FSR 3 composition scale dampening (CC_ContactShadowScale = 1.0f).
- Shadow Map: Neutralized frame-variable FSR 3 projection jitter offset (SQ_JitterOffset = float2(0.0f, 0.0f)) in FillSunCSMConstantBuffer.

## Build 160

- Lens Flare: Removed orange halo ring and ghost rings around the sun, compacting sun glow and preserving godrays.
- Section Draw Distance: Configured presets (Low: 3, Medium: 5, High: 7, Extreme: 9) with Medium (5) as global default.
- F11 Menu & Presets: Integrated Surface Detail, Water Reflections (SSR & strength), Vegetation Push, and Rain Rendering into graphics presets.
- Surface Detail: Simplified UI to a single checkbox automatically controlling normal maps and parallax occlusion mapping.
- Particle FX: Added continuous rain-dependent opacity scaling for smoke/fog (50% -> 70%) and water particles (100% -> 50%) via GetRainFXWeight.

## Build 159

- Atmospheric Scattering: Removed artificial sun profile zoning and integrated unified celestial preservation masks for sun and moon.
- Moon Projection: Added AC_MoonScreenPos to atmosphere constant buffers and GSky celestial projection for accurate moon transmission through low clouds.
- Vegetation Wind: Integrated interaction push scale to properly attenuate both current and previous wind offsets for consistent FSR motion vectors.
- UI: Corrected contact shadows tooltip translation and UTF-8 escapes.

## Build 158

- Ambient Particles: Finalized 1:1 code replacements. Completely removed ambient particles from active code.
- Vegetation Push: HeroAffectsObjects triggers Vertex shader reload and is completely decoupled.
- Atmospheric Scattering: Replaced sun transmission masks to prevent high-energy centers from being visible through cloud layers.
- Water SSR: Added shore-based attenuation to SSR reflections.

## Build 157

- Ambient Particles: Verify completely removed from UI, settings, presets, and original script files (m_bIsAmbientPFX).
- Vegetation Push: Completely decoupled from WindQuality. User value Display/HeroAffectsObjects is now correctly preserved.
- Visual FX Draw Distance: Hardcoded to 10000.0f. Removed obsolete slider, INI loading/saving, and logic decoupling it from OutdoorSmallVobDrawRadius.
- Lens Flare: Procedural Lens Flare 1:1 integrated into GodRay shaders (PS_PFX_GodRayZoom.hlsl and CS_PFX_GodRayZoom.hlsl). Completely driven by Godrays/GodRayStrength without extra settings.

## Build 156

- Vegetation Push: Umwandlung in eine reine Checkbox im F11-Menue direkt vor Rain Rendering. Der Verdraengungsradius ist fest auf 1200.0f eingestellt. FixupSettings ueberschreibt HeroAffectsObjects nicht mehr, auch bei deaktiviertem Wind nicht.
- Screen-Space GI: Umwandlung in eine reine Checkbox ohne Staerke-Slider im F11-Menue. FixupSettings stellt ScreenSpaceGIStrength bei aktivierter Checkbox fest auf 1.0f und bei deaktivierter Checkbox auf 0.0f ein. ScreenSpaceGIStrength wurde aus Preset-Vergleichen entfernt.
- Ambient Particles & [ENGINE]/noAmbientPFX: Vollstaendige Anbindung von Ambient Particles an Gothics native [ENGINE]/noAmbientPFX-Option via zCOption.h Hook und SyncAmbientParticlesOption in ImGuiShim.cpp. Die Zuordnung der Partikeleffekte erfolgt 1:1 durch Gothics m_bIsAmbientPFX-Feld.
- Dynamic Clouds Sonnen-Transmission: Ueberarbeitung des Sonnenblocks in PS_PFX_LowClouds.hlsl mit realistischer Transmission (Sonnenkern 18 %, Halo 6 %) und sanfter Wolken-Rueckseiten-Durchleuchtung (backlitCloudColor).
- Pruefung: Statische Diff-, HLSL-String-, Inhalts-, Zeilenzahl-, Byte- und SHA-256-Hash-Pruefung lokal und direkt im OneDrive-Backup-Ziel; kein vollstaendiger lokaler C++-Build.

## Build 155

- Partikelbeleuchtung: Trennung von Nacht-RGB-Dimmung (nightDim) und Regen-Alpha-Dimmung (rainAlpha) in PS_ParticleSimple.hlsl und PS_ParticleDistortion.hlsl. Rauch bleibt ueber particleLightingScale (1.0) staerker betroffen als Wasserpartikel (0.25).
- F11-Menue & Presets: Water Reflections steht genau einmal direkt nach HDR Tone Mapping und bleibt aus allen Presets ausgeschlossen. Dynamic Clouds und Ambient Particles stehen direkt unter Depth of Field und sind vollstaendig in Presets integriert (Low=false, sonst true). Ambient Particles deaktiviert bei false ausschliesslich atmosphaerischen Ground Fog.
- Vegetationsverdraengung: Der F11-Regler steuert HeroAffectsObjectsRadius (Display -> HeroAffectsObjectsRadius), welche den Such- und Verdraengungsradius um den Spieler skaliert.
- Dynamic Clouds Optimierung: Schleifeninvariante Werte vor die Raymarching-Schleife verlagert, exakt leere Dichte-Samples (density <= 0.0f) per continue uebersprungen und Schleife bei voller Deckkraft (transmittance <= 0.001f) per break beendet. Exakt 8 Marching-Schritte und alle visuellen Details bleiben 100% erhalten.
- Pruefung: Statische Diff-, HLSL-String-, Inhalts-, Zeilenzahl-, Byte- und SHA-256-Hash-Pruefung lokal und direkt im OneDrive-Backup-Ziel; kein vollstaendiger lokaler C++-Build.

## Build 154

- **DoF Composite Shader Fixes (PS/CS_PFX_DoF_Composite.hlsl)**: `GetSkyEdgeBlurSample`-Aufruf im `IsSkyDepth`-Block ergÃ¤nzt und `OutputComposite` UAV-Register (`u0`) wiederhergestellt.
- **Wasser-Reflection 39Â°/50Â°-Kurve & Sky-Reflection (PS_Water.hlsl)**: NeigungsabhÃ¤ngige Ausblendung auf 39Â°â€“50Â° zurÃ¼ckgestellt (`0.64278761f` / `0.77714596f`) und Low-Cloud-Komposition Ã¼ber `skyBase` wiederhergestellt.
- **Ocean-Regenfarben & Tint-Anpassungen (PS_Water.hlsl)**: Ocean-Nachtregen-Fallback und Volume-Grading auf kÃ¼hles Blaugrau (`0.86f, 0.99f, 1.16f`) umgestellt; Ocean-Tint bei Regen weich auf 35% abgeschwÃ¤cht.
- **Weltbezogene Ocean-Profile (GothicGraphicsState.h)**: Standard-Wasserfarben und StÃ¤rken fÃ¼r OldWorld (`0.72, 0.82, 0.84`, `0.65f`), NewWorld (`0.78, 0.90, 0.92`, `0.55f`) und AddonWorld (`0.72, 0.88, 0.95`, `0.0f`) hinterlegt.
- **GSky Savegame & OldWorld Fix (GSky.cpp)**: Statisches Makro in `LoadSkyResources()` durch dynamische `DaySkyTexture`-PrÃ¼fung (`ST_OldWorld` vs `ST_NewWorld`) und `ApplyDaySkyColorProfile()` ersetzt.
- **SSR 5-Tap Kreuzfilter (PS_Water.hlsl)**: SSR-Treffersampling durch tiefengefÃ¼hrten 5-Tap-Kreuzfilter zur Weichzeichnung von Reflexionskanten erweitert.

## Build 153

- **Steilwasser-SSR/Cubemap-Ausblendung (PS_Water.hlsl)**: NeigungsabhÃ¤ngige Ausblendung auf 33Â°â€“67Â° korrigiert (`smoothstep(waterfallSsrOffCos, waterfallSsrFullCos, waterGeometryUp)`).
- **Entfernung alter Wasserfall-Renderpfad (D3D11GraphicsEngine.cpp/.h)**: `FrameTransparencyMeshesWaterfall`, `waterfallTransparencyMeshes` und der Rendergraph-Pass `Draw FrameTransparencyMeshesWaterfall` vollstÃ¤ndig entfernt; `MT_WaterfallFoam`-Geometrie lÃ¤uft nun durch die normale Transparenzsortierung.
- **RÃ¼cknahme DoF-Reactive-Mask & Shader-Fixes (D3D11PFX_DepthOfField.cpp/.h, CS/PS_PFX_DoF_Composite.hlsl, D3D11PfxRenderer.cpp/.h)**: DoF-Reactive-Mask-Erweiterung vollstÃ¤ndig zurÃ¼ckgenommen, UAV-Deklaration `OutputComposite` (u0) in `CS_PFX_DoF_Composite.hlsl` ergÃ¤nzt und `GetSkyEdgeBlurSample`-Aufruf in `PS_PFX_DoF_Composite.hlsl` korrigiert.

## Build 151

- **Entfernung wirkungsloser Wasserfall-Marker (D3D11GraphicsEngine.cpp & PS_Water.hlsl)**: Marker-Funktionen (`IsWaterfallTexture`, `IsWaterTextureExcludedFromSSR`, `TextureNameContainsMarker`) und wirkungsloser `else if (isWaterfall > 0.5f)` Shader-Pfad entfernt. `WaterMaterialInfoConstantBuffer` auf 48 Byte angepasst. Steile WasserflÃ¤chen nutzen die Legacy-Wasserdarstellung.
- **Ocean Edge Sky Smoothing (PS_Water.hlsl)**: Sanfte Angleichung dunkler seitlicher Ocean-RÃ¤nder an weiter innen liegende Himmelsspiegelungsproben (`oceanSideSkyBlend`).
- **SSR & Cubemap Steilwasser-Ausblendung (PS_Water.hlsl)**: Ausblendung von SSR und Cubemap auf stark geneigtem Wasser harmonisiert (`cubeStrength*=steepWaterSsrFactor`).

## Build 150

- **Wasser- und SSR-Korrekturen (PS_Water.hlsl)**: Ocean edge fade vertikal geglÃ¤ttet, Legacy-Nachtverdunkelung angepasst, SSR auf steilem Wasser ausgeblendet, Wasserfall-Mischungen korrigiert und ssrActive nach der Neigungsreduktion berechnet.
- **DoF-Maskierung & RenderGraph Safety**: DoF-Pass maskiert SSR- und Specular-FlÃ¤chen Ã¼ber WaterMask/SpecularMask SRVs. RenderGraph gegen ungÃ¼ltige Handles abgesichert (IsHandleRegistered mit GetHandleIndex).

## Build 149

- **Umstrukturierung**: Die "include"-Ordner wurden in "Include" umbenannt.
- **Hook Safety (zCWorld.h)**: Die Hook-Funktionen hooked_zCWorldDisposeVobs und hooked_LoadWorld wurden Ã¼berarbeitet, um direkte Rohzugriffe auf Engine::GAPI->GetLoadedWorldInfo()->MainWorld zu vermeiden und stattdessen sichere, null-geprÃ¼fte Zugriffe Ã¼ber  uto* worldInfo zu verwenden.

## Build 148

- **Codebase Cleanup & Humanisierung**: Redundante Kommentare und KI-Auffaelligkeiten in C++- und HLSL-Dateien bereinigt. Ordnerstruktur auf einheitliches Include umgestellt.
- **Shader Formatierung (PS_Water.hlsl)**: Wasserfall-Shader-Logik formatiert und bereinigt.
- **Diagnosetest FSR3 Contact Shadows**: Testweise Umgehung der Albedo-Alpha-Indoor-Klassifizierung unter FSR3 in PS_PFX_ScreenSpaceLightingTrace.hlsl und PS_PFX_ScreenSpaceLightingTemporal.hlsl.

## Build 147 (Dedizierte C++ Render-Pipeline fÃƒÂ¼r WasserfÃƒÂ¤lle)

- **C++ Engine (D3D11GraphicsEngine.cpp)**: WasserfÃƒÂ¤lle (OWODWAT, WATERFALL, WASSERFALL) rendern jetzt ÃƒÂ¼ber eine vollstÃƒÂ¤ndig entkoppelte und exklusive Render-Pipeline. Sie umgehen jegliche Standard-Wasser- oder Material-Shader (PS_Water / MT_Water).
- Stattdessen wird nun gezielt PS_Simple und VS_Ex verwendet, wodurch sÃƒÂ¤mtliche (teils unerwÃƒÂ¼nschte) Screen-Space-Reflektionen (SSR), Cubemap-Spiegelungen, Normal-Maps und Specular-Eigenschaften fÃƒÂ¼r animierte WasserfÃƒÂ¤lle hart und sicher auf Engine-Ebene ausgeschlossen werden.

## Build 146 (Wasserfall Animations- und Material-Fixes)

- **C++ Engine**: WasserfÃƒÂ¤lle (OWODWAT, WATERFALL, WASSERFALL) verwenden nun konsistent das MT_WaterfallFoam Material anstelle des Standardwasser-Shaders (MT_Water).
- **Animierte Texturen**: Die Engine prÃƒÂ¼ft jetzt korrekt auf animierte Texturen (GetAniTexture()) bei der Wasserfall-Erkennung, sodass animierte WasserfÃƒÂ¤lle nun korrekt aus dem generischen PS_Water- und Wasser-SSR-Pfad ausgeschlossen werden.

## Build 145 (Waterfall-Override und lokale Fixes)

- **PS_Water.hlsl**: Dedizierter else if (isWaterfall > 0.5f) Block hinzugefÃƒÂ¼gt, um WasserfÃƒÂ¤lle vom regulÃƒÂ¤ren Ocean/Legacy-Refraction- und Fresnel-Handling abzutrennen und SSR/Cubemap explizit zu ÃƒÂ¼berschreiben.
- **D3D11GraphicsEngine.cpp**: Weitere C++-seitige Logikanpassungen fÃƒÂ¼r Wasser und Fallbacks durch den Benutzer lokal integriert.

## Build 144 (Lokales Refactoring)

- **PS_Water.hlsl**: Umfassendes manuelles Refactoring und StrukturÃƒÂ¤nderungen (Ocean/Legacy Code-BlÃƒÂ¶cke reorganisiert).
- **C++ Engine & Header**: Lokale ÃƒÂ„nderungen in D3D11GraphicsEngine.cpp und AtmosphericScattering.h durch den Benutzer integriert.

## Build 142 (Stabile Wolkenkanten und globale Beleuchtung wie Build 139)

- Dynamische Wolken: unsichere halbaufgeloeste Tiefensamples an weit entfernten, alpha-getesteten Baumkronen werden nicht mehr als einzelner Treffer auf volle Deckung normalisiert; ein begrenzter tiefen- und raumgewichteter 5x5-Rekonstruktionsfilter stabilisiert die Wolkendeckung gegen gruene flackernde Blattpixel.
- Wolken-Tiefenregeln: Sky und Geometrie bleiben strikt getrennt, solide Geometrie erhaelt keinen Sky-Farbfallback und Wolken koennen weiterhin raeumlich korrekt vor Weltgeometrie und VOBs liegen.
- Tages-/Nachtbeleuchtung: die NW_CITY_WINDOW-Sonderregel und ihre BSP-/VOB-Scans sowie GBuffer-Marker wurden vollstaendig entfernt; globale Tagesaufhellung, Nachtaufhellung, Mondlicht, SSS und atmosphaerische Einfaerbung entsprechen wieder Build 139.
- Indoor-Klassifizierung: Punktlichter sowie die eigenstaendige Indoor-Begrenzung von SSGI/Contact Shadows verwenden wieder die klassische Build-139-Alpha-Schwelle; andere Build-141-Fixes fuer Meerwasser/SSR und den Regen-Sonnenfade bleiben erhalten.
- Pruefung: Build 139 gezielt als Referenz verglichen; elf Beleuchtungs-/GBuffer-Dateien stimmen byte-identisch mit Build 139 ueberein; Wolkenfilter, Sky-/Geometrie-Trennung, Ocean-Praefix/-Koerper, gemeinsamer SSR-Abschluss, Regen-Sonnenprofil, Aufrufer, Escape-Artefakte und `git diff --check` statisch kontrolliert. Kein vollstaendiger lokaler C++-/Shader-Build.

## Build 142
- Modifizierte D3D11GraphicsEngine.cpp: 'OWODWAT' wird von SSR ausgeschlossen.
- Modifizierte PS_Water.hlsl: Dedicated Waterfall-Override hinzugefuegt.
- Modifizierte PS_Water.hlsl: Legacy-Wasser um Volumetric Murkiness und Shoreline Foam erweitert.
- Modifizierte PS_Water.hlsl: Ocean SSR Contact Fade Artifact Fix uebernommen.

## Build 141 (Indoor-Beleuchtung, Meerwasser, dynamische Wolken und Regen-Sonne)

- Indoor-/Outdoor-Abgrenzung: reservierte GBuffer-Marker ersetzen die mit dunklen Outdoor-Vertexfarben kollidierenden Build-140-Werte; die Sonderregel greift nur bei tatsaechlichen Indoor-Receivern, waehrend Outdoor sowie Raeume mit `NW_CITY_WINDOW*` die globale Tages-/Nachtaufhellung und Einfaerbung behalten.
- Indoor-Erkennung: pauschale BSP-Leaf- und Bounding-Box-VOB-Scans wurden entfernt; im Outdoor-BSP werden nur lightmapped Worldpolys kontrolliert und VOBs nur ueber ihren tatsaechlichen Indoor-Status markiert.
- Meerwasser: Texturen mit Praefix `NW_WATER_LAKE` erhalten wieder den blauen Ocean-Volumenkoerper aus Build 139, unabhaengig von der optionalen Tint-Staerke; Reflexionen bleiben im gemeinsamen, tiefen-/kanten-/hitqualitaetsgesicherten Build-140-SSR-Pfad gegen NPC-/Objektartefakte.
- Dynamische Wolken: tiefenkompatible Wolkensamples koennen wieder vor Landschaft und Weltgeometrie liegen; Sky-/Geometrieklassen und relative Tiefen werden getrennt, damit Baumkanten keinen Sky-/Cloud-Hintergrund vermischen.
- Regen-Sonne: das vollstaendige Mie-/Sonnenprofil behaelt seine feste Groesse und wird nach der Phasenberechnung nur in der Amplitude bis exakt null ueberblendet; beim Regenende erscheint es ueber dieselbe Kurve wieder, ohne einen verbleibenden Punkt.
- Pruefung: Build 139 und 140 gezielt als Referenzen verglichen; `origin/master` war Fast-Forward-kompatibel; Aenderungsumfang, GBuffer-Marker, Indoor-Klassifizierung, Ocean-Praefix/-Koerper und gemeinsamer SSR-Abschluss, Cloud-Tiefenklassen, Sonnenprofil-Regenfade, Shader-Klammern, Escape-Artefakte und `git diff --check` statisch kontrolliert. Kein vollstaendiger lokaler C++-/Shader-Build; finale Renderpruefung erfolgt im Spiel.

## Build 140 (Arbeitsbuild)

- Arbeitsbuild aus dem gepushten Build-139-Stand 82d6e67 angelegt; weitere Aenderungen folgen in diesem Build.
- Indoor-Tageslicht: die bisherige kuenstliche Tagesaufhellung greift in Indoor-Raeumen nur noch, wenn ein NW_CITY_WINDOW*-VOB/Visual im Raum oder innerhalb der 30f-Toleranz erkannt wird; Worldmesh, VOBs, MOBs und Skeletal-Receiver nutzen denselben Marker.
- SSGI/Contact Shadows: Screen-Space-GI und Kontaktschatten werden per Albedo-GBuffer-Maske nur noch fuer Indoor-Receiver inklusive 30f-Aussentoleranz berechnet; Outdoor-Pixel laufen im Trace/Temporal fruehzeitig auf 0.
- F11-Menue: die Tooltips fuer Screen Space GI und Contact Shadows weisen knapp darauf hin, dass beide Effekte nur indoor wirken.
- Pruefung: Register-/Aufrufer-Bindings fuer Screen-Space-Lighting, Indoor-Daylight-Polymarker, PowerShell-Escape-Artefakte und git diff --check statisch kontrolliert. Kein vollstaendiger lokaler C++-/Shader-Build.

## Build 139 (Wasserreflektionen, Regen-/Cloud-Stabilisierung und Materialdaten)

- Wasser: `NW_WATER_LAKE01` bleibt im Ocean-Wasserpfad; Wasserreflektionen und kameranahe Objekt-/NPC-Kontakte werden stabilisiert, ohne auf den alten Legacy-Wasserpfad zurueckzugehen.
- Wasser/Regen/Nacht: Meerwasser wird bei Regen staerker an den grauen beziehungsweise blau-dunklen Wetter-/Nachtschleier gebunden; der Atmosphere-Unterhorizont wird tiefer gehalten, damit Sky-Farben nicht zu frueh orange/schwarz durch das Wasser laufen.
- Wasserfaelle: Wasserfall-Foam wird aus Distanz dauerhaft angefordert, damit Wasserfalltexturen nicht erst nah an der Kamera erscheinen.
- Regenhimmel/Clouds: der Sonnenspot wird bei Regen komplett ueber `AC_SunVisibility` ausgeblendet; dynamische Wolken bleiben optisch erhalten, werden nachts aber weich vom Horizont angehoben, und diskrete Regen-Cloud-Sample-Spruenge wurden entfernt.
- Rain Ground SSR: Bodenreflektionen nutzen die GBuffer-Specular-Werte als Materialfilter; bewegtes Wabern wurde entfernt und durch feines Regen-Krizzeln ersetzt.
- Materialien: `system\GD3D11\textures\materials.json` wurde als bereinigte Fallback-Datenbank eingebaut; Wasser, Foam, Alpha/Vegetation und IceDragon-Schnee erzeugen kein Rain-Ground-SSR, und Materialwerte werden im Loader gekappt.
- Normal-/Displacementmaps: echte Normalmaps werden nur genutzt, wenn das Material sie erlaubt; der alte Wet-Distortion-Normalfallback ist abgeschaltet, `displacementFactor=0` bleibt wirklich 0, und Displacementmaps werden bei 0 nicht geladen/gebunden.
- Pruefung: AGENTS-Regeln gelesen; Buildnummer aus outputs ermittelt; Materialien-JSON geparst und auf Wertebereiche, Keys, IceDragon-/Wasser-SSR-Treffer geprueft; Material-/Normal-/POM-Bindings, Wet-Ground-SSR-Bindings, Shader-Escape-Artefakte und git diff --check statisch kontrolliert. Kein vollstaendiger lokaler C++-/Shader-Build.

## Build 138 (LowClouds in Wasser-SSR/Godrays und Wasserlook aus Build 134)

- LowClouds: dynamische Wolken werden vor Wasser und Godrays als eigene Layer-/Depth-Ressource erzeugt und spaeter tiefenbewusst ueber das Bild komponiert.
- Wasser-SSR: der Wasser-Pixelshader uebernimmt den Build-134-Wasserlook und kann LowClouds in SSR-Treffern reflektieren; die bestehende WaterMaterialInfo-Logik fuer Ocean-Tint, Wasserfall-SSR-Sperren und Regenmasken bleibt erhalten.
- Godrays: LowClouds werden in Pixel- und Compute-Godray-Masken beruecksichtigt, damit Wolken die Lichtstrahlen sichtbar formen koennen.
- LowCloud-Kanten: entfernte alpha-getestete Vegetation bekommt beim LowCloud-Compositing einen konservativen Farbfallback, damit an Baumkonturen nicht roher Sky durchscheint, wenn Wolken dahinter liegen.
- Konsistenz: der seltene Standalone-Godray-Fallback verwendet wieder den Depth-SRV statt des alten Normal-SRV-Parameters.
- Pruefung: AGENTS-Regeln gelesen; Buildnummer aus outputs ermittelt; Wasser-/LowCloud-/Godray-Bindings, WaterMaterialInfo-Pfade, Merge-Marker, PowerShell-Escape-Artefakte und git diff --check statisch kontrolliert. Kein vollstaendiger lokaler C++-/Shader-Build.
- Korrekturpush: ungueltige `IsValid()`-Pruefungen auf RenderToTextureBuffer/RenderToDepthStencilBuffer durch vorhandene Texture-/View-Pruefungen ersetzt; Wasser-Shader ist wieder byte-identisch zu Build 134.
- Korrekturpush: Wasser-SSR ist wieder naeher am Build-132-Fallback abgestimmt; rohe Sky-/Sonnen-/Mond-Screenhits werden nicht mehr als harte Wasserreflektion genutzt, LowClouds laufen nur bei aktiven Wasserreflektionen in den SSR-Pfad, und Nicht-`NW_WATER_LAKE01`-Wasser bleibt beim 132-artigen Wasserlook.
- Korrekturpush: Regenwasser bei Tag/Nacht wurde grauer/blauer abgestimmt, LowCloud-Baumkonturen und der LowCloud-Horizont-Fill wurden nachgezogen, und LowClouds maskieren Godrays bei tief stehender Sonne staerker.
- Korrekturpush: der Resize-Pfad selbst bleibt unveraendert; Texture-Pool-Clear entfernt keine aktiven Targets mehr, um den R6025-Absturz beim Aufloesungswechsel zu vermeiden.
- Korrekturpush: `D3D11PFX_GodRays.cpp` verwendet fuer die Atmosphere-CB-Abfragen wieder einen nicht-const `GSky`-Pointer, damit `Release_G1_AVX` nach dem Godray-LowSun-Boost kompiliert.

## Build 137

- Reset auf Build 132 als stabile Basis.
- Build 134/136-Render-, Sky-, Wetter-, FSR3- und Shader-Portierungen bewusst verworfen.
- Keine Startpfad-, Launcher-, DirectDraw-, Hooking-, Detours- oder Memory-Patching-Aenderungen aus 134 uebernommen.
- F11-Menue: Vegetations-/Wasser-/Regenoptionen bereinigt, Wasserreflektionen und Regendarstellung umbenannt, D3D11-Version 18.0 angezeigt und Presets auf sichtbare Optionen unterhalb der Trennlinie begrenzt.
- Vegetation: Objektinteraktion wird automatisch ueber Windeffekte gesteuert, Backlight bleibt dauerhaft aktiv und der Interaktionsradius entspricht 0,5 m.
- Regen/Wetter: Savegame-Laden setzt den Wetterzustand stabil zurueck, RainClouds bleiben bei Regen sichtbar und Regen klingt mit Wolken-/Sonnen-Rueckkehr sauber ab.
- FSR3/Contact Shadows: Contact Shadows bleiben unter FSR3 sichtbar, werden dort abgeschwaecht und NPC-/Gesichtsreceiver werden deutlich entschaerft beziehungsweise ausgeschlossen.
- Transparenz/Wet Ground SSR: transparente Weltgeometrie bekommt eine eigene Wet-Blocker-Maske, damit Regen-/Nachtflackern und Durchschimmern durch solide Geometrie reduziert werden, ohne funktionierende Sperrmasken zu entfernen.
- Dynamische Wolken/Wasser/Schatten: Wolken-Compositing nutzt tiefenbewusstes Upsampling gegen helle/dunkle Objektkanten, Wasser ist bei Tag/Nacht/Regen neutraler abgestimmt und NPC-nahe Schatten werden stabiler weichgefiltert.
- Text: der deutsche F11-Hinweis nutzt CP1252-kompatible Umlaute fuer Gothics Textausgabe.
- Pruefung: AGENTS-Regeln gelesen; Buildnummer aus outputs ermittelt; Projekt-XML, Shader-/Projektpfade, Shaderregistrierungen, F11-/INI-/Preset-Pfade, Regen-/RainCloud-Pfade, FSR3-/Contact-Shadow-Gates, Transparenz-/Wet-SSR-Bindings, Escape-Artefakte und git diff --check statisch kontrolliert. Kein vollstaendiger lokaler C++-/Shader-Build.

## Build 132 (Performance-Basis, F11-Aufraeumen und interne Schattenfilter-Auswahl)

- Aufraeumen: die nicht erfolgreichen Occlusion-Culling- und Motion-Blur-Systeme wurden vollstaendig aus Code, Shaderregistrierung, Projektdateien, F11-Menue und INI-Persistenz entfernt.
- Performance: Deferred-Z-Prepass ist als Standard aktiv; FSR3-Velocity-/Reactive-/Transparency-Masken werden in Deferred und Forward+ nur noch bei aktivem FSR3 als MRTs erzeugt und gebunden.
- Schattenfilter: die F11-Option `Shadow Filter` wurde entfernt; PCSS bleibt intern Standard, Simple PCF wird nur als Feature-Level-10 beziehungsweise Shadow-Atlas-Fallback verwendet und alte INI-Werte werden ignoriert.
- Settings: tote `DrawThreaded`-Einstellung wurde entfernt; `SortRenderQueue` bleibt unveraendert.
- Grenzen: Render Scale, Aspect-/Viewport-, Kamera- und echte Aufloesungswechsel-Pfade bleiben unveraendert.
- Pruefung: AGENTS-Regeln gelesen; Buildnummer aus outputs ermittelt; Kirides-Nightly/17.9.7 fuer Performance-Pfade verglichen; Occlusion-/MotionBlur-/ShadowFilter-Reste, Projekt-XML, Render-Scale-/Aspect-Grenze, Escape-Artefakte und git diff --check statisch kontrolliert. Kein vollstaendiger lokaler C++-/Shader-Build.

## Build 131 (HZB-Occlusion-Snapshot, konsistente VOB-Schatten und Motion-Blur-Sampling)

- Occlusion: die HZB-Auswertung nutzt nun einen stabilen Readback-Snapshot vom vorher abgeschlossenen Frame; neue HZB-Daten werden erst nach dem World-Depth-Pass fuer den naechsten Frame erfasst, damit Shadow- und Main-Collect nicht gegen wechselnde Tiefendaten laufen.
- Occlusion: die Bounding-Box-Projektion nutzt die zur Renderer-CPU-Projektion passende Projection-mal-View-Reihenfolge; wenn kein frischer Readback verfuegbar ist, wird konservativ nicht gecullt.
- Schatten: kleine VOBs/Mobs, die durch dieselbe HZB-Entscheidung ausgeblendet werden, werden auch aus Sun- und Pointlight-Shadow-Collects genommen; grosse VOBs bleiben weiterhin von Occlusion ausgeschlossen.
- Motion Blur: das Sampling ist symmetrisch um den aktuellen Pixel statt einseitig in Richtung vorheriger Pose, die horizontale stabile Zone ist breiter und der Uebergang zu den Raendern weicher.
- Aufraeumen: die nicht erfolgreichen Main-View-Frame-Stamp-/Shadow-Kopplungen aus dem vorherigen Versuch sind entfernt.
- Grenzen: Render Scale, Aspect-/Viewport-, Kamera- und echte Aufloesungswechsel-Pfade sowie DoF bleiben unveraendert.
- Pruefung: AGENTS-Regeln gelesen; Buildnummer aus outputs ermittelt; origin/master mit PortableGit/OpenSSL abgeglichen; Occlusion-/Shadow-Pfade, Render-Scale-/Aspect-Grenze, Shader-Escape-Artefakte und git diff --check statisch kontrolliert. Kein vollstaendiger lokaler C++-/Shader-Build.

## Build 130 (HZB-Occlusion-Fix, konsistente VOB-Schatten und Motion-Blur-Maske)

- Occlusion: die HZB-Bounding-Box-Projektion nutzt dieselbe Projection-mal-View-Reihenfolge wie der restliche Renderer, damit sichtbare VOBs nicht durch falsche Clip-Projektion verschwinden.
- Schatten: VOBs, die durch die Main-View-Occlusion ausgeblendet werden, werden im Shadow-Collect ebenfalls uebersprungen; grosse nicht-occludable VOBs bleiben davon unberuehrt.
- Motion Blur: die stabile Bildzone ist horizontal breiter und der Uebergang zu den geblurten Raendern weicher; kleine Restgeschwindigkeiten werden gedaempft, um Nachziehen nach Kamerastopps abzuschwaechen.
- Grenzen: Render Scale, Aspect-/Viewport-, Kamera- und echte Aufloesungswechsel-Pfade bleiben unveraendert.
- Pruefung: AGENTS-Regeln gelesen; Buildnummer aus outputs ermittelt; origin/master mit PortableGit/OpenSSL abgeglichen; Occlusion-/Shadow-Pfade, Render-Scale-/Aspect-Grenze, Shader-Escape-Artefakte und git diff --check statisch kontrolliert. Kein vollstaendiger lokaler C++-/Shader-Build.

## Build 129 (Konservative HZB-Occlusion, DoF-Rueckbau und Motion-Blur-Randlook)

- Occlusion: die HZB-Reduktion nutzt fuer reversed-Z nun konservative Mindest-Tiefen statt naechster Max-Tiefen, damit einzelne nahe Pixel keine sichtbaren VOBs/Vegetation ueber ganze Kacheln wegcullen; das versteckte Tiny-Screen-Culling unter derselben Option ist entfernt.
- DoF: die Depth-of-Field-Shader sind wieder auf den Build-127-Look zurueckgesetzt, inklusive vorheriger CoC-/Sky-Edge-Logik.
- Motion Blur: Randmaske, Heldenbereich, Samplingrichtung und Konstanten folgen wieder Build 127; die aktuelle depth-aware Kantenabsicherung gegen Silhouettenartefakte bleibt erhalten.
- Grenzen: Render Scale, Aspect-/Viewport-, Kamera- und echte Aufloesungswechsel-Pfade bleiben unveraendert.
- Pruefung: AGENTS-Regeln gelesen; Buildnummer aus outputs ermittelt; origin/master mit PortableGit/OpenSSL abgeglichen; DoF-Shader gegen Build 127 verglichen; HZB-/Tiny-Cull-, Motion-Blur- und Shader-Escape-Artefakte sowie git diff --check statisch kontrolliert. Kein vollstaendiger lokaler C++-/Shader-Build.

## Build 128 (DoF-Kanten, Motion-Blur-Stabilitaet, HZB-Occlusion, Schatten-Updates und Wasser-SSR-Rueckbau)

- DoF: Vorder- und Hintergrundunschaerfe nutzen eine kleine CoC-Erweiterung, damit geblurte Objektkanten weniger hart ausgeschnitten wirken, waehrend die bestehende Sky-Maske erhalten bleibt.
- Motion Blur: Staerke und maximale Laenge sind reduziert; Velocity-Dilation und Tiefenkanten-Gewichtung sollen Helden-/Silhouettenartefakte und weisse Uebergangsbereiche abschwaechen.
- Occlusion: das alte BSP-Predicate-/Query-Culling wurde entfernt; stattdessen baut der Renderer nach dem Welt-Depth-Pass eine kleine Depth-Hierarchie und cullt nur kleine VOBs/Mobs konservativ pro Bounding-Box. Wenn der asynchrone Readback nicht frisch verfuegbar ist, bleibt alles sichtbar.
- Schatten: entfernte Outdoor-Schatten-Cascades werden bei relevanter Kamera-Bewegung oder -Drehung sofort aktualisiert.
- Wasser-SSR: der nicht erfolgreiche Vordergrund-Occluder-/Thin-Fill-Fix wurde wieder auf den Build-124-Wasser-SSR-Stand zurueckgesetzt, ohne Wet-Ground-SSR anzufassen.
- Grenzen: Render Scale, Aspect-/Viewport- und echte Aufloesungswechsel-Pfade bleiben unveraendert.
- Pruefung: AGENTS-Regeln gelesen; Buildnummer aus outputs ermittelt; origin/master mit PortableGit/OpenSSL abgeglichen; PS_Water.hlsl gegen Build 124 verglichen; HZB-/Occlusion-Projektpfade, alte Query-Reste, SSR-Fallback-Marker, Render-Scale-/Aspect-Grenze, Shader-Escape-Artefakte und git diff --check statisch kontrolliert. Kein vollstaendiger lokaler C++-/Shader-Build.

## Build 127 (Motion-Blur-Heldenschutz und Wasser-SSR Thin-Occluder-Fill)

- Motion Blur: die stabile Zone schuetzt nun zusaetzlich den unteren mittigen Heldenbereich, statt nur exakt um die Bildmitte zu liegen.
- Wasser-SSR: duenne Vordergrund-Occluder wie Seile oder Pfosten bekommen einen kleinen bilateralen Nachbar-Fill aus direkten SSR-Trefferumgebungen; breite Occluder bleiben beim bisherigen Cubemap-Suppression-Fallback.
- Grenzen: Nebel/Fog, Render Scale, Aspect-/Viewport- und Aufloesungswechsel-Pfade bleiben unveraendert.
- Pruefung: AGENTS-Regeln gelesen; Buildnummer aus outputs ermittelt; origin/master mit PortableGit/OpenSSL abgeglichen; Fog-/Render-Scale-/Aspect-Grenze, Shader-Escape-Artefakte und git diff --check statisch kontrolliert. Kein vollstaendiger lokaler C++-/Shader-Build.

## Build 126 (Dynamic-Cloud-Regen, Motion Blur, Wasser-SSR und Vegetationsradius)

- Dynamic Clouds: Tiefwolken blenden bei Regen gegen die Raincloud-Textur aus, werden im Composite auch vom Nachtnebel mitgenommen und der Low-Cloud-Pass wird bei starkem Regen uebersprungen.
- Regen-Schleier: der globale Regenschleier bleibt ohne Dither, bekommt aber eine raeumlich weichere Wirkung ueber die bestehende Fog-Maske.
- FSR3/Contact Shadows: Contact Shadows nutzen bei aktivem FSR3 wieder die reduzierte Staerke `0.35`, waehrend andere Modi beim Standard `0.50` bleiben.
- Motion Blur: die Bildmitte bleibt stabiler und Bewegungsunschaerfe nimmt zu den Raendern zu; Tiefenkanten werden strenger gewichtet, um Helden-/Silhouettenartefakte zu reduzieren.
- Wasser-SSR: Vordergrundobjekte lassen den Cubemap-Fallback an den betroffenen Wasserstellen weniger stark durchbrechen, ohne Wet-Ground-SSR anzufassen.
- Vegetation: der Interaktionsradius fuer Hero/NPC-Vegetation ist kleiner, waehrend Staerke und gaussscher Uebergang unveraendert bleiben.
- Pruefung: AGENTS-Regeln gelesen; Buildnummer aus outputs ermittelt; origin/master mit PortableGit/OpenSSL abgeglichen; Render-Scale-/Aspect-Grenze, Shaderpfade, Dither-/Escape-Artefakte und git diff --check statisch kontrolliert. Kein vollstaendiger lokaler C++-/Shader-Build.

## Build 125 (Wasser-SSR, Regenwolken, FSR3-Masken und 4:3-Seitenverhaeltnis)

- Wasser-SSR: Vordergrundobjekte vor Wasserreflexionen reduzieren nur die betroffenen Screen-Space-Treffer weich, damit fehlender Hintergrund kaschiert wird, ohne Wet-Ground-SSR anzufassen.
- Regen-Schleier: der globale Regen-/Sky-Schleier ist ohne Dither, beginnt etwas naeher und laeuft weicher/laenger in die maximale Wirkung; tagsueber bleibt die Regenwolken-Textur sichtbar abgeschwaecht erhalten.
- Dynamische Wolken: Low Clouds werden im Composite vom gleichen Regen-/Nacht-Schleier mitgenommen und laufen bei Regen beziehungsweise starker Nacht mit weniger Raymarch-Schritten, waehrend klare Tageswolken ihre volle Schrittzahl behalten.
- FSR3: Alpha-getestete Weltflaechen und Contact Shadows schreiben gezielte Transparency-/Composition-Masken fuer die Rekonstruktion; die Contact-Shadow-Staerke ist wieder der normale Standardwert.
- Seitenverhaeltnis: der fehleranfaellige World-Projection-Aspect-Hack ist entfernt; echte Aufloesungswechsel aktualisieren die Gothic-Kamera wieder direkt, Render Scale bleibt davon getrennt.
- Pruefung: AGENTS-Regeln gelesen; Buildnummer aus outputs ermittelt; origin/master mit PortableGit/OpenSSL abgeglichen; Projekt-XML, Shader-/Projektpfade, FSR3-Maskenfluss, Render-Scale-Grenze, Konflikt-/Escape-Artefakte und git diff --check statisch kontrolliert. Kein vollstaendiger lokaler C++-/Shader-Build.

## Build 124 (Nachtregen-Schleier, Aufloesungswechsel, NPC-Vegetation, Motion Blur und TAA-Rueckbau)

- Nachtregen/Tagregen: der Regen-Schleier liegt als sanfter Overlay ueber Welt und Himmel; nachts staerker, tagsueber abgeschwaecht, damit entfernte Geometrie und Sky wieder zusammenhaengender wirken.
- Seitenverhaeltnis: echte Aufloesungswechsel synchronisieren Gothic-Viewport und Kamera nach World-Load wieder mit der Backbuffer-Aufloesung, ohne den Render-Scale-Pfad anzufassen.
- Vegetation: der Hero-Affects-Vegetation-Effekt ist wieder naeher am Kirides-Nightly-Verhalten, wirkt aber weiter auch fuer weitere NPCs.
- Motion Blur: ein optionaler F11-Schalter aktiviert Bewegungsunschaerfe als persoenliche Option; Grafik-Presets setzen diese Option nicht.
- TAA/FSR3: TAA wurde aus UI, Shadern und PostFX-Code entfernt; FSR3 nutzt einen eigenen Temporal-State fuer Jitter/Velocity und bleibt als `AA_FSR3 = 2` der einzige FSR3-Anti-Aliasing-Modus.
- Pruefung: AGENTS-Regeln gelesen; Buildnummer aus outputs ermittelt; TAA-/AA-FSR-Reste, Projekt-XML, Shader-/Projektpfade, F11-/INI-Pfade, Konflikt-/Escape-Artefakte und git diff --check statisch kontrolliert. Kein vollstaendiger lokaler C++-/Shader-Build.

## Build 123 (Render Scale, Detail-Menue-Rueckbau und Sky-Farbprofile)

- Render Scale: der Skalierungspfad wurde wieder auf das Build-117-Verhalten ausgerichtet, damit nicht-100-Prozent-Render-Scale keine unterschiedlichen Bildbestandteile fehlerhaft skaliert.
- F11/INI: das Detail-Untermenue wurde entfernt; versteckte Detail-/Entwicklerwerte werden nicht mehr aus normalen User-INI-Eintraegen gelesen und der sichtbare F11-Hauptmenue-Umfang bleibt massgeblich.
- Nachtregen und Wolken: Nachtregen ist naeher am Build-061-Eindruck, Cloud Day/Rain/Night Defaults wurden festgezogen und die Tagwolkenfarbe folgt dem aktiven Sky-Profil.
- Sky/Fog: `Cloud Day Color` und `FogColorMod` haengen am aktiven Tag-Sky-Profil; OldWorld/World behalten optisch den G1-Sky-Look, sonstige Welten den SkyDay.dds-Look. OldWorld/World nutzen nachts `NightFogBrightness 0.35`, Standard bleibt `0.70`.
- Pruefung: AGENTS-Regeln gelesen; Buildnummer aus outputs ermittelt; Render-Scale-, F11-/INI-, Sky-/Fog- und Shaderpfade statisch kontrolliert; Konflikt-/Escape-Artefakte und git diff --check geprueft. Kein vollstaendiger lokaler C++-/Shader-Build.

## Build 121 (Detail-Menue, Render-Scale-Fix, Wolken/Wasser-Tuning und Regenpartikel)

- F11: das Standardmenue bleibt erhalten und bekommt nur einen Detail-Button; darin liegen Feinregler fuer Nachtregen, Nacht-/Nebelhelligkeit, dynamische Wolken, Vegetationseinfluss und Meerwasserfarbe.
- Dynamische Wolken: Dichte, Groesse, Hoehe, Reichweite, Geschwindigkeit, Sonnenlicht sowie Tag-/Regen-/Nachtfarbe werden gespeichert und direkt im Low-Cloud-Shader ausgewertet.
- Meerwasser: der Wasserpass kann Meerwasser separat faerben, waehrend Fluss-/Bach-/Wasserfall-Marker ausgeschlossen bleiben; die Standardstaerke ist neutral.
- Seitenverhaeltnis/Render Scale: die Gothic-Kamera wird nur mit der echten Backbuffer-Aufloesung synchronisiert; Render Scale/FSR bleibt intern und greift nicht mehr in die Spielprojektion ein.
- Nachtregen und Partikel: Nachtregen nutzt die dunklere Mittelbereichsfarbe; nicht-emissive Partikel werden bei Regen tagsueber genauso stark abgedunkelt wie nachts, wobei Rauch staerker und Wasserpartikel weiterhin schwaecher reagieren.
- Pruefung: AGENTS-Regeln gelesen; Buildnummer aus outputs ermittelt; Build 117 als Render-Scale-Referenz verglichen; Settings-/INI-Pfade, Wasser-/Wolken-/Partikel-Shader, CBuffer-Layout, Konflikt-/Escape-Artefakte und git diff --check statisch kontrolliert. Kein vollstaendiger lokaler C++-/Shader-Build.

## Build 120 (Load-stabile Projektion, dichtere Wolken und Nachtregen-Mitteldistanz)

- Seitenverhaeltnis/Load: die Session-Kamera schreibt nach echter Aufloesungsaenderung und nach `zCCamera::Activate` wieder die logische Spielaufloesung in den Gothic-Viewport; Render Scale bleibt dabei strikt von der Gothic-Kamera-Projektion getrennt.
- Render Scale/FSR: reine Render-Scale-Aenderungen bauen nur die DX11-Renderziele neu und loesen keine zusaetzliche Gothic-Kamera-Projektionsaktualisierung mehr aus.
- Tiefwolken: Wolkenfelder sind dichter, tagsueber neutraler/weisslicher, bei Regen grauer und nachts etwas dunkler; die langsame world-space Bewegung bleibt erhalten.
- Nachtregen: die mittlere entfernte Weltgeometrie wird bei Regen nachts staerker in dunkles kaltes Blaugrau gezogen, waehrend der sehr ferne Hintergrund nicht weiter abgedunkelt wird.
- Pruefung: AGENTS-Regeln gelesen; Buildnummer aus outputs ermittelt; Kamera-/Viewport-Hooks, Render-Scale-Pfad, Cloud-/Nachtregen-Shader, Konflikt-/Escape-Artefakte und git diff --check statisch kontrolliert. Kein vollstaendiger lokaler C++-/Shader-Build.

## Build 119 (Dynamische Wolken, Nachtregen und robuste Aufloesungswechsel)

- F11: Dynamische Wolken sind ein eigener Schalter mit INI-Persistenz; Himmelseffekte/Sky Effects steuert wieder nur Regen und Regeneffekte.
- Tiefwolken: Wolken laufen unabhaengig vom Regen, sind nachts dunkler, bewegen sich langsamer sichtbar in Weltkoordinaten und verzichten auf die teure globale Wolkenschatten-Abdunklung am Boden.
- Nachtregen: sehr entfernte Weltgeometrie wird bei Regen nachts weiter in ein dunkleres kaltes Blaugrau gedrueckt, ohne in Schwarzgrau zu kippen.
- Seitenverhaeltnis/Render Scale: Gothic-Kamera und zVidResFullscreenX/Y nutzen wieder die echte Spielaufloesung statt der intern skalierten Renderaufloesung; nach Resize wird die Kamera-Projektion erneuert, damit 16:9/4:3-Wechsel und Savegame-Loads nicht zwischen schmal/breit kippen.
- Pruefung: AGENTS-Regeln gelesen; Buildnummer aus outputs ermittelt; Settings-/INI-Pfade, Low-Cloud-Gates, Nachtregen-Shader, Viewport-/Optionspfad, Konflikt-/Escape-Artefakte und git diff --check statisch kontrolliert. Kein vollstaendiger lokaler C++-/Shader-Build.

## Build 118 (Tiefwolken, Nachtregen und stabile 4:3-Projektion)

- Tiefwolken: die Wolkenfelder bekommen staerker lokale Hoehenformen, weichere Oberkanten und dichtere Baenke in mittlerer bis weiter Entfernung, damit weniger flache Wolkendecken und weniger harte Feldkanten entstehen.
- Nachtregen: sehr weit entfernte Weltgeometrie wird bei Regen nachts nochmals dunkler und kuehler blaugrau begrenzt, ohne in Schwarzgrau zu kippen.
- Seitenverhaeltnis: die fehleranfaellige Desktop-/Output-Aspekt-Heuristik fuer die Weltprojektion wurde entfernt; Gothic aktualisiert die Kamera-Projection vor dem DX11-World-Stage, damit 16:9/4:3-Wechsel und Savegame-Loads nicht mehr zwischen schmaler und breiter Darstellung kippen.
- Pruefung: AGENTS-Regeln gelesen; Buildnummer aus outputs ermittelt; Kirides 17.9.7 als Projektions-/Viewport-Referenz verglichen; Shaderpfade, Projektionspfad, Konflikt-/Escape-Artefakte und git diff --check statisch kontrolliert. Kein vollstaendiger lokaler C++-/Shader-Build.

## Build 117 (Nachtregen, Tiefwolken und Windowed-Projektion)

- Tiefwolken: Wolkenfelder sind kamerastabiler, nutzen eine feste Welt-Hoehenschicht, feinere Raymarch-Schritte und koennen Sonne beziehungsweise Mond sichtbar verdecken.
- Wolkenschatten: direkte Sonnen-/Mondlichtwirkung wird bei Wolkenueberdeckung weicher und staerker gedimmt, einschliesslich sichtbarer Lichtscheiben im Himmel.
- Nachtregen: entfernte Weltgeometrie faellt bei Regen nachts in ein dunkleres kaltes Blaugrau mit niedrigerem Luma-Deckel, ohne den Himmel-/Regenwolken-Haze mitzudrehen.
- Windowed-4:3: die Weltprojektionskompensation wird gegen bereits passende Projektionen abgesichert, damit Laden/Resolution-Wechsel keine zu breite Figurendarstellung erzeugt.
- Pruefung: AGENTS-Regeln gelesen; Buildnummer aus outputs ermittelt; Shaderpfade fuer Heightfog/Composition/Low-Clouds, Projektionspfad, Konflikt-/Escape-Artefakte und git diff --check statisch kontrolliert. Kein vollstaendiger lokaler C++-/Shader-Build.

## Build 116 (Low-Cloud-Pass, Shader-Reloads und Windowed-Projektion)

- Tiefwolken: die world-space Tiefwolken laufen nun ueber einen eigenen PostFX-Pass/Shader, statt in Composition/Heightfog mitzustecken; das reduziert die teuren Shader-Recompiles beim Umschalten von Contact Shadows und Screen Space GI.
- Shader-Kategorien: Heightfog und Low Clouds sind als `SkyEffects` kategorisiert, waehrend Contact Shadows/SSGI weiter nur die `Other`-Composition-Permutation betreffen.
- Windowed-4:3: die Weltprojektionskompensation greift nun fuer jeden echten Windowed-Modus mit `StretchWindow=false`, nicht nur fuer den Startup-Windowed-Pfad; HUD/F11 bleiben weiterhin an die Fensterflaeche gekoppelt.
- Pruefung: AGENTS-Regeln gelesen; Buildnummer aus outputs ermittelt; Projekt-XML, Shaderregister, Low-Cloud-Aufrufer, Windowed-Aspektpfad, Konflikt-/Escape-Artefakte und git diff --check statisch kontrolliert. Kein vollstaendiger lokaler C++-/Shader-Build.

## Build 115 (Tiefwolken-Felder, Nachtregen und Windowed-4:3-Projektion)

- Tiefwolken: die vorherige durchgehende Nebelwirkung wurde zu eigenstaendigen, weltkoordinatenbasierten Wolkenfeldern mit sichtbaren Luecken, hoehenversetzten Baenken, einfacher Selbstabschattung und sonnenzugewandter Oberseitenaufhellung umgebaut.
- Wolkenschatten: die Tiefwolken dunkeln entfernte Weltgeometrie weich und breit ab, ohne harte Shadowmap-Kanten und weiterhin unabhaengig vom Regen.
- Nachtregen: der Regennebel bleibt beim Tag naeher am Build-112-Verhalten; nachts wird die entfernte Weltgeometrie dunkler grau statt hellgrau, und der 360-Grad-Haze wirkt weich auch auf den Himmel.
- Windowed-4:3: die horizontale Weltprojektionskompensation gilt nun auch im Windowed-Modus, damit 800x600-Ingame nicht schmal zusammengedrueckt wirkt; HUD/F11 bleiben an die Fensterflaeche gekoppelt.
- Pruefung: AGENTS-Regeln gelesen; Buildnummer aus outputs ermittelt; Projektionspfad, Shaderaufrufe, Konflikt-/Escape-Artefakte und git diff --check statisch kontrolliert. Kein vollstaendiger lokaler C++-/Shader-Build.

## Build 114 (G1-AVX2 Buildfix fuer 4:3-Weltprojektion)

- Buildfix: die 4:3-Weltprojektionskompensation nutzt nun `rendererState.RendererSettings.StretchWindow` statt einer nicht vorhandenen freien `StretchWindow`-Variable, damit Release_G1_AVX2 wieder kompiliert.
- Pruefung: angehaengten GitHub-Buildlog ausgewertet; betroffene D3D11GraphicsEngine-Stelle und vorhandene StretchWindow-Aufrufer kontrolliert; git diff --check statisch kontrolliert. Kein vollstaendiger lokaler C++-/Shader-Build.

## Build 113 (Himmelseffekte, volumetrische Tiefwolken und 4:3-Weltprojektion)

- Himmelseffekte: der bisherige F11-Schalter fuer Regen heisst nun Himmelseffekte/Sky Effects und steuert Regen, Regeneffekte und die neuen tiefen atmosphaerischen Wolkenschichten gemeinsam.
- Volumetrische Tiefwolken: world-space Raymarching fuer dunkle Tal-/Bergnebelkoerper mit Noise-Dichte, Distanzbegrenzung, Regen-/Nachtgewichtung, einfacher Selbstabschattung und Depth-Begrenzung gegen sichtbare Weltgeometrie integriert.
- Nachtregen: entfernte Weltgeometrie wird bei Regen in der Nacht deutlich dunkler und der 360-Grad-Regennebel dichter, damit Silhouetten nicht hellgrau aus dem Nebel leuchten.
- 4:3-Darstellung: World-Rendering bekommt bei gestrecktem Fenster eine horizontale Projektionskompensation, waehrend HUD/F11 wieder die originale UI-Projektion nutzt.
- Pruefung: AGENTS-Regeln gelesen; Buildnummer aus outputs ermittelt; Build 111 gegen 113 fuer die Nachtregen-Aenderung verglichen; Shaderaufrufe, Constant-Buffer-Layout, F11-Abhaengigkeit, Konflikt-/Escape-Artefakte und git diff --check statisch kontrolliert. Kein vollstaendiger lokaler C++-/Shader-Build.

## Build 112

- Seitenverhaeltnis: Logische Spielaufloesung und physische Borderless-Swapchain sind getrennt. Welt, HUD, Gothic-Menue und F11 werden gemeinsam in der gewaehlten Aufloesung gerendert und erst zur Ausgabe proportional mit schwarzen Balken eingepasst.
- Nachtregen: Die zusaetzlichen flachen Regen- und Tiefennebelschichten wurden entfernt. Der 360-Grad-Entfernungsnebel folgt wieder dem bewaehrten Verlauf, ist nachts dichter und laesst die Wolkendecke nur noch leicht durchscheinen.
- Regenuebergang: In Gothic 2 haelt der echte Wettertyp den Atmosphaerenzustand waehrend aktiven Regens stabil. Nach bestaetigtem Wetterende bleibt der Ausklang strikt monoton und ignoriert verspaetete RainFX-Pulse.
- NPC-Schatten: PCSS bleibt fuer die Welt aktiv; markierte animierte NPC-/Skeletal-Empfaenger verwenden automatisch den stabilen PCF-Abtastpfad.
- Objektsichtweite und VisualFX: Die F11-Skala verwendet zehn gleichmaessige 2500er-Schritte von 2500 bis 25000. Niedrig nutzt Stufe 2, Mittel und Default Stufe 4, Hoch Stufe 6 und Extrem Stufe 8. VisualFX folgt bis Stufe 4 der Objektsichtweite und bleibt darueber bei 10000.
- Pruefung: Statische Diff-, Aufrufer-, Shaderstruktur-, Persistenz- und Seitenverhaeltnispruefung; kein vollstaendiger lokaler C++-Build.

## Build 111 (Korrekturpush)

- Startstabilitaet: Die absturzverdaechtige MENU.DAT-Spracherkennung ueber Gothics VDFS wurde entfernt; die Renderer-Sprache wird nun im F11-Menue manuell zwischen Englisch und Deutsch gewaehlt und global persistiert.
- F11-Speicherung: Die ungewoehnliche STRG-Weltspeicherung samt Welt-Schreibpfad wurde entfernt; sichtbare Renderer-Einstellungen werden ausschliesslich global gespeichert.
- Mod-Weltformat: Weltdateien duerfen nur noch Fog, Atmosphere und Rain setzen; der alte Atmoshpere-Schreibfehler und die entsprechende interne Benennung wurden ohne Legacy-Fallback korrigiert.
- Dokumentation: README auf Installation, aktuelle Buildvoraussetzungen, Mod-Weltformat, Abhaengigkeiten, Lizenz und die gleichwertige Autorenliste reduziert.
- Pruefung: statische Diff-, Aufrufer-, INI-, Symbol- und Projektkonsistenzpruefung; kein vollstaendiger lokaler C++-Build.
- CI-Korrektur: Die lokalisierten std::array-Tabellen im F11-Menue verwenden nun die von MSVC benoetigte doppelte Aggregatklammerung.
- Aufloesungskorrektur: F11-Aufloesungen werden wieder wirklich uebernommen; Menu und HUD fuellen Borderless vollstaendig aus, waehrend nur die 3D-Projektion ein abweichendes Fensterseitenverhaeltnis ausgleicht. Der Sprachwert bleibt am Ende der Settings-Struktur, damit bestehende Feld-Offsets erhalten bleiben.
- Schattenkorrektur: EVSM4-Momente bei erhaltener neuer Cascade-Logik wiederhergestellt; CSM-Empfaengerbias fuer alle Flaechen stabilisiert und Pointlight-Shadowmaps mit Reichweitenhysterese sowie festem FSR-3-tauglichem Filter beruhigt.
- F11: Ueberlange deutsche Beschriftungen wurden gekuerzt und nur die Sprachauswahl verbreitert. Der Blue-Noise-Fog bleibt unveraendert.
- Ground Fog und VisualFX: Die originale Ground-Fog-Partikeldeckkraft bleibt erhalten; fuer alle Partikeleffekte gilt ohne Boundingbox-Sonderfall ausschliesslich VisualFXDrawRadius als Distanzgrenze.
- Pointlight-Schatten: Die Shadowmap bleibt bis zur VisualFX-gesteuerten Lichtausblendung zugeteilt; statt der harten 9x-/10x-LightRange-Grenze reduziert ein stabiler Distanzfade in Legacy und Tiled die Schattenstaerke im letzten Viertel der realen Sichtweite.
- Schattenfilter: EVSM wurde vollstaendig aus Auswahl, Persistenz, Shaderregistrierung, Ressourcenverwaltung und HLSL entfernt; alte EVSM-Konfigurationswerte fallen auf Simple PCF zurueck.
- Schattenstreifen: Die seit Build 109 ergaenzten, wirkungslosen globalen Geometrienormalen- und Mindestbias-Patches wurden gezielt auf das Build-109-Verhalten zurueckgenommen.
- F11-Sprache: Beschriftung und Auswahlfeld verwenden exakt dieselben Breiten wie Grafikprofil.
- Regennebel: Zusaetzlich zur Geometrieueberblendung wirkt bei Regen wieder ein globaler Nahschleier, nachts staerker als tagsueber und fuer Himmelspixel reduziert, damit die Wolkendecke sichtbar bleibt.
- Interaktive Vegetation: Horizontaler Vollbereich auf 0-20 und der weiche Uebergang auf 20-25 Welt-Einheiten begrenzt; die maximale Verschiebung bleibt bei 38.
- Nachtnebel: Der horizontale Entfernungsnebelschleier ist nachts 20 Prozent dunkler; vertikaler Ground Fog und Fernwelt-Dunkelung bleiben unveraendert.

- Regen bei Nacht: Der globale Regenschleier wird um eine entfernungsabhaengige Geometrieschicht ergaenzt; Himmel und Wolkendecke bleiben schwacher betroffen. Kompositions- und Height-Fog-Fallback verwenden dieselbe Logik.
- Regennebel-Uebergang: Die Geometrieverdichtung erreicht ihre maximale Deckkraft erst nach mindestens 3500 Welteinheiten beziehungsweise bei 90 Prozent der Fog-Reichweite; eine quintische S-Kurve verhindert einen sichtbaren Beginn oder harten Fernuebergang.
- Seitenverhaeltnis: Bei gestrecktem Borderless-Betrieb wird nur das fertige Weltbild vor HUD und Menue mit schwarzen Balken proportional eingepasst; 800x600 auf 1920x1080 ergibt 600x600 plus je 100 logische Pixel Seitenrand und damit physisch korrektes 4:3.
- Pointlight-Schatten: Festes 12-Tap-PCF sowie renderaufloesungsabhaengige Mindestweichheit und ein breiterer Distanzuebergang beruhigen FSR-3-/TAA-Flackern; Shadowmaps und Licht bleiben weiterhin bis zur VisualFX-Reichweite erhalten.
- Schattenfilter-Standard: PCSS ist fuer neue beziehungsweise auf Standardwerte zurueckgesetzte Konfigurationen vorausgewaehlt; vorhandene explizite Benutzerauswahlen bleiben erhalten, Feature Level 10 faellt weiterhin auf Simple PCF zurueck.
- World-Shadow-Bewegung: Die geglaettete Sonnen-/Mondrichtung wird nicht mehr auf harte 1/500-Schritte quantisiert und die CSM-Kamera nicht mehr in 64-/160-Einheiten versetzt. Die vorhandene globale Shadow-Texel-Ausrichtung stabilisiert weiterhin gegen Subpixel-Flimmern; Cascade-Renderintervalle und GPU-Last bleiben unveraendert.
- Occlusion Culling: Fuer neue Konfigurationen standardmaessig aktiv; die bewussten Profilwerte bleiben erhalten, also aktiv bei Niedrig/Mittel und inaktiv bei Hoch/Extrem. Individuell gespeicherte Benutzerwerte bleiben beim normalen Laden ebenfalls erhalten.
- Rauch und Fog: Erkannte Smoke-, Rauch-, Steam-, Dampf-, Fog-, Nebel-, Dunst- und Ground-Fog-Partikel werden gamma-korrekt auf exakt 75 Prozent ihrer bisherigen finalen Deckkraft reduziert; Feuer- und Wasserpartikel bleiben unveraendert.
- FSR-3-Himmel: Nur im niedrigen FSR-3-Skalierungsbereich unter 67 Prozent wird Fog-Blue-Noise nicht mehr vor der Rekonstruktion eingemischt. Dort erfolgt stattdessen ein schwaches, dunkelheitsgewichtetes Blue-Noise-Dithering im Ausgabeformat gegen diagonale Rekonstruktionsstreifen; hoehere Qualitaetsstufen bleiben unveraendert.

- Regenferne-Korrektur: Nur weit entfernte Geometrie wird bei Nachtregen entlang des bestehenden weichen Distanzverlaufs bis auf 65 Prozent der bisherigen Nebelfarbe abgedunkelt; Himmel und Wolkendecke bleiben unveraendert.
- Dithering-Korrektur: Das doppelte Fog-/Ausgabedithering und der auffaellige einzelne DDS-Farbkanal sind entfernt. Ein einziges pixelstabiles, kachelfreies Hash-Dither wirkt nach Gamma in allen Darstellungsmodi mit maximal einer halben 8-Bit-Stufe.
- Seitenverhaeltnis-Korrektur: Nicht mehr nur die Welt, sondern der vollstaendige Frame samt Gothic-HUD, Hauptmenue und F11 wird in Borderless proportional eingepasst. 4:3 erzeugt auf 16:9 seitliche Balken, Ultrawide auf 16:9 obere und untere Balken; Fenstermodus und native Seitenverhaeltnisse bleiben ohne Balken.
- World-Shadow-Stabilisierung: Die kontinuierliche Lichtrichtung wird pro Kaskade erst nach etwa einer projizierten Texelbreite zusammen mit Matrix und Shadowmap uebernommen. Bestehende Kaskadenintervalle bleiben erhalten, das Light-Space-Raster rundet symmetrisch und es entstehen keine zusaetzlichen regulaeren Shadowmap-Renderings.
- Rauch und Fog: Die vorherige 75-Prozent-Korrektur wird ersetzt; erkannte Rauch-, Steam-, Dampf-, Fog-, Nebel-, Dunst- und Ground-Fog-Partikel enden nun gamma-korrekt bei exakt 50 Prozent ihrer bisherigen finalen Deckkraft. Feuer- und Wasserpartikel bleiben unveraendert.

## Build 110 (Korrekturpush)

- CI-Fix: Deutsche UTF-8-UI-Texte bleiben als `u8`-Literals erhalten, werden aber fuer ImGui/Gothic gezielt als `const char*` uebergeben, damit Release_G1_12f unter C++20 nicht an `char8_t` scheitert.
- Inhaltlich keine zusaetzlichen Renderer-Aenderungen gegenueber Build 110.
- Pruefung: CI-Fehlerstellen in `GothicAPI.cpp` und `ImGuiShim.cpp` statisch gegen die gemeldeten C2664/C2446-Fehler kontrolliert; kein vollstaendiger lokaler C++-Build.

## Build 110

- Pointlights und ihre Shadowmaps folgen wieder der VisualFX-Sichtweite mit festem Nightly-Maximum, ohne eigenen F11-Regler.
- Schatten- und Vegetationspfade wurden weiter gegen die verbliebenen Streifen- und Flimmerartefakte nachgezogen; EVSM, Contact Shadows und die zugehoerigen Shaderpfade wurden konsistenter angebunden.
- Anzeige- und F11-UI-Verhalten wurden vereinheitlicht, inklusive Display-Mode- und Tooltip-Polish sowie lokalisierter Startmeldung.
- Partikel- und Fog-Faelle sowie Distanz-/Dither-Verhalten wurden weiter geglaettet, ohne die vorhandenen Effekte pauschal abzuschalten.
- Pruefung: statische Diff-, Aufrufer-, Binding- und Shaderpfadkontrolle; kein vollstaendiger lokaler C++-Build.

## Build 109

- Vegetation: die gestreiften Schattenartefakte auf vertikaler Alpha-Vegetation werden nur auf rueckwaertigen Vegetationsflaechen abgefangen; allgemeine Schatten auf anderen Oberflaechen bleiben erhalten.
- Contact Shadows: die feste interne Staerke bleibt bei `0.35` fuer FSR3 und TAA, sonst bei `0.50`; der F11-Regler fuer Contact Shadows bleibt entfernt und die Option ist nur noch ein einfacher Enabler.
- F11: Tooltiptexte wurden kurz und allgemein formuliert; Displaymodus und Versionsanzeige bleiben auf den vereinbarten sichtbaren Positionen.
- EVSM: echter EVSM4-Filterpfad mit mehrstufigen Kaskaden und qualitativer Aufloesungsstaffelung je nach Shadow-Quality weiter integriert.
- Pruefung: nur statische Diff-/Binding-/Aufrufer- und Shaderpfadkontrollen; kein lokaler Vollbau.

## Build 108 (Vegetation-Schatten, Contact Shadows und Versionsanzeige)

- Vegetation: die Schattenbehandlung ist auf Vegetations-Receiver eingegrenzt; die stoerenden gestreiften Artefakte auf vertikaler Alpha-Vegetation werden nur dort abgefangen, nicht auf allgemeinen Oberflaechen.
- Contact Shadows: FSR3 nutzt fest intern `0.35`, ohne FSR3 fest `0.50`; der F11-Staerkeregler ist entfernt, der Schalter bleibt als einfacher Enable-Eintrag.
- F11/Version: die Versionsanzeige steht oben rechts auf Preset-Hoehe, und die oeffentliche Version ist fest auf `Version 18.0` ohne Git-/Datumszusatz gesetzt.
- Build-Pipeline: die Nightly-Build-Konfiguration gibt `18.0` als feste Versionsnummer weiter, damit der veroeffentlichte Stand stabil bleibt.
- Pruefung: nur statische Diff-/Binding-/Aufrufer- und Shaderpfad-Kontrollen; kein lokaler Vollbau.

## Build 106 (MSM, Backlit Vegetation, Contact Shadows und Oillamp-Schatten)

- MSM: der Filter bleibt auch bei Shadow Softness ganz links im MSM-Pfad aktiv und nutzt dort einen sehr kleinen Momenten-Footprint statt eines Hard-/Simple-Fallbacks; hoehere Softness-Werte skalieren den Momentenfilter staerker.
- Backlit Vegetation: Rueckseiten-Transmission ignoriert normale Schattenstreifen nur auf der Rueckseite, Vorderseiten-Schatten bleiben normal. Die allgemeine Vegetationsmaske wurde breiter gefasst, damit Baumlaub wieder sichtbarer auf dem Niveau der aelteren Backlit-Wirkung bleibt, aber mit reduzierter interner Staerke 0,5.
- Contact Shadows und FSR3: Contact-History ist von der Farb-History getrennt und bleibt auch bei FSR3 Native AA stabiler, nicht nur bei Render Scale unter 100 Prozent.
- Indoor-Pointlights: NW_CITY_OILLAMP_01.3DS-Oillamps duerfen analog zu Flammen je das naechste statische und dynamische Licht bis 150 Einheiten als schattenberechtigt verankern; der Schattenursprung wird auf den Oillamp-Mittelpunkt plus 50 Hoeheneinheiten gezogen.
- Offene Ingame-Grenzen: Oillamp-Lichtzuordnung, MSM-Bildruhe bei niedriger Softness, Backlit-Wirkung auf normale Baeume und FSR3-Contact-Stabilitaet sollten im Spiel final gegengeprueft werden.
- Pruefung: Buildnummer aus outputs ermittelt; AGENTS-Regeln gelesen; gezielte Status-, Diff-, Shader-/Binding-, Oillamp-Anker- und git diff --check-Pruefungen statisch kontrolliert. Kein vollstaendiger lokaler C++-/Shader-Build.

## Build 105 (MSM-Hard-Softness, Backlit-Gate und Wind-Motion)

- MSM: bei Shadow Softness ganz links wird kein Momentfilter und keine Moment-Erzeugung mehr genutzt; der Pfad faellt auf direkten Shadow-Compare zurueck, damit harte Schatten Simple-naeher und guenstiger bleiben.
- Backlit Vegetation: Sonnen-/Mondlicht, Forward+, Deferred-, Tiled- und dynamisch schattierte Pointlights koppeln die Rueckseiten-/Ausnahme-Durchleuchtung vollstaendig an den F11-Schalter Backlit Vegetation.
- FSR3/Wind: instanzierte Vegetation schreibt Motion Vectors mit der vorherigen Windphase, damit starker Regenwind weniger History-Schlieren erzeugt.
- 12-Uhr-Weltschatten: Weltgeometrie im Sonnen-Shadowpass wird nicht mehr zusaetzlich gegen das Kaskaden-Frustum verworfen; VOB-/NPC-Culling bleibt unveraendert.
- Pruefung: AGENTS-Regeln gelesen; gezielte MSM-, Backlit-, Wind-CBuffer-, Weltmesh-Shadow-Culling- und Aufruferpruefungen sowie git diff --check statisch kontrolliert. Kein vollstaendiger lokaler C++-/Shader-Build.

## Build 104 (MSM-, Noon-, Backlit- und Preset-Korrekturen)

- MSM: Moment-Sampling auf direkte 4-Moment-Auswertung ohne grobe Moment-Mips/Blocker-Mip-Suche zurueckgefuehrt, damit der Filter weniger pixelig und leichter wird.
- 12-Uhr-Weltschatten: wirkungslosen World-Mesh-Culling-Sonderfall entfernt und die Shadow-Kamera/Kaskadenprojektion am Zenith stabilisiert, ohne sichtbare Sonnen-/Mondbeleuchtung umzubiegen.
- Backlit Vegetation: gemeinsamer Transmissionspfad fuer Sonne/Mond, Forward+/Deferred-/Tiled-Pointlights und dynamisch schattierte Pointlights; benannte Ausnahmen nutzen dieselbe konturbetonte Durchleuchtung statt einfacher Rueckseitenaufhellung.
- F11/Presets: Standard bleibt SSGI aus und Contact Shadows an. Presets setzen Low beide aus, Medium/High Contact Shadows an und SSGI aus, Extreme beide an.
- Pruefung: AGENTS-Regeln gelesen; Defaultwerte, Presetzuordnung, Shader-/Aufruferpfade, betroffene Diffs und git diff --check statisch kontrolliert. Kein vollstaendiger lokaler C++-/Shader-Build.

## Build 103 (MSM-, Noon-, Partikel- und Ressourcen-Korrekturen)

- MSM: Moment-Mip-Footprints an den tatsaechlichen Filterdurchmesser gekoppelt, damit das sichtbare Shadowmap-Texelraster bei weichen Schatten reduziert wird.
- 12-Uhr-Weltschatten: Kaskaden wieder aus den echten Kamera-Frustum-Slices mit stabilem Texel-Snapping und dynamischen Tiefengrenzen aufgebaut; Weltgeometrie wird am exakten Zenith zweiseitig in die Shadowmap geschrieben.
- Partikel und Backlit: die normale Partikelbeleuchtung einschliesslich bestehender Tag-/Nachtabdunklung gilt wieder fuer Groundfog; die benannten Vegetationsausnahmen verwenden wieder die funktionierende identische Vorder-/Rueckseitenbeleuchtung.
- Ressourcen: optimierte RainCloud.dds uebernommen, ungenutzte VDB-Atlasdaten aus dem Partikel-Instanzlayout entfernt (60 statt 72 Byte) und den leeren fuenften Sky-SRV-Slot entfernt.
- Offene Ingame-Grenzen: MSM-Bildruhe, 12-Uhr-Weltschatten, Groundfog-Nachtabdunklung und Backlit-Ausnahmen muessen im Spiel bestaetigt werden.
- Pruefung: DDS-Format, Abmessungen, Mip-Anzahl und Quellhash kontrolliert; CPU-/Shader-Partikellayout, Sky-Bindings, betroffene Shaderpfade und git diff --check statisch geprueft. Kein vollstaendiger lokaler C++-/Shader-Build.

## Build 102 (MSM-Pipeline, Shadow-/PFX-Korrekturen und F11-Feinschliff)

- MSM: echte Moment-Shadow-Map-Pipeline mit optimierter 4-Moment-Kodierung, separaten Moment-Ressourcen, per-Cascade-Mips und lazy Cascade Updates nach valider Erstbefuellung integriert. Shadow Quality bleibt unabhaengig vom Filter und erlaubt auch Extreme/8192 mit MSM.
- MSM-Korrektur: Receiver vor dem ersten Moment werden wieder explizit als beleuchtet bewertet, damit Licht und Schatten nicht invertiert wirken und der Singular-Fallback keine hellen Flaechen abdunkelt.
- 12-Uhr-Schatten: den wirkungslosen Projektions-Clamp entfernt und stattdessen die World-Mesh-Shadow-Culling-Entscheidung im kritischen Noon-Fenster entschaerft, passend zur beobachteten Beschraenkung auf Weltgeometrie.
- Groundfog-PFX: Build-099-Texturinterpolation fuer nebelartige Partikel wiederhergestellt, damit grossflaechige Groundfog-/Rauch-PFX ueber Wasser wieder sichtbar werden.
- Contact Shadows: F11-Layout korrigiert, steile/vertikale Flaechen weniger hart ausgeschlossen und die FSR3-History fuer Contact-Alpha enger geklemmt, um Nachziehen und Flackern zu reduzieren.
- F11: Shadow Filter sauber ueber Shadow Softness platziert, Titelbalken entfernt und Settings-Cursor auf den OS-Cursorpfad gelegt, damit er nicht mit niedriger Render-FPS mitruckelt.
- Pruefung: AGENTS-Regeln gelesen; gezielte Shader-, UI-, Workflow-/Buildnummer-, Diff- und Praeprozessorpruefungen sowie git diff --check statisch kontrolliert. Kein vollstaendiger lokaler C++-/Shader-Build.

## Build 102 (Schatten-, PFX-, UI- und Screen-Space-Korrekturen)

- 12-Uhr-Schatten: den wirkungslosen Noon-Culling-Sonderfall entfernt und die Welt-Kaskadenprojektion auf den stabilen Kirides-17.9.7-Pfad mit festem Tiefenbereich und Texel-Snapping zurueckgefuehrt.
- Groundfog-PFX: eindeutige Groundfog-/Fog-Smoke-Effekte behalten ihren originalen Blendmodus und lineare Emitter-Deckkraft; die allgemeine Nachtabdunklung normaler Partikel bleibt unveraendert.
- F11/Maus: aufloesungsunabhaengige virtuelle Menueflaeche eingefuehrt und OS-Mauskoordinaten auf dieselbe Flaeche abgebildet, damit Groesse und Trefferposition nach Aufloesungswechseln stabil bleiben.
- Contact Shadows: FSR3-History tiefen-/normalvalidiert, Contact-Raymarch und Softfilter ressourcenschonender ausgelegt und wirkungslose Traces frueh beendet.
- SSGI: vier rotierende Low-Discrepancy-Strahlen mit temporaler Akkumulation ersetzen acht zufaellige Strahlen; Reichweitengewichtung und Energiebegrenzung reduzieren breite Lichtlecks bei deutlich weniger Raytests.
- Backlit Vegetation: vollstaendig aufgehellte Rueckseiten durch konturbetonte, nach innen abklingende Lichttransmission ersetzt.
- Offene Ingame-Grenzen: 12-Uhr-Weltschatten und Groundfog-PFX muessen im Spiel bestaetigt werden; SSGI und Contact Shadows sind getrennt auf Bildruhe, duenne Occluder und Leistung zu vergleichen.
- Pruefung: Referenzpfade gegen Kirides 17.9.7 abgeglichen; Aufrufer, Shaderregister, Constant-Buffer-Layouts, Optionen, Ressourcenpfade, Escape-Sequenzen und git diff --check statisch kontrolliert. Kein vollstaendiger lokaler C++-Build.

## Build 101 (MSM-Testfilter, Shadow-/PFX-Fixes und Velocity-Fallback)

- Schattenfilter: MSM als testbarer Shadow-Filter-Modus ergaenzt und im F11-Menue als eigener Enabler eingebunden; Standard bleibt der bisherige einfache Schattenfilter.
- Contact Shadows: sichtbare raybasierte Contact-Shadow-Wirkung aus Build 099 wiederhergestellt und mit deterministischen Mehrfachrays stabilisiert, damit harte Unterbrechungen reduziert werden ohne die Funktion praktisch auszublenden.
- Groundfog-PFX: GROUNDFOG- und nebelartige additive PFX werden breiter erkannt, nicht durch die normale Distanz-/Frustum-Logik verworfen und bleiben ohne unpassende Partikelbeleuchtung sichtbar.
- 12-Uhr-Schatten: Shadow-Kamera-Pullback und Cascade-Update nahe Zenith stabilisiert, damit die Sonnen-Schattenmap um Punkt 12 Uhr nicht kurz ausfaellt.
- TAA/Velocity: der Depth-Motion-Vector-Fallback nutzt nun dieselbe Reprojektionskonvention wie Geometry- und Sky-Velocities (`previousUV - currentUV`). Der normale MRT-Velocity-/FSR3-Pfad bleibt unveraendert.
- Pruefung: AGENTS-Regeln gelesen; Buildnummer aus outputs ermittelt; gezielte Shader-/Konventionspruefung, Diffkontrolle, BOM-/Escape-Sequenzpruefung und git diff --check statisch kontrolliert. Kein vollstaendiger lokaler C++-/Shader-Build.

## Build 100 (Rueckbau VDB, Contact Shadows, Groundfog und Schattenstabilitaet)

- VDB-Rueckbau: VDB-Wolken und VDB-Feuer vollstaendig aus Runtime-Code, Shadern, Paketdateien und Lizenzhinweisen entfernt. FIRE.PFX, FIRE_HOT.PFX und FIRE_COMPLETE_A0.TGA laufen wieder ueber die normalen Gothic-Partikel-/Decalpfade.
- Contact Shadows: der lange richtungsbasierte Screen-Space-Trace wurde durch eine kurze, weiche und deterministische Kontaktabdunklung ersetzt, damit die unterbrochenen Muster, Blickwinkel-Spruenge und FSR-3-Flackerstellen reduziert werden.
- Backlit Vegetation: NW_NATURE_WATERGRASS_56P als weitere Prefix-Ausnahme fuer identische Vorder-/Rueckseitenbeleuchtung ergaenzt.
- Schatten um 12 Uhr: die Shadow-Kamera-Up-Richtung wird nahe Zenith frueher stabilisiert, ohne Sonne-/Mondzeiten oder Lichtuebergaenge zu veraendern.
- Groundfog-PFX: GROUNDFOG-Partikel werden nicht mehr durch die zu harte PFX-BBox-Frustum-Pruefung verworfen; Draw-Radius und showVisual bleiben erhalten.
- F11/Starttext: Startanzeige weist zusaetzlich auf F11 fuer Grafikeinstellungen hin; das F11-Fenster zeigt keine Versionsnummer mehr im Titel.
- Pruefung: AGENTS-Regeln gelesen; gezielte VDB-Symbol-/Asset-/Lizenzpruefungen, Shaderregister-/Escape-Sequenzpruefung, relevante Git-Diffs und git diff --check statisch kontrolliert. Kein vollstaendiger lokaler C++-/Shader-Build.

## Build 099 (Clouds, VDB-Feuer, Backlit-Menue und Contact-Shadow-Stabilisierung)

- VDB-Wolken: die Atlaswolken wurden von uebergrossen Flaechen auf mehrere kleinere lokale Schichten umgestellt. Die Randmaske ist weich, die Kerne bleiben dichter, Tageshimmel und Sternenhimmel bleiben zwischen den Wolken sichtbar; Nacht- und Regenfaerbung bleiben erhalten.
- VDB-Feuer: FIRE_HOT.PFX fuer Lagerfeuer wird robuster auch bei Pfad-/Quote-/Namenszusatz erkannt. FIRE.PFX und FIRE_HOT.PFX nutzen globale fraktionale Atlas-Animation mit Frame-Interpolation und vertikaler VDB-Orientierungskorrektur gegen Stocken und falsche Ausrichtung; FIRE_MEDIUM und TORCH bleiben unveraendert.
- Backlit Vegetation: der F11-Eintrag steht direkt ueber Enable Rain. Die zweitseitige Beleuchtung matcht die genannten Vegetationsnamen als Wortstamm/Prefix, etwa NW_NATURE_GRASSGROUP_01, OW_NATURE_BUSH_02*, OW_NATURE_BUSH_03*, NW_NATURE_PLANT_03*, NW_KORN* und OW_GRASS_WINTER*.
- Contact Shadows: das pixelgebundene Contact-Jitter wurde im Contact-Shadow-Pfad entfernt, der ferne Weltgeometrie-Bereich frueher ausgeblendet und die Contact-Alpha-Maske normal-/tiefenbewusst weich gefiltert. SSGI-Sampling und FSR-3-Pfad bleiben unveraendert.
- Pruefung: AGENTS-Regeln gelesen; gezielte Status-/Diff-/Shaderstellenpruefungen, BOM-/Zeilenumbruchkontrolle, literal eingefuegte Escape-Sequenzen und git diff --check statisch kontrolliert. Kein vollstaendiger lokaler C++-/Shader-Build.

## Build 098 (CI-Korrektur)

- CI-Korrektur: NPC-Materialmarker fuer per-draw Node-Attachments im gueltigen Vob-Kontext erzeugt und den unveraenderten VDB-Feuer-Instanzvektor explizit an die const-inkorrekte VertexBuffer-Upload-Schnittstelle angepasst.
- Inhaltlich keine weiteren Renderer-, Shader- oder Ingame-Aenderungen gegenueber Build 097.
- Pruefung: beide gemeldeten Release_G1_AVX2-Compilerstellen, Variablengueltigkeit, UpdateBuffer-Signatur, betroffene Aufrufer und git diff --check statisch kontrolliert; kein vollstaendiger lokaler C++-/Shader-Build.

## Build 098 (Wolken, Feuer, Backlit, Contact Shadows und DoF)

- VDB-Wolken: die Atlaswolken werden als mehrere kleinere, lokal vorbeiziehende und dichter dargestellte Schichten verteilt; blauer Tageshimmel und Sternenhimmel bleiben zwischen den Wolken sichtbar, Nacht- und Regenfaerbung bleiben erhalten.
- VDB-Feuer: FIRE_HOT und FIRE werden zusaetzlich ueber den exakten Vob-Namen erkannt, damit Atlasflamme und Unterdrueckung der alten Partikel beziehungsweise nahen FIRE_COMPLETE_A*-Decals verlaesslich greifen; FIRE_MEDIUM und TORCH bleiben unveraendert.
- Backlit Vegetation: F11 nutzt einen reinen Enabler mit festem internen Wert 0,5; ein alter INI-Staerkewert beeinflusst die Funktion nicht mehr. Identische Vorder-/Rueckseitenbeleuchtung gilt exakt fuer NW_NATURE_GRASSGROUP, OW_NATURE_BUSH_02, OW_NATURE_BUSH_03, NW_NATURE_PLANT_03, NW_KORN und OW_GRASS_WINTER.
- Contact Shadows: der wirkungslose FSR-3-Sonderpfad wurde entfernt. Deterministisches Sampling, dichtere Ray-Schritte und robustere Nahkontakt-Toleranzen reduzieren Blickwinkel-Spruenge und FSR-3-Flackern; NPC-Kontakte bleiben sichtbar, aber gegen harte Gesichtsartefakte begrenzt.
- Depth of Field: der F11-Staerkeregler beeinflusst nur noch die Hintergrund-/Fernunschaerfe; die Vordergrundunschaerfe behaelt ihren festen bisherigen Standardradius.
- Pruefung: betroffene Shaderregister, Constant-Buffer-Layouts, PFX-Erkennung, F11-/INI-Pfade, doppelte Hilfsdefinitionen, literal eingefuegte Escape-Sequenzen und git diff --check statisch kontrolliert. Kein vollstaendiger lokaler C++-/Shader-Build.

## Build 097 (VDB-Feuer und -Wolken, Graslicht und Contact Shadows)

- VDB-Feuer: FIRE_HOT.PFX und FIRE.PFX werden durch kompakte animierte Atlas-Flammen fuer Lagerfeuer beziehungsweise Kamine ersetzt; FIRE_MEDIUM.PFX und TORCH.PFX bleiben unveraendert. FIRE_COMPLETE_A0.TGA wird nur im Nahbereich eines FIRE.PFX-Kaminfeuers unterdrueckt; bei fehlendem Atlas bleibt das originale Partikelfeuer als Fallback erhalten.
- VDB-Wolken: zehn gepackte Volumenwolkenvarianten als zwei animierte Atmosphaerenschichten fuer Tages-, Nacht- und Regenstimmung integriert. Die bestehende Regenwolkendecke bleibt erhalten; es gibt keinen zusaetzlichen Vollbild- oder Schattenpass.
- Backlit Vegetation: Visualnamen mit GRASSGROUP erhalten bei aktivierter Funktion identische Vorder- und Rueckseitenbeleuchtung ohne zusaetzlichen Backlit-Aufschlag. Der F11-Regler steuert weiterhin nur die Backlit-Staerke der uebrigen Vegetation.
- Contact Shadows: NPC-Pixel werden gezielt erkannt und nutzen kuerzere, weichere und schwaechere Contact-Rays gegen harte Gesichts- und Halsartefakte. Weltgeometrie behaelt die bisherige Wirkung.
- FSR 3: nur das Contact-Shadow-Raymuster und dessen temporale Alpha-Historie wurden stabilisiert, um insbesondere fernes Flackern zu reduzieren; Upscaling, Schaerfung, SSGI, Wasser und sonstige FSR-3-Pfade bleiben unveraendert.
- Lizenzen/Paket: erforderlicher JangaFX-CC0-Hinweis kompakt in GD3D11/Licences.txt ergaenzt; ausgeliefert werden nur die beiden komprimierten Laufzeitatlanten.
- Pruefung: Ressourcenpfade und Paketaufnahme, Atlasabmessungen/-inhalt, PFX-Zuordnung und Fallbacks, GBuffer-Marker und alle Beleuchtungsdecoder, Rendergraph-/Shaderregister-/Constant-Buffer-Bindings, Klammer-/Praeprozessorpaare und git diff --check statisch kontrolliert. Kein vollstaendiger lokaler C++-/Shader-Build.

## Build 096 (Stabilitaet, HDR, Beleuchtung und Release-Lizenzen)

- Release-Lizenzen: Attributions- und Lizenztexte in einer einzigen GD3D11/Licences.txt gebuendelt; doppelte MIT-Texte, nicht ausgelieferte Komponenten und der nicht benoetigte Hinweis fuer die einzelne Regentextur entfernt. Die Paketierung entfernt zusaetzlich die separate XeGTAO-Lizenzkopie.
- Sonne/Mond: Abendlicher Wechsel auf die korrekte Zeit 19:15-19:45 verschoben; Sonnenlicht/-schatten blenden 19:15-19:30 aus, Mondlicht/-schatten 19:30-19:45 ein. Godrays und Backlit Vegetation folgen den Lichtgewichten; Wasser-Glints bleiben unabhaengig.
- Regennebel: Tages- und Nachtstaerke auf den Stand vor der letzten Verstaerkung zurueckgefuehrt; der weichere, laengere Distanzuebergang bleibt erhalten.
- Contact Shadows: Nah- und Mittelbereich mit breiteren Rays, zehn Schritten und hoeherer Wirkung deutlich sichtbarer; Fernbereich endet frueher, um Flackern nicht zu verstaerken. FSR 3 selbst bleibt unveraendert.
- Lade-/Exit-Stabilitaet: Welt-, BSP-, VobTree- und MeshManager-Zugriffe abgesichert; leere Geometrie wird ohne polys[0]-Zugriff behandelt. PostFX-/FSR-Ressourcen werden vor dem D3D-Geraet freigegeben und der optionale D3D-Debugzeiger sicher initialisiert.
- Water Effects: sichtbarer Standard und F11-Regler bleiben bei 1,0 beziehungsweise 0,0-2,0; 1,0 entspricht nun der bisherigen Wirkung von 1,4. Wasser- und Regenboden-SSR nutzen dieselbe Normalisierung.
- HDR: Luminanzhistorie startet und resettiert bei Welt-/Spielstandladen neutral auf 18-Prozent-Grau statt maximaler Startbelichtung. Helle und dunkle Adaption bleiben weich getrennt, erreichen das Ziel aber deutlich schneller.
- Pruefung: gezielte Git-Diffs, Projekt-/Shaderregistrierungen, Lade-/Ressourcenlebenszyklen, Feature-Gates, Definitionen, Konfliktmarker und git diff --check statisch kontrolliert. Kein vollstaendiger lokaler C++-/Shader-Build.

## Build 095 (DÃƒÂƒÃ†Â’ÃƒÂ‚Ã‚Â¤mmerung, Screen-Space-Licht und Regennebel)

- Sonne/Mond: Licht, Schatten, Godrays und Backlit Vegetation nutzen getrennte 15-Minuten-Uebergaenge. Mondlicht blendet morgens 04:15-04:30 aus, Sonnenlicht 04:30-04:45 ein; abends blendet Sonnenlicht 17:30-17:45 aus und Mondlicht 17:45-18:00 ein. Schattenquellen ueberlappen nicht.
- Himmel/Wasser: der Mondkoerper bleibt positionsbasiert sichtbar statt ueber das Lichtgewicht ausgeblendet zu werden. Sonnen- und Mond-Glints auf Wasser laufen positionsbasiert unabhaengig von den Licht-/Schatten-Fades weiter.
- Godrays/Backlit: Godrays folgen dem Sonnenlichtgewicht inklusive Regen-Ausblendung; Backlit Vegetation folgt den vorhandenen Sonnen-/Mond-Lichtgewichten inklusive Regen-Ausblendung.
- Contact Shadows/SSGI: SSGI sampelt breiter und verlaesslicher mit weiter gedeckelter Energie, damit mehr Flaechen subtil reagieren ohne die alten Lichtstreifen zurueckzubringen. Contact Shadows werden staerker auf Nahkontakte begrenzt und in der Ferne ausgeblendet, ohne FSR 3 selbst zu veraendern.
- Regennebel: die Geometrie-Nebelrampe bei Regen beginnt weicher ueber eine groessere Distanz, nachts wirkt der Regennebel staerker und tagsueber bei Regen leicht. Himmel-/Wolkenpfad, FSR 3, normale Regen-Transitions und trockener Nachtnebel bleiben unveraendert.
- Pruefung: Shader-Diff, Klammer-/Escape-Pruefungen, Screen-Space-Lighting-Constant-Buffer-Layouts, Zeitfenster-Samples und `git diff --check` statisch kontrolliert. Kein lokaler HLSL-Compiler und kein vollstaendiger lokaler C++-Build ausgefuehrt.

## Build 094 (CI-Korrektur)

- CI-Korrektur: Default-Renderer-Settings in LoadMenuSettings vor die optionale UserSettings-Dateipruefung gezogen, damit der anschliessende Reset versteckter Kirides-/Advanced-Werte in jedem Pfad auf den gueltigen Default-Snapshot ds zugreifen kann.
- Inhaltlich keine weiteren Renderer-, Shader- oder Ingame-Aenderungen gegenueber Build 093.
- Pruefung: gemeldete Release_G1_12f-Compilerstellen, Deklarationsgueltigkeit im gesamten Funktionspfad und git diff --check statisch kontrolliert; kein vollstaendiger lokaler C++-Build.

## Build 094 (Bereinigung und Licht-/Wetter-Korrekturen)

- Release-Bereinigung: 1043 nachweislich unbenutzte Drittanbieter-, Beispiel- und Dokumentationsdateien entfernt; erforderliche Lizenz- und Attributionshinweise in `blobs/licenses` erhalten und in die Release-Paketierung aufgenommen.
- Sonne/Mond: gerichtetes Licht, Schatten und Wasserreflexionen wechseln morgens 04:58-05:02 und abends 18:58-19:02 in getrennten weichen Zwei-Minuten-Fenstern; Sonnen- und Mondschatten ueberlappen nicht.
- Contact Shadows/SSGI: falsche Hauptlichtrichtung der Contact-Rays korrigiert und Treffer robuster gemacht. SSGI nutzt pixel-/framevariierte Strahlen, mindestens zwei Treffer, Energiebegrenzung und normalbewusstes Temporal-Clamping gegen horizontale Lichtstreifen.
- Regen: Nacht-Regentropfen rund 20 Prozent transparenter, gemeinsam fuer nativen und FSR-3-Pfad. Manuelles F11-Wiedereinschalten uebernimmt den aktuellen Regenzustand sofort; natuerliche Wetteruebergaenge behalten ihre bisherige Rampe.
- Regennebel: nur die Deckkraft auf Weltgeometrie bei gleichzeitig Nacht und aktivem Regen leicht angehoben; Wolken-/Himmelpfad, Nachtnebel und sonstige Geometrie-/Himmeluebergaenge bleiben unveraendert.
- F11/HDR: HDR ist fuer neue Standardwerte aktiv. Deaktivierte gekoppelte Effektregler werden auch beim ersten Oeffnen konsistent auf Null dargestellt.
- Pruefung: Projekt- und Shaderregistrierungen, Include-/Ressourcenpfade, Constant-Buffer-Layouts, Paketquellen, literal eingefuegte Escapes und `git diff --check` statisch kontrolliert. Kein lokaler HLSL-Compiler und kein vollstaendiger lokaler C++-Build vorhanden.

## Build 093 (Bodenlicht und Regenwolken-Korrektur)

- Pointlights/Fackeln: gemeinsamer NdotL-Helfer fuer Deferred-, Dynamic-Shadow-, Tiled- und Forward+-Pfade ergaenzt. Nach oben gerichtete Bodenflaechen erhalten bei sehr bodennahen Lichtquellen einen kleinen lokalen Wrap-Anteil, damit eine abgelegte Handfackel den Boden beleuchtet; Reichweite und vorhandene Schattenauswertung bleiben erhalten.
- Regenwolken: zweite Detailwolken-Schicht samt Shaderbinding, Ressourcenpfad und unbenutzter `RainCloudDetail.dds` vollstaendig entfernt. Die einzelne Wolkendecke wird nun als `RainCloud.dds` ausgeliefert.
- Wolkentextur: gegenueberliegende Randbereiche der gelieferten Gothic-Wolke symmetrisch verblendet und erneut als BC1-DDS mit elf Mip-Stufen erzeugt; dadurch schliesst die wiederholte Textur ohne sichtbare harte Kachelgrenzen.
- Regennebel: Standarddichte von `0.00050` auf `0.00078` angehoben und bestehende INIs mit exakt dem alten Standardwert migriert; individuell abweichende Werte bleiben erhalten. Die Sky-Abschwaechung wurde so angepasst, dass der dichtere Nebel auch vor dem Himmel sichtbar ist, die Wolkendecke aber durchscheint.
- Occlusion Culling: Large-VOBs werden im Hauptsichtbarkeitspass nicht mehr occlusion-gecullt, damit grosse Objekte wie Baeume nicht spaet aufpoppen; Small-VOBs und Mobs laufen weiter durch den strengeren Occlusion-Pfad fuer den eigentlichen Performancegewinn.
- Contact Shadows/SSGI: Screen-Space-Licht in einen eigenen Trace- und Temporalpass ausgelagert. Contact Shadows sind wasser-/normalgefiltert und begrenzt; SSGI nutzt stabile hemisphaerische Strahlen, History-Reprojection, Depth-Validation und Neighborhood-Clamping gegen flackernde Striche und punktfoermige Artefakte.
- UserSettings.ini: Welt-/Menueeinstellungen werden fuer den Build auf sichtbare F11-Werte begrenzt; versteckte Kirides-/Advanced-Optionen und alte Gamma/Brightness-Schluessel werden beim Speichern entfernt. Display-Tuning bleibt ausschliesslich ueber `DisplayContrast` und `DisplayBrightness` erhalten.
- Pruefung: alte Base-/Detail-Referenzen und unbenutzte Detaildatei entfernt; DDS-Format, Abmessungen, Mip-Anzahl und Randkontinuitaet kontrolliert; Atmosphaeren-, Pointlight-, Tiled-, Forward+-, Composition- und Screen-Space-Lighting-Shader statisch auf Ressourcen-, Register-, Klammer- und Praeprozessor-Konsistenz geprueft; Occlusion-Flags, Rendergraph-Aufrufer und UserSettings-Lese-/Speicherpfade mit Kirides 17.9.7/Nightly abgeglichen; `git diff --check` sauber. Kein lokaler HLSL-Compiler und kein vollstaendiger lokaler C++-Build vorhanden.

## Build 092 (Regenwolken und VFX-Rollback)

- Regenwolken: Base-Projektion von einem nahezu konstanten Himmelsausschnitt auf eine klar wiederholte Dome-Projektion umgestellt; die transparente Detailbank nutzt eine dichtere, gedrehte Projektion mit schnellerer Eigenbewegung und staerkerer Parallaxe. Regennebel, Wetteruebergang, Horizont und Nachtlogik bleiben unveraendert.
- Presets: Medium nutzt im F11-Menue fuer FSR 3 nun Render Scale High Quality statt Quality.
- Fackel/VFX: den erfolglosen Indoor-/Outdoor-Transition-Fallback vollstaendig auf den stabilen Stand von Build 088 zurueckgestellt. Die kurze bekannte Schattenluecke an der Grenze wird bewusst akzeptiert; der zusaetzliche Frame-Light-Scan sowie die erweiterten Parent-/Remove-Sonderpfade sind entfernt.
- Contact Shadows/SSGI: Wasser- und Wasserfallmaske auch fuer Screen-Space-Lichteffekte erzeugt und in die Composition gereicht; Wasser wird als Empfaenger und Ray-Treffer ausgeschlossen. Contact Shadows nutzen kuerzere stabilere Rays mit begrenzter Abdunklung; SSGI nutzt stabile Richtungen, Mindesttreffer und Highlight-Kompression gegen punktfoermige Artefakte.
- Pruefung: betroffene Fackel-/VFX-Funktionen blockweise exakt mit Build 088 verglichen; Wolken-UV-Abdeckung gegen beide ausgelieferten DDS-Dateien stichprobenartig ausgewertet; Shaderstruktur, Ressourcenpfade und `git diff --check` statisch kontrolliert. Kein vollstaendiger lokaler C++-/Shader-Build.

## Build 091 (Stabilitaets- und Darstellungsfixes)

- VFX-/Fackelstabilitaet: zusaetzlichen rohen Transition-Light-Container entfernt; Inventarentfernung prueft nun das konkrete Vob. Dynamische am NPC getragene sowie echte VisualFX-Lichter bleiben wie die Handfackel ueber die autoritative Light-Map sichtbar und schattenberechtigt.
- Pointlights/NPCs: Indoor-Kennbit fuer Skeletal-GBuffer-Pixel wiederhergestellt, ohne die sichtbare NPC-Farbe oder atmosphaerische Lichtstaerke abzuschwaechen.
- Screen-Space-Licht: bisherigen Nachbarschaftsfilter ersetzt. SSGI verfolgt sechs hemisphaerische View-Space-Strahlen mit Depth-Treffern und sammelt sichtbare Ein-Bounce-Radiance; Contact Shadows verfolgen kurze View-Space-Sichtstrahlen zur aktiven Sonne beziehungsweise zum Mond. Nur High und Extreme aktivieren beide Effekte.
- Regen/Wetter: F11-Regen-Aus deaktiviert Wolkendecke und wetterabhaengige Lichtdaempfung sofort. Fernwelt/Himmel folgen einem einzigen monotonen 60-Sekunden-Wetterwert; nasse Boeden duerfen unabhaengig davon weiter trocknen.
- Regenwolken: bereitgestellte Gothic-Base als langsam bewegte, wiederholte obere Decke integriert; transparente Detailwolke als tiefere, schneller driftende Schicht mit eigener Projektion und staerkerer Parallaxe. Unbenutzte PNG-Duplikate entfernt.
- Sky-Boden: wirkungslosen Schwarzreturn aus Build 089 wieder entfernt; Horizont, Nachtnebel und Weltueberblendung bleiben unveraendert.
- Wasser: fehlerhaften Kamera-Depth-Occlusion-Trace entfernt, damit Vordergrundgeometrie Sonnen- und Mondglanz auf dahinterliegendem Wasser nicht mehr zerschneidet; Regenausblendung bleibt erhalten.
- HDR: Normwert 1,0 bleibt bei intern 7,5; die Stufen oberhalb intern 10 bis zum Maximum 15 verstaerken LPM nun weiter statt am bisherigen Blend-Limit zu saettigen.
- Pruefung: statische Lebenszyklus-, Aufrufer-, Constant-Buffer-, Shaderregister-, Preset-, Ressourcen- und Diff-Pruefungen; kein vollstaendiger lokaler C++-/Shader-Build.

## Build 090 (Korrekturpush)

- CI-Korrektur: VFX-Fallback-Bounding-Sphere speichert die effektive Lichtposition nun explizit per `XMStoreFloat3` in `DirectX::XMFLOAT3`; damit ist die in Release_G1_12f gemeldete ungueltige Zuweisung von `float3` behoben.
- Inhaltlich keine weiteren Renderer-Aenderungen gegenueber Build 089.
- Pruefung: gemeldete Compilerstelle, alle weiteren Aufrufer von `GetEffectivePositionWorld`, Typgrenzen und `git diff --check` statisch kontrolliert; kein vollstaendiger lokaler C++-Build.

## Build 089

- F11/Presets: `Custom` aus der Auswahlliste entfernt, bleibt als erkannter Zustand sichtbar; Presets steuern nur sichtbare F11-Werte, waehrend sichtbare AA-Modi ihre normale CAS-Folge anwenden duerfen.
- F11-Effektregler: einheitliche 0-2-Stufenskalen mit gekoppelten Enablern, Wiederherstellung des letzten aktiven Werts und dezenter inaktiver Skala; HDR neu normiert (`1.0` entspricht dem bisherigen Wert `7.5`, `2.0` entspricht `15`).
- Screen-Space-Licht: Contact Shadows und Screen-Space GI getrennt, mit eigenen Enablern, Reglern und kurzen Tooltips; High und Extreme aktivieren beide Effekte.
- Schatten/Lichter: Extreme-Pointlight-Schatten auf 256 gesetzt; VFX-Lichter bleiben am Indoor-/Outdoor-Uebergang samt Bodenschatten gesammelt, ohne den funktionierenden Handfackelpfad oder atmosphaerische Schattenregeln umzubauen.
- Himmel/Wetter: Unterseite des Taghimmels hart schwarz; neue mehrschichtige Regenwolkendecke, angepasste Backlit-Vegetation und sichtbare Wolken trotz Regennebel; Regenbeginn und monotones Aufklaren dauern jeweils mindestens 60 Sekunden und ignorieren auslaufende Controllerpulse.
- Wasser: terrainverdeckte Sonnenreflexion stabilisiert, passende Mondreflexion ergaenzt und beide bei Regenwolken ausgeblendet; gemeinsamer optisch gleichwertiger Occlusion-Trace reduziert doppelten Shadercode.
- Composition: Heightfog nur outdoor angewendet, auch wenn Contact Shadows oder SSGI indoor aktiv bleiben; Nutzerfassung von `starsh.dds` unveraendert uebernommen.
- Pruefung: Atmosphaeren-, Wasser-, HDR- und Composition-Shader sowie mehrere Composition-Makrovarianten erfolgreich mit DXC kompiliert; statische Aufrufer-, Binding-, Ressourcen-, Escape- und `git diff --check`-Pruefungen; kein vollstaendiger lokaler C++-Build.

## Build 088

- F11/Presets: Presets auf Low/Medium/High/Extreme bereinigt; Shadow Quality steuert Welt- und Pointlight-Schatten gemeinsam, Reset-to-Defaults sowie Pointlight-/World-Shadow-/Shadow-Filtering-Auswahl entfernt.
- Schatten: Pointlight-Schatten laufen fest dynamisch; Simple-PCF bleibt Standard, PCSS-Codepfad entfernt; sichtbare berechtigte dynamische Lichter aktualisieren ihre Schatten pro Frame.
- HDR/LPM: HDR Tone Mapping wieder links bei Contrast/Brightness mit Enabler und Strength-Slider 1-10; LPM-Staerke und Bloom-Anteil folgen dem Slider, Auto-Exposure bleibt enger begrenzt.
- Preset-Erkennung: VSync, FPS-Limit, HDR, Display, Helligkeit und Kontrast sind preset-unabhaengig; zurueckgestellte Preset-Werte werden wieder als passendes Preset erkannt.
- Licht/SSR: unberechtigte Atmolichter gedaempft; Wasser-Sonnen-Glanz stabiler gegen SSR-Geometrie verdeckt, ohne den SSR-Pfad selbst umzubauen.
- Pruefung: statische UI-/INI-/Shader-/Constant-Buffer-/Aufrufer-, Escape- und git diff --check-Pruefungen; kein vollstaendiger lokaler C++-/Shader-Build.

## Build 087

- F11/Presets: Pointlight-Shadow- und Shadow-Filtering-Auswahl entfernt; Pointlight-Schatten laufen intern dauerhaft dynamisch, Presets setzen diese Modi nicht mehr.
- Presets/UI: Low nutzt FSR Balanced, Ambient Occlusion, Backlit Vegetation, Wind und Characters-affect-objects; VSync bleibt standardmaessig aus und preset-unabhaengig; Object-Draw-Distance auf 1-10 skaliert.
- HDR/LPM: Auto-Exposure enger begrenzt, damit LPM weniger stark nach oben beziehungsweise unten regelt.
- Wasser: prozeduraler Sonnen-Glanz wird nur dort gedaempft, wo Wasser-SSR bereits valide Szenengeometrie reflektiert; SSR selbst bleibt unveraendert.
- Pointlight-Schatten: schattenberechtigte sichtbare Lichter im aktiven Radius werden im Dynamic-Pfad pro Frame aktualisiert; nicht berechtigte Atmolichter bleiben ausgeschlossen.
- Pruefung: statische UI-/INI-/Shader-Aufrufer-, Escape- und git diff --check-Pruefungen; kein vollstaendiger lokaler C++-/Shader-Build.

## Build 086

- HDR/LPM: offiziellen FidelityFX-LPM-Pfad mit moderatem Kontrast, Highlight-Shoulder und Farbsaettigung fuer kraeftigere Tiefenwirkung abgestimmt; Belichtung und 18%-Mittelgrau bleiben unveraendert.
- Pointlight-Modi: auf `Static` und `Dynamic` bereinigt; dynamische VisualFX-Lichter nutzen ohne kuenstliche Dreiergrenze die normalen Schatten-/Atlaslimits und bleiben bei fehlendem Schattenslot weiterhin als Licht sichtbar.
- Pointlight-Zuordnung: Weltflammen und Parent-VOB-Lichter global aufgeloest; pro Flamme hoechstens das bevorzugt verwandte beziehungsweise naechste statische und nichtstatische Licht innerhalb 150 Einheiten verankert.
- Mehrflammen-/Konfliktfaelle: TGA dominiert ein einzelnes TGA/PFX-Paar; mehrfach beanspruchte Lichter und Leuchten mit mehreren gleichartigen Flammen behalten ihre gesetzte Position und bleiben schattenfaehig.
- Pruefung: statische Parameter-, Shaderflag-, Aufrufer-, Ressourcen-, UI-/INI- und `git diff --check`-Pruefungen; kein vollstaendiger lokaler C++-/Shader-Build.

## Build 086 (Korrekturpush)

- Korrekturpush: lokalen `ParentClaim`-Standardwert von der umgebenden Variable entkoppelt; MSVC kann den Vector damit regulaer ueber `std::construct_at` default-konstruieren.
- Inhaltlich keine weiteren Renderer-Aenderungen gegenueber Build 086.
- Pruefung: gemeldete Template-Instanziierung, aehnliche lokale Initialisierer und `git diff --check` statisch kontrolliert; kein vollstaendiger lokaler C++-Build.

## Build 085

- Korrekturpush: offizielle FidelityFX-CPU-Helfer fuer das LPM-Setup kompatibel zugeordnet und den vollstaendigen zCVobLight-Typ eingebunden.
- Inhaltlich keine weiteren Renderer-Aenderungen gegenueber Build 084.
- Pruefung: gemeldete Compilerstellen, Praeprozessor-Zuordnungen, Includes und Diff statisch kontrolliert; kein vollstaendiger lokaler C++-/Shader-Build.

## Build 085 (Inhaltspush)

- HDR/LPM: Shaderaufruf auf den offiziellen FidelityFX-LPM-Filterpfad korrigiert; LPM kompiliert wieder mit passender 6-Parameter-Signatur.
- Pointlight-Flammen: PFX-/TGA-Flammen werden anhand von Visual-/Textur-Namen robuster erkannt; nahe doppelte PFX/TGA-Flammen werden als ein Flammenpunkt behandelt, TGA-Position bevorzugt.
- Pointlight-Schatten: kleine berechtigte Lichter bleiben ohne 300er-Sperre schattenfaehig; Flammen-/Parent-Zuordnung bleibt auf dauerhafte Weltlichter begrenzt.
- VFX-Schatten: VisualFX-erzeugte zCVobLight-Lichter koennen im Modus `Dynamic + VFX` dynamische Schatten werfen; bewegliche VFX-Lichter sind auf zwei aktive Schattenlichter mit 0,5s Fade begrenzt, stabile VFX-Lichter nutzen den normalen Dynamic-Pfad.
- F11/Presets: Pointlight-Modus `Dynamic + VFX` im Preset Very High, World-Draw-Distance-Skala auf 1-10 mit Low/Mid/High/Very High 3/4/5/6.
- Pruefung: statische Diff-, Aufrufer-, Projektdatei-, Shader-Signatur-, Escape- und `git diff --check`-Pruefungen; kein vollstaendiger lokaler C++-/Shader-Build.

## Build 085 (Korrekturpush)

- Korrekturpush: `SectionDrawRadius`-INI-Laden explizit als `int` geklemmt und danach auf den Zieltyp gecastet; Release_G1_AVX kompiliert damit ueber die gemeldete `std::clamp`-Mehrdeutigkeit hinaus.
- Inhaltlich keine weiteren Renderer-Aenderungen gegenueber Build 085 (Inhaltspush).
- Pruefung: CI-Fehlerstelle und Diff statisch kontrolliert; kein vollstaendiger lokaler C++-Build.

## Build 084

- Korrekturpush: Pointlight-Quellenklassifizierung fuer dynamisch erfasste Lichter ueber die GothicAPI-Instanz aufrufbar gemacht; Release_G1 und Release_G1_12f kompilieren damit ueber die gemeldete Fehlerstelle hinaus.
- Inhaltlich keine weiteren Renderer-Aenderungen gegenueber Build 083.
- Pruefung: beide CI-Fehlerstellen, Sichtbarkeit, Aufrufer und Diff statisch kontrolliert; kein vollstaendiger lokaler C++-/Shader-Build.

## Build 084 (Inhaltspush)

- HDR: bisherige Eigenbau-LPM-Kurve durch den offiziellen AMD-FidelityFX-LPM-Setup-/Filterpfad ersetzt; lineares 0,18-Mittelgrau und korrekte LPM-Ausgabe, Legacy bleibt vorerst zum Ingame-Vergleich erhalten.
- Pointlight-Schatten: genau eine zugeordnete Flamme bestimmt Licht- und Schattenposition; bei keiner oder mehreren Flammen bleibt die originale Light-VOB-Position.
- Pointlight-Hierarchie: Parent-Lichter werden bis zum ersten Nicht-Licht-Parent uebersprungen; NPC-/VFX-Hierarchien bleiben ausgeschlossen.
- Pointlight-Reichweite: starre 300er-Sperre entfernt; kleine berechtigte Lichter legen Schattenressourcen erst bei Sichtbarkeit an.
- Pruefung: statische Diff-, Aufrufer-, Positions-, Constant-Buffer-, Shader- und Paketpfadpruefungen; kein vollstaendiger lokaler C++-/Shader-Build.

## Build 083

- Frame Generation: den unbrauchbaren DX11-FG-/Optical-Flow-Pfad samt exklusiven Quellen entfernt; FSR 3.1.2 bleibt unveraendert als Upscaler erhalten.
- HDR: Legacy und LPM neu abgestimmt, ungenutzte Tone-Mapping-Modi entfernt und die kompakte Auswahl neben HDR im Display-Bereich angeordnet.
- F11-Menue: nach Oeffnen und Aufloesungswechsel verlaesslich zentriert, danach weiterhin per Titelleiste verschiebbar.
- Pointlight-Schatten: Dynamic/Full-Verhalten des urspruenglichen Kirides-Nightlys wiederhergestellt; Schatten nur fuer dauerhafte VOB-Lichter oder vorhandene parentlose Lichter nahe erkannter Flammen-PFX/-TGA.
- Kaskadenschatten: jitterstabile Tiefenrekonstruktion fuer TAA/FSR sowie lit Border-Sampling und Z-Grenzpruefung gegen Flimmern und dunkle Kaskadenraender.
- Pruefung: statische Diff-, Aufrufer-, Constant-Buffer-, Shaderbinding-, Ressourcen- und Projektdateipruefungen; kein vollstaendiger lokaler C++-/Shader-Build.

## Build 082
- FOV: normale F11-FOV-Funktion und Runtime-Override entfernt; alte INI-FOV-Werte werden geloescht beziehungsweise ignoriert, Gothic bleibt bei der nativen Kamera/Projektion.
- F11-Menue: Fenster zentriert beim Oeffnen und nach Aufloesungswechsel, bleibt waehrend der Sitzung aber wieder per Titelleiste verschiebbar.
- DoF: Schaerfe-/Unschaerfeuebergaenge nutzen kameraradiale Tiefe statt nur View-Z, damit der Fokus beim Drehen stabiler bleibt.
- HDR: kurze Tone-Mapping-Auswahl `Legacy`/`LPM` im F11-Menue ergaenzt; alte interne HDRToneMap-Werte werden auf Legacy normalisiert.
- Pruefung: statische Diff-, Escape-, Shaderpfad- und UI-Pruefungen; kein vollstaendiger lokaler C++-/Shader-Build.

## Build 081
- FOV/Weitwinkel: 100 bleibt exakt Gothics Originalprojektion; hoehere Werte vergroessern horizontalen und vertikalen Projektionswinkel mit identischem Tangensfaktor, sodass keine achsenabhaengige Stauchung oder Dehnung entsteht.
- F11-Menue: Einstellungsfenster bleibt konsequent mittig; FOV-Hilfetext und Endbezeichnung wurden kurz und wirkungsbezogen formuliert.
- FSR3/Dialoggesichter: Kopfaufsatz verwendet vorherige Kopfknochenmatrix; Morph-Meshes liefern vorherige lokale Vertexpositionen fuer echte Mimik-Bewegungsvektoren. Der bisherige Reactive-Wert bleibt nur ergaenzend.
- Pruefung: statische Aufrufer-, Mehrpass-, Shaderbinding-, Projektionsverhaeltnis- und Diff-Pruefungen; FOV und Dialoggesichter muessen ingame validiert werden, kein vollstaendiger lokaler C++-/Shader-Build.

## Build 080
- FOV/Breitbild: 100 bleibt Gothics unveraenderte Originalprojektion; andere Werte skalieren horizontalen und vertikalen Sichtwinkel kontinuierlich, waehrend horizontal zusaetzlich die ausgabeaufloesungsabhaengige Hor+-Korrektur einfliesst.
- F11-Menue: nach einem Aufloesungswechsel wird das skalierte Fenster automatisch wieder mittig positioniert.
- FSR3/Dialoge: vorhandener Dialog-Reactive-Schalter wird mit Reactive 0.30 ohne T&C im Diffuse-Pfad ausgewertet, um Gesichts-Schlieren zu reduzieren.
- FSR3/DoF: Depth of Field wieder an seine fruehere Position vor dem Upscaling und in interner Renderaufloesung zurueckgesetzt.
- Offen: FOV auf extremen Seitenverhaeltnissen, F11-Zentrierung, Dialoggesichter, DoF-Flimmern und transparente Regenausschluesse muessen ingame geprueft werden; kein vollstaendiger lokaler C++-/Shader-Build.

## Build 079
- FSR3/Sky: echte rotationsbasierte Motion-Vektoren nur fuer Sky-Depth-Pixel ergaenzt, um Bewegungsschlieren ohne erneute Reactive-/T&C-Maskierung zu reduzieren.
- FSR3/DoF: Depth of Field wird bei aktivem FSR3 erst nach dem Upscaling in Ausgabeaufloesung angewendet; Nicht-FSR-Pfade bleiben in ihrer bisherigen Reihenfolge.
- Bestehende Kirides-Maskenwerte und die separate FSR3-Regentropfenanpassung bleiben unveraendert.
- Pruefung: statische Shaderregistrierungs-, Projektdatei-, Aufrufer-, Binding- und Rendergraph-Pruefungen; kein vollstaendiger lokaler C++-/Shader-Build.

## Build 078
- Korrekturpush ohne neuen Folge-Build: IceRegion-Helper fuer Weltmesh- und sortierte DrawWorldMesh-Schluessel typneutral gemacht, damit Release_G1_12f wieder kompiliert.
- Inhaltlich keine weiteren Renderer-Aenderungen gegenueber Build 077.
- Pruefung: CI-Fehlerstelle statisch gegen `WorldMeshKey`/`MeshKey` kontrolliert; kein vollstaendiger lokaler C++-Build.

## Build 078 (Folgepush)
- F11-Menue: Fenster, Bedienelemente und Text skalieren bei niedrigen Ausgabeaufloesungen gemeinsam und bleiben insbesondere bei 800x600 bedienbar.
- Regen/Materialien: wirkliche Blend-Transparenz wird ohne Textur-Namensheuristik von Regentropfen, Oberflaechennaesse und Rain-Ground-SSR ausgeschlossen; den wirkungslosen `ICEREGION*`-/Alpha-Test-Sonderweg entfernt.
- FSR3: Welt-, Alpha-Test- und Sky-Maskierung auf das Kirides-Nightly-Verhalten zurueckgesetzt; die separate FSR3-Regentropfenanpassung bleibt erhalten.
- FOV/Breitbild: `100` bleibt exakt Gothics Original; `101-120` blendet kontinuierlich bis zur vollstaendigen seitenverhaeltnisabhaengigen Hor+-Korrektur, `121-130` bietet zusaetzliche Weite. Vertikaler Original-FOV und Kamerahoehe bleiben erhalten.
- Offen: FSR3-Flimmern/Schlieren und die transparenten Regenausschluesse muessen ingame validiert werden; kein vollstaendiger lokaler C++-/Shader-Build.

## Build 077
- Regen/Himmel: Mond-/Sonnenlicht-Schatten werden bei Regen mit ausgeblendet; Regenhimmel bleibt ohne zusaetzliche Nebel-/Nachtsicht-Aenderungen.
- Eisregion: Rain-/Wetness-/Rain-Ground-SSR-Effekte werden gezielt fuer `ICEREGION*`-Materialien blockiert; alter breiter `ICE`-/`EIS`-Heuristikfix ersetzt.
- FSR3: Sky-T&C auf 0.05 reduziert, Alpha-Test-Reactive auf Kirides-artige 0.10 gesetzt, Dialog-Reactive auf 0.30 ohne Dialog-T&C gestellt.
- FOV: Regler wieder als Hor+-Widescreen-FOV aufgebaut; `100` laesst Gothic original unangetastet, hoehere Werte verbreitern das horizontale Sichtfeld.
- Frame Generation: im DX11-Build deaktiviert, weil der manuelle Pfad Optical Flow/Interpolation serialisiert und massive Framedrops verursacht.
- Offen: FSR3-Flimmern, Dialog-Schlieren, Eisregion und FOV muessen ingame final geprueft werden; vollstaendiger lokaler C++-/Shader-Build wurde nicht ausgefuehrt.

## Build 076
- Regen/Himmel: Sonne, Mond und Sterne werden bei Regen sauber ausgeblendet; bestehende Nebel-/Himmelsuebergaenge bleiben unangetastet.
- Regen/Materialien: Regen-, Wetness- und Rain-Ground-SSR-Effekte auf Wasser/Eis/transparenten Flaechen unterdrueckt; Rain-Ground-SSR-Bewegung beruhigt.
- FSR3: Regenstaerke bei aktivem FSR3 reduziert; Alpha-/Dialog-Reactive-Masken fuer weniger Baumflimmern und Gesichts-Krizzeln nachjustiert.
- FOV: 100 = originale Gothic-Kamera; kleinere Werte weiter, groessere Werte enger, horizontal und vertikal aus der nativen Kamera skaliert.
- Frame Generation: F11-Position direkt ueber HDR; DXGI-Frame-Latency fuer manuelles DX11-FG angepasst.
- Offen: Vollstaendiger lokaler C++-Build wurde in dieser Umgebung nicht ausgefuehrt; FSR3-Flimmern und Frame Generation muessen ingame validiert werden.

## Build 075

- Grundlage: Kirides Nightly; 17.9.7 bleibt der letzte Stable-Vergleichsstand davor.
- FSR: Alpha-Test-Flimmern von Vegetation reduziert, indem stabile Tiefen-/Motion-Vector-Flaechen nicht pauschal reaktiv markiert werden.
- Frame Generation: auf Flip-Swapchains begrenzt, unnoetige Vollbildkopie entfernt und Present-/Pacing-Nebenpfade abgesichert.
- Kamera: `100` ist der UI-Wert Original und laesst Gothics native Projektion unveraendert; andere Werte aendern nur den horizontalen FOV.
- NPC-Schatten: Codex-spezifische Aufweichung zurueckgenommen.
- Regenhimmel: Sonnen-Mie-Anteil, Godrays und Sterne werden ausschliesslich bei aktivem Regen ausgeblendet.
- Rain Ground SSR: `ICE`-/`EIS`-Weltmeshes blockieren nasse Bodenreflexionen; horizontale Reflexionsbewegung wurde beruhigt.
- Offene Grenze: Die vorhandene x86-DX11-FSR-Runtime bleibt 3.1.2, da das bereitgestellte offizielle 3.1.4-SDK keine ABI-kompatiblen x86-DX11-Binaries enthaelt.
- Pruefung: statische Diff-, Aufrufer-, Binding- und Projektdateipruefungen; kein vollstaendiger lokaler C++-/Shader-Build.

## Build 185
- Implement volumetric height fog candidate selection for world and rain (Blocks 1-18)


## Build 186
- Editor-Widget-Klassen und ImGuiEditorView rueckstandsfrei entfernt (inklusive BaseWidget, EditorLinePrimitive, GVegetationBox, WidgetContainer, Widget_TransRot).
- GothicAPI und Launcher-Schnittstellen vollstaendig von LoadCustomZENResources und weiteren Editor-Exporten bereinigt.
- Wet Ground SSR: Puddle Geometric World Normal-Berechnung integriert, um Normal-Verzerrungen aus den Puddle-Masken fuer Wasseroberflaechenreflexionen zurueckzunehmen.


## Build 189
- Volumetric Lighting (Godrays) ueberarbeitet: UI auf einen einzigen Ein/Aus-Schalter mit gekoppeltem Staerkeregler vereinfacht.
- Interne Feature-Level-Normalisierung: DX10-Hardware faellt automatisch auf radiale (Low) Lichtstrahlen zurueck, waehrend DX11-Hardware volumetrische (High) Godrays nutzt.
- Presetvergleiche und INI-Speicherung auf den neuen EnableGodRays-Master-Schalter umgestellt.


## Build 191
- Add invalid sky layer marker to low clouds
- Fix validity-aware 2x2 filter for stable sky low clouds
- Enhance water glint transmission based on reflected cloud coverage
- Improve Godrays with radial luminance compression and dynamic litFraction
- Add Temporal Reprojection (TAA) for Volumetric Godrays to increase sample stability

## Build 192
- Implementiert: Contact Shadows sind (bei aktivem FSR 3) auf InnenrÃ¤ume beschrÃ¤nkt. Ein weicher Ãœberblendungseffekt (Transition) sorgt fÃ¼r flieÃŸende ÃœbergÃ¤nge.
- Implementiert: Nahbereichs-Godrays (Near Shaft Scattering) fÃ¼r Lichtstrahlen in direkter KameranÃ¤he.
- Anpassung: Legacy-Wasser-Nachthelligkeit wird nun ausschlieÃŸlich uferabhÃ¤ngig relativ skaliert (kein globales Aufhellen).
- Anpassung: Cubemap-Spiegelungen auf Wasser beachten nun die HemisphÃ¤re (hemi).
- UI: "Volumetric Lighting" in "Light Shafts" umbenannt. Tooltip fÃ¼r Kontaktschatten aktualisiert.

## Build 193
- Regulaerer Push: Radiale GodRays ueberarbeitet: AC_SunVisibility aus der Gewichtung entfernt. Sky-Alpha-Maskierung in CS_PFX_GodRayMask eingefuehrt. Zoom-Pass extrahiert shaftProfile ueber Sampling-Varianz zur Objektkantenerkennung. Combine-Pass nutzt shaftProfile fuer saubere, trennscharfe Ueberblendung zwischen gedämpftem Himmels-Radial und ungedämpften Lichtstrahlen an Kanten, ohne das Lens-Flare zu beeinflussen.

## Build 194
- CS_PFX_GodRayZoom.hlsl: Radial Blur wiederhergestellt, Legacy-Lens-Flare entfernt, volumetrische Phasenfunktion vereinheitlicht, Gewichtung bei Combine angepasst.
- PS_PFX_LowClouds.hlsl: Breiter Sun-Backlight-Pfad (Broad Sun Mask) mit separaten Dichtemasken fuer Body und Thin-Edge implementiert.
- PS_Water.hlsl: Shore-Intervalle vereinheitlicht, Reflexionen fuer Ocean-Geometry gedaempft und stabilisiert.
- PS_PFX_LowCloudComposite.hlsl: Geometrie-Wolken erhalten bei niedriger Konfidenz im 5x5-Fenster einen volumetrischen Fallback (ComputeRefinedLowClouds) mit voller Raymarch-Integration (inkl. PFXBuffer-Uebergabe in D3D11PfxRenderer.cpp).

## Build 199
- Regulaerer Push: BspPortalCuller (Portal Culling System) implementiert und in GothicAPI, WorldObjects, ShadowMap und PointLight verknuepft.

## Build 200
- Korrekturpush: GitHub-Build repariert / korrigiert.
- Korrekturpush: Weitere Fixes (zweiter Upload).
- Korrekturpush: D3D11PfxRenderer Fix.

- Korrekturpush: Low-Cloud-Dimensionen vorerst auf /2 zurueckgesetzt (Fehlersuche fuer /4 dauert an).

- Korrekturpush: Manuelle Code-Zentralisierung fuer cloudRes und Anpassungen auf /4 durch den Benutzer uebernommen.

- Korrekturpush: Viewport in RenderLowCloudLayer liest Dimensionen nun laufzeitsicher direkt aus der RenderTarget-Textur, um Asynchronitaeten bei /4-Aufloesung auszuschliessen.

- Korrekturpush: Viewport in RenderLowCloudLayer greift statt auf dynamisches RenderTarget-Auslesen wieder auf eine lokale cloudRes-Berechnung zurueck (synchronisiert auf /4).

- Korrekturpush: Signatur-Mismatch behoben (cloudRes-Parameter in D3D11PfxRenderer.h analog zur .cpp entfernt).

- Korrekturpush: Erneuter Rollback zur dynamischen Viewport-Dimensionierung via RenderTarget-GetDesc() in RenderLowCloudLayer (Sicherstellung der Sync-Integritaet).

- Korrekturpush: Diverse manuelle Feinabstimmungen (DoF default an, dynamische Wolken default aus, reduzierte RainFog-Opacity, erweiterte Tag/Nacht-Farbtonkorrektur fuer WetGroundSSR und Ozean-Reflexionen).

- Korrekturpush: Kaskaden-Frustum fuer Shadow-Weltmesh wiederhergestellt (behebt massiven Triangle-Overhead durch AlwaysContainingFrustum-Workaround).

- Korrekturpush: Sky-Edge-Blur im DoF-Composite implementiert und statische Silhouette-Confidence entfernt, um Himmelsuebergaenge unscharfer Objekte weicher zu mischen.

- Regulaerer Push: Umstellung der Release-Tags auf explizite Versionierung (v18.0) und Entfernung des Build-Prefixes aus Release-Artefakten.

- Korrekturpush: Access Violation beim Prozessende behoben (Shutdown-Logik aus DllMain entfernt). Inverse-Gamma-Korrektur fuer Groundfogs korrigiert (35% Sichtbarkeit wiederhergestellt). SSR-Fallback-Cubemap fuer Wasser bei Nacht abgedunkelt.

- Korrekturpush: Groundfog-Erkennung erweitert (Swampfog/Dunst in NewWorld wird wieder vom Renderer als echter Nebel mit korrektem Culling und Lighting behandelt). Weichere Uferuebergaenge beim Color-Blending des Legacy-Wassers hinzugefuegt.

- Korrekturpush: Groundfog-Erkennung (NewWorld Sumpfnebel) ueberarbeitet (sichere Erkennung ueber Firesmoke-Textur und BlendMode statt generischer Namenssuche, um False-Positives bei magischen Effekten/Feuer zu vermeiden).

- Korrekturpush: Groundfog-Erkennung weiter verfeinert (Vermeidung von False-Positives bei echten Rauch-Effekten wie humansmoke durch zusaetzliche Namensausnahmen, Emissive-Logik bereinigt).

### Build 202
- Korrekturpush: Nightly-Fix fuer Shader. Die WorldPosition wird nun korrekt als echte Weltkoordinate aus dem Vertex Shader (VS_Ex und VS_ExInstancedObj) exportiert, wodurch die Window-Cutout-Logik und das korrekte Clipping in den Pixel Shadern (z. B. GBuffer und Forward-Rendering) wieder fehlerfrei arbeiten.

### Build 203
- Korrekturpush: CBuffer/CPU-InverseView Architektur für Window-Cutouts. Die WorldPosition-Exporte aus den Vertex Shadern wurden zurückgerollt, stattdessen wird die Inverse View Matrix einmalig pro Frame auf CPU-Seite berechnet und in den CutoutConstants-CBuffer hochgeladen. Dies vermeidet Vertex-Shader-Aufblähungen und erzielt das gleiche Ergebnis robuster im Pixel Shader.

- Korrekturpush: Tile-based Window Cutouts implementiert. Die Cutout-Schleife im Pixel Shader wertet nun eine 2D-Tile-Maske aus, um nur noch die Fensterausschnitte zu berechnen, die den aktuellen Bildschirm-Tile schneiden. Das verbessert die Shader-Performance massiv.
- Korrekturpush: OilLamp Emission Logic überarbeitet. Bei der Zuweisung von Lichtern an Öllampen (ConfigureAllPointlightShadowSources) werden nun statische Lichter im Suchradius bevorzugt. Nur wenn kein statisches Licht gefunden wird, greift das System auf das nächste dynamische Licht zurück.
- Korrekturpush: MeshVisualInfo um WindowGlassBounds erweitert für präziseres Bounding-Box-Tracking von Fensterscheiben.
- Korrekturpush: C++ Typecast-Fix in der Window-Cutout-Logik (Korrektur von BaseVisualInfo auf MeshVisualInfo per dynamic_cast).
- Korrekturpush: C++ XMMATRIX Transpose-Fixes. Gothic speichert Vob-Matrizen standardmäßig für Shader (Column-Major) transponiert ab. Für Berechnungen auf der CPU (DirectXMath) müssen diese zurück-transponiert werden. Das korrigiert die Orientierung der Window-Cutout-Bounds und die Richtung der Öllampen-Licht-Offsets. Zusaetzlich wurde der Y-Offset für Öllampen-Schatten auf 20.0f reduziert.
- Korrekturpush: Ocean Water Drift gefixt (PS_Water.hlsl). Die Weltkoordinaten für die Distortion-Animation werden für den Ozean nun um 90 Grad gedreht, damit die Wellenbewegung in die korrekte Richtung verläuft.
- Korrekturpush: City-Window Validierung & Counterparts (GothicAPI). Gegenüberliegende City_Window-Instanzen (innen/außen) werden nun automatisch verknüpft, um zu prüfen, ob die Transparenz an dieser Stelle gültig ist (verhindert Löcher in Wänden, wo keine Cutouts existieren). In PS_Simple.hlsl wurde ein spezieller FFDATA-Pfad für den transparenten Teil ergänzt.
- Korrekturpush: Feintuning der Öllampen-Schatten und Ocean-Water-Drift. Der Y-Offset für Öllampen-Schatten wurde wieder auf 50.0f und der Forward-Offset auf 25.0f angepasst. Die Ozean-Rotation in PS_Water.hlsl wurde um weitere 180 Grad gedreht (-Z, X), um die exakte Original-Laufrichtung zu treffen.
- Korrekturpush: City-Window Validierung extrem vereinfacht. Die fehleranfälligen Sonden für Fußböden und Decken wurden entfernt. Die Fenster fallen nun standardmäßig auf transparent (fail open) zurück, es sei denn, eine Mehrheit der Abtastpunkte findet definitiv eine parallele Wand hinter dem Cutout.
- Korrekturpush: Öllampen-Emission gelockert. Bei der Suche nach statischen oder dynamischen Lichtern für die Öllampen-Emission wird nicht mehr streng gefiltert, ob das Licht schon von einer normalen Flamme "verbraucht" wurde, was zuvor dazu führte, dass viele Lampen dunkel blieben. Es wurde auch ein Log hinzugefügt, das die Emission-Links aufschlüsselt.
- Korrekturpush: Ocean Water Animation (PS_Water.hlsl) finalisiert. Statt die Koordinaten noch einmal umständlich zu drehen, wird nun für den Ozean einfach die Animationszeit (RI_Time) umgekehrt (-1.0f). Dadurch laufen die Wellen exakt auf die Küste zu, ohne dass sich die Form der Wellen verzieht oder Flüsse beeinträchtigt werden.
- Korrekturpush: Shader-Refactoring und Optimierungen. Die Window-Cutout-Schleife (PS_Diffuse.hlsl) nutzt nun 'firstbitlow' für noch schnellere Iterationen durch die Tile-Maske. Die Öllampen-Emission wurde in einen dynamischen Branch ausgelagert und eine harte Discard-Regel für den Alphatest von City_Windows (auch im Forward-Path) eingeführt. Kleinere Aufräumarbeiten im Water-Shader (Variablennamen).
- Korrekturpush: City-Window Backface-Culling anstatt Counterpart-Pairing (GothicAPI). Die umständliche Logik, gegenüberliegende City_Window-Instanzen zu paaren, wurde komplett verworfen. Stattdessen liest die Engine nun direkt beim Parsen der Mesh-Geometrie die Front-Normale der realen Glasscheibe aus. In CollectVisibleVobs werden nach hinten zeigende Fenster (die untexturierte Rückseite) nun direkt aussortiert und gar nicht erst in die Render-Queue aufgenommen. Das ist extrem elegant und performant!
- Korrekturpush: City-Window Backface-Culling Feintuning und RenderState-Refactoring. Das Backface-Culling für Fenster hat nun eine Toleranz von 5 Grad (BackfaceToleranceSinSq), um ein hartes Aufpoppen an scharfen Kanten zu vermeiden. In DrawFrameAlphaMeshes wurde das State-Management optimiert (CullMode für Glas auf CULL_NONE gesetzt, um falsche Winding-Orders von Innen-Meshes abzufangen, und redundante State-Updates eliminiert). Zusätzlich wurden BSP-Tree-Guards eingefügt, um Abstürze in frühen Ladephasen von Gothic zu verhindern.
- Korrekturpush: City-Window Sky-Safeguard und Particle-Lighting (Nightly). In PS_Simple.hlsl wurde ein intelligenter Sky-Safeguard für City_Windows implementiert: Über eine vertikale Probe (EvaluateWindowSkyPath) wird nun im Screen-Space geprüft, ob der Weg zum Himmel durch Weltgeometrie verdeckt ist; falls ja, blendet das Fenster auf opak über. In den Particle-Shadern wurde eine stärkere nächtliche Verdunkelung für Bodennebel (groundFog) integriert. Außerdem schreibt opake Weltgeometrie nun korrekterweise WorldGeometryMask = 1.0f.
- Korrekturpush: City-Window Textur Update und Sky-Safeguard Verbesserungen. Die vertikalen Probes in PS_Simple.hlsl wurden von 8 auf 24 erhöht, um keine Wandstreifen mehr zu überspringen. Die DDS-Textur City_Window.dds wurde aktualisiert und der Shader erzwingt nun ein Minimum an Alpha (0.18f), damit das Glas immer sichtbar bleibt, selbst wenn die Textur voll transparent ist.
- Korrekturpush: Particle Night-Tint und Transparency-CBuffer. Bodennebel (und andere Partikel) nutzen nun GetAmbientNightWeight() und erhalten über ApplyAmbientNightTint eine echte nächtliche Tönung. In PS_Transparency wurde zudem ein GA_LightingTint Parameter in den Constant Buffer aufgenommen.
- Korrekturpush: Legacy Waterfalls Hue-Correction (PS_Water.hlsl). Steile Wasserfälle erhalten nun dieselbe Scene-Hue-Korrektur wie horizontales Wasser, allerdings erst nach der Reflection/Cubemap-Komposition, um Farbabweichungen in den Rot/Blau-Kanälen zu verhindern.
- Regulärer Push: Abschließende Optimierungen in Build 203. EvaluateWindowSkyPath bricht nun im [loop] frühzeitig ab, wenn das Ergebnis feststeht, und die horizontalen Probes werden nur ausgeführt, wenn der mittlere Pfad blockiert ist. Die Cache-Logik in GothicAPI nutzt std::atomic_bool. Atmospheric Scattering wird nun korrekt NACH der Öllampen-Emission angewendet.

- Korrekturpush: Compute Shader WindowSkyVisibility. Die teure Screen-Space Raycast-Logik fr City_Windows wurde in einen eigenen Compute Shader (CS_WindowSkyVisibility.hlsl) ausgelagert, der das Ergebnis nun pro Frame in einer reduzierten Maske cacht. PS_Simple greift nur noch auf diese Textur zu, was die Fragment-Shader deutlich entlastet. Zudem wurde der Alpha-Bereich fr das Glas sanft abgedunkelt.
- Korrekturpush: Feintuning an WindowSkyVisibility und OilLamps. In D3D11GraphicsEngine wird das SRV nun explizit ungebunden, BEVOR der Compute-Shader aufgerufen wird, um D3D11 Hazard-Warnungen (gleichzeitiges Binden von SRV und UAV) zu beheben. Die SkyGuard-Distanz für Fenster wurde von 2000-3000 auf 1000-1500 reduziert, um den Effekt im Nahbereich schneller greifen zu lassen. Für Öllampen (GothicAPI) wurde die StrongChromaticSaturation von 0.55 auf 0.75 erhöht, damit leicht gefärbte Flammen öfter den weißen Fallback nutzen.
- Regulärer Push: Abschließende Optimierungen in Build 204. Implementierung eines Textur-Swap-Mechanismus für Partikel (Dark/Bright Textures wie FIRESMOKE_DARK zu FIRESMOKE) in GothicAPI, um dynamische Verdunkelung zu unterstützen. Öllampen mischen nun einmalig die Farben aus statischen und dynamischen Lichtquellen (MixOilLampEmissionColors), sodass die Lampen-Emission stabil bleibt und nicht mehr unangenehm mit Licht-Animationen flackert.

- Korrekturpush: Feintuning in Build 205. Die Output-Dithering-Kurve in PS_PFX_GammaCorrectInv wurde angepasst (Dithering bleibt nun über einen weiten Luminanzbereich aktiv), was Banding-Artefakte an Wänden und Laub extrem reduziert. In PS_ParticleDistortion und PS_ParticleSimple wurde die Tag/Nacht-Wichtung für Wasserpartikel optimiert. In PS_Simple wurde der alte Screen-Space Connectivity-Test für das untere Bildschirmdrittel der City_Windows verworfen, da Tiefe nahe Null dort dem Gothic-Void/Boden entspricht und nicht dem Himmel.
- Regulärer Push: Abschließende Optimierungen in Build 205. In PS_Water.hlsl wurde die Shore-Fade-Logik repariert (Wasser verblasst nun nicht mehr fälschlicherweise in die leere Szene, wenn flaches Land dahinter liegt, und schmale Wasserfälle werden nicht mehr weichgezeichnet). In ShadowSampling.h nutzt der Fallback-Pfad ohne Blue-Noise nun ebenfalls Hash-Rotation, um hartes Banding der PCF-Schatten zu verhindern. In WorldObjects.h wurden neue Properties (WaterGeometryClass) für das kommende Water-Refactoring vorbereitet.

- Korrekturpush: Revert und Redesign in Build 206. Einige experimentelle Features des ersten Pushes (CSM-Schatten-Dither, Water-RenderMode-Split, Compute-Fallback in PS_Simple) wurden wieder verworfen. Dafür wurde das Output-Dithering in PS_PFX_GammaCorrectInv deutlich nachgeschärft: Es bleibt nun bis in die hellsten Bereiche (0.995f) aktiv, da Quantisierung auch in hellen, flachen Gradienten stark sichtbar war. Der Daytime Gothic-Fog (PS_PFX_Heightfog) nutzt nun weichere Fade-Grenzen für das Ausblenden der Geometrie und erzwingt im Hintergrund zwingend die Nebelfarbe, um unschöne blaue Silhouetten in der Ferne zu tilgen.
- Korrekturpush: Redesign der City-Window Sky-Protection (PS_Simple). Der Compute-Shader Ansatz (TX_WindowSkyVisibility) wurde verworfen und durch einen genialen, analytischen Texture-Space Raycast ersetzt. Die 5 Fensterreihen der City_Window Textur werden nun über Derivate (ddx, ddy) präzise in den Screen-Space projiziert, wodurch die Sichtbarkeit jeder Reihe unabhängig ermittelt werden kann. In ShadowSampling.h wurde das Penumbra-Dithering wieder entfernt und stattdessen eine dynamische Weichzeichnung bei flachen Einfallswinkeln (grazingFootprint) eingeführt. Zudem wurde in PS_World.hlsl der WorldGeometryMask Export aufgeräumt.
- Korrekturpush: Perspektivisch korrekte Fensterprojektion & Nebel-Tuning. In PS_Simple.hlsl wurde die Screen-Space Projektion der Fenster-UVs massiv verbessert (WindowUvToScreenPosition rechnet nun mit UV/W statt linearen UVs, um Verzerrungen zu vermeiden). In PS_PFX_Heightfog.hlsl blockt Gothic-Nebel Geometrie in der Ferne nun unabhängig von der Tageszeit konsistent aus (worldFogOpacity greift das FarOcclusion-Gewicht auf). In ShadowSampling.h wurde das Grazing-Footprint-Experiment wieder verworfen.

- Regulärer Push: Abschließende Optimierungen in Build 207. Einführung eines dynamischen Ocean-Climates: Je nach Welt (ADDONWORLD vs Khorinis) hat das Ozean-Wasser in PS_Water nun unterschiedliche Streu- und Absorptionsprofile sowie einen spezifischen Luma-neutralen Tint, der bei Regen und Nacht physikalisch korrekt abnimmt. In D3D11ShaderManager wurde die Shadow-Kernel-Qualität (PCF/PCSS Taps & Blue Noise) nun direkt an die TAA/FSR3-Rekonstruktion gekoppelt: Ohne TAA nutzt der Renderer deutlich mehr Taps und verzichtet auf stochastisches Noise, um Stippling auf animierten Charakteren zu vermeiden.

- Korrekturpush: Feinschliff in Build 208. Das Vegetation-Wind-System wurde perfektioniert (VS_ExInstancedObj.hlsl): Der Wind wird nun zwingend im World-Space nach der Instanz-Transformation angewendet, sodass sich Pflanzen bei Rotation im Spacer nicht mehr "in die falsche Richtung biegen". Das System nutzt nun zudem die reale Bodenneigung (WindGroundPlane aus WorldObjects.h), anstatt stur die lokale Bounding-Box für das Biegungsverhalten heranzuziehen. In PS_Water.hlsl fadet der Jharkendar-Ocean-Tint in flachem Wasser sanfter aus, damit die Küstenübergänge weich bleiben. Für die PCSS-Schatten wurden die Tap-Zahlen (ShadowSampling.h) für das No-TAA-Fallback auf robustere Werte kalibriert.
- Korrekturpush: Weiterer Feinschliff in Build 208. In D3D11GraphicsEngine.cpp wurde die Zuweisung für die WindGroundPlane repariert (direkte float3 -> XMVECTOR Konvertierung statt XMLoadFloat3, um eventuelle Alignment-Probleme zu umgehen). In der Deferred Shading Pipeline (Legacy & Tiled) wurde ein genialer Kniff eingebaut: Der Distanz-Tier für die Pointlight-Schatten wird nun direkt in der ShadowSoftness encodiert (über einen 16.0f Marker). So können die Shader erkennen, ob sie weite Schatten mit weniger Taps rendern sollen, ohne dass der ohnehin heiße Constant-Buffer wachsen muss!
- Korrekturpush: Abschließender Wind-Polishing in Build 208. In VS_ExInstancedObj.hlsl (und D3D11GraphicsEngine.cpp) wurde zwischen "Gras" und "Bäumen" unterschieden (abgeleitet aus VisualCamAlign). Gras-Patches nutzen nun eine affine Terrain-Scherung (signedTerrainShear), was die hässlichen inversen Deformationen bei fehlerhafter Platzierung verhindert und den Boden-Übergang stationär hält. Bäume behalten ihr non-lineares Biegungsverhalten. Außerdem wurde die Root-Gewichtung nun endlich auch auf die dynamische Charakter-Interaktion (Hero läuft durch Busch) angewandt, sodass Büsche nicht mehr an der Wurzel vom Boden abreißen!
- Korrekturpush: Performance- & Stabilitäts-Update für das neue Wind-System (Build 208). In VS_ExInstancedObj.hlsl wurde für Bäume (grassShearProfile < 0.5f) wieder ein strikt rigider Stamm (untere 12%) erzwungen, damit Gelände-Ankerpunkte niemals Baumstämme verschieben. Es wurde ein Distanz-LOD für den Wind eingeführt (windDistanceFade & detailLod), das hochfrequente Blatt-Turbulenzen auf Entfernung ausblendet und auf extreme Distanzen den Wind komplett abschaltet. Zur massiven Einsparung von ALU-Zyklen wird der vorherige Wind-Zustand (previousWorldWindOffset) für Motion Vectors nur noch berechnet, wenn ccurateWindVelocity (TAA/FSR) aktiv ist. In C++ wird die WindGroundPlane nun über WindGroundPlaneInitialized effizient gecacht.
- Korrekturpush: Finales PointLight-Shadow & Lighting Update in Build 208. In PointLightShadows.h wurde der Poisson-Kernel von 12 auf 8 Taps reduziert, dafür aber mit einer kamera-unabhängigen Rotation (PLS_StableWorldNoise) versehen. Das bricht das Banding auf, ohne temporäres Flimmern zu verursachen! In ForwardPlusLighting.hlsl wurde das Auslesen der ShadowCube-Arrays anhand der neuen Bitmask-Tiers (StaticLow, Dynamic) implementiert. Zudem wurde der veraltete, rechenintensive ComputeIndoorDoorFloorBleed entfernt – Indoor-Lichter bleiben nun durch einen simplen Maskierungs-Check sauber vom Outdoor-Terrain getrennt, ohne unschöne Contour-Bands an Türen zu erzeugen.
- Korrekturpush: Rollback des Shadow-Noise und Slot-Fix in Build 208. Die Rotation des PointLight-Kernels (PLS_StableWorldNoise) und das Distance-Tier-Encoding wurden vorerst wieder entfernt, da sie offenbar Nebeneffekte hatten. Der Kernel bleibt nun bei statischen 8-Taps, was stabiler sein sollte. Zusätzlich wurde das FP_DynamicShadowCubeArray in ForwardPlusLighting.hlsl von Slot 	13 auf 	21 verschoben, um potenzielle Kollisionen im Ressourcen-Binding (z.B. mit anderen Texturen) zu verhindern.
- Korrekturpush: Software-PCF für PointLight-Schatten in Build 208. Da die Cubemaps ohnehin lineare radiale Tiefenwerte speichern, wurde das Hardware-PCF (SampleCmpLevelZero) durch einen manuellen Software-PCF Ansatz über einen linearen Sampler (SampleLevel) und smoothstep ersetzt. Das verhindert effektiv, dass sehr weite Shadow-Softness-Kernels die binären PCF-Coverage-Level sichtbar freilegen (Banding-Artefakte). Dementsprechend wurden die Sampler in Tiled-Shading, Forward-Plus und DynShadow auf SS_Linear umgebogen.
- Korrekturpush: Hardware-PCF Comeback und Adaptive Shadow-Taps in Build 208. Das Software-PCF-Experiment wurde verworfen und auf Hardware-PCF (SampleCmpLevelZero) zurückgerollt. Stattdessen wurde nun ein adaptives Distance-LOD für die Pointlight-Schatten eingebaut (eceiverCameraDistance): Ab einer Softness > 0.75 interpolieren im Nahbereich (< 5m und < 2m) dynamisch bis zu 8 zusätzliche, dichte Filter-Taps stufenlos hinzu. Dadurch bleibt das Shadow-Sampling in der Ferne bei performanten 8 Taps, während Kanten im Nahbereich butterweich verschmelzen und Banding verstecken. Die Sampler wurden entsprechend wieder auf SS_Comp zurückgesetzt.
- Regulärer Push: Abschließende Optimierungen in Build 208. Das adaptive Shadow-LOD wurde verworfen und stattdessen durch eine massive Hardware-Beschleunigung ersetzt: Für extrem weiche Pointlight-Schatten (shadowSoftness > 0.75) nutzt die Engine nun die GatherCmp-Instruktion. Damit werden mit nur 4 Texture-Fetches gleich 16 Tiefenwerte gesampelt (Hardware-PCF x4 pro Fetch). So erhalten weite Pointlight-Schatten nun butterweiche 16 Taps zum Preis von 4, ohne jegliche Distanz-Zonen oder komplexes LOD-Management!


























