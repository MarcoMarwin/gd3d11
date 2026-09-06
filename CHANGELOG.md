## Build 227

## Build 226 (Release v18.0)
- **Release v18.0:** Offizielles Release auf Basis des stabilisierten Stands von Build 226.
- **Pointlight-Shadow Caching:** Die Initialisierung des statischen Punktlicht-Mesh-Caches (InitResources / WorldMeshCollectPolyRange) wurde aus dem WorkerThreadPool zurück in den Haupt-Render-Thread verlegt. Die vorherige asynchrone D3D11-Buffer-Erstellung kollidierte beim GPU-Virtual-Address-Mapping mit dem Immediate Context des Render-Threads, was zu Framerate-Stalls führte. Der synchrone Aufbau behebt diese Ruckler.

## Build 225
- **Punktlicht-Schatten für dynamische Objekte & Attachments:**
  - GothicAPI führt nun dedizierte Listen (PointlightAnimatedSkeletalVobs und PointlightAnimatedVobCasters), um bewegliche NPCs sowie getragene Items/Waffen performant im dynamischen Schatten-Pass (SHADOW_CASTER_VOBS) mitzuführen, ohne jeden Frame ungefiltert alle VOBs oder Skelette durchsuchen zu müssen.
  - NPCs, die sich noch im initialen BSP-Tree befinden, werfen nun verlässlich dynamische Schatten.
  - OnVobMoved erkennt Statuswechsel (casterClassificationChanged), wenn NPCs Waffen ziehen oder wegstecken, und invalidiert gezielt die statische Licht-Map, um hängengebliebene Schattenartefakte zu verhindern.
- **Kamin-Erkennung:** Erkennung statischer Kamin-Lichtquellen auf Skelett-VOBs (barbq_, oc_fireplacebig_v01) erweitert.
- **Voreinstellungen:** Im Grafik-Preset "Low" sind dynamische Punktlicht-Schatten nun standardmäßig aktiviert.

## Build 224
- **Cascaded Shadow Maps (CSM):**
  - Kaskadenselektion auf die bewährte projektionsbasierte Logik aus Build 221 zurückgestellt.
  - Experimenteller Tiefen-Cutoff der Kamera entfernt.
- **Shadow-Atlas:** Hardware-basiertes Leeren des gesamten Shadow-Atlas über einen einzelnen globalen ClearDepthStencilView-Aufruf optimiert.
- **Einstellungen:** Robuste ...OrDefault()-Fallback-Funktionen für alle Enum-basierten Grafikeinstellungen implementiert; ungenutztes Preset "Sehr niedrig" (Very Low) entfernt.
- **Punktlicht-Cache:** Caching-Logik (OnVobMoved / OnVobAdded) überarbeitet, sodass animierte NPCs und Attachments die statische Base-Map nicht mehr invalidieren.

## Build 222
- **Build-Abschluss:** Header-Refactoring zur Entkopplung von GothicGraphicsState.h vom MAX_CSM_CASCADES-Makro.
- **Shadow Cascades & Atlas:**
  - Kaskadenauswahl im Shader orientiert sich primär an der Distanz zur Kamera (SQ_ShadowCascadeSplits), um fehlerhafte Nahkaskaden-Zuweisungen auf Distanz zu unterbinden.
  - Weichzeichnung der PCF-Filter (GetCascadeWorldTexelSize) greift direkt auf die Atlas-Dimensionen (SQ_ShadowAtlasSize, SQ_CascadeShadowResolution) zu.
- **F11-Menü:**
  - Standard-Checkboxen auf das einheitliche MenuCheckbox-Format umgestellt.
  - Technische Bezeichnung "XeGTAO" durch "Umgebungsverdeckung" (Ambient Occlusion) ersetzt.
  - Dropdown-Breiten für Profil und Sprache pixelgenau an das zweispaltige Raster angepasst.
  - Schalter für Regeneffekte thematisch zu Wind und Wolken gruppiert.
  - Versionsnummer und Schließen-Button kompakt neben "Erweitert..." platziert.
  - Automatisches Speichern beim Verlassen von Reglern und Schließen des Menüs integriert.
  - Beschreibungen und Tooltips überarbeitet.

## Build 221
- **Build-Abschluss:** HDRToneMapStrength entfernt (statisches Tonemapping); HDR-Saturate im Atmospheric-Scattering-Pass ergänzt.
- **Stabilität:**
  - Sicherheitsprüfungen via Engine::IsShuttingDown() in Engine-Hooks (zCBspTree, zCWorld, zCModel, zCTexture) integriert, um Abstürze beim Beenden abzufangen.
  - Bounds-Checks in der BSP-Polygon-Extraktion (TryGetLOD0Polygons) gehärtet; MainWorld-Pointer wird direkt durchgereicht.
  - OnWorldLoaded wird am Ende von zCWorldLoadWorld zuverlässig mit dem World-Pointer aufgerufen.
- **Atmosphäre & Wolken:**
  - Raymarching der volumetrischen Low Clouds am Horizont von 8 auf 12 Schritte erhöht und Horizont-Fading (skyBottomFade) angepasst.
  - Kantenverfeinerung im Upsampling-Pass (ComputeRefinedSkyLowCloudAlpha) reduziert Raster-Artefakte an der Horizontlinie.
- **Kompatibilität:** Expliziter Cast für u8"Kantenglättung" in ImGui::CalcTextSize für C++20-Konformität (char8_t).

## Build 220
- **Build-Abschluss:** Feinschliff am F11-Menü-Layout und Farbkorrektur für Wasserpartikel bei Nacht.
- **Dithering & Filterung:**
  - Texturbasiertes Blue-Noise-Dithering (TX_OutputBlueNoise) im finalen Gamma-Pass integriert, um Banding über das gesamte Bild zu unterdrücken.
  - Upsampling-Filter für Low Clouds auf 3x3-Kernel umgestellt, um Struktur und Kantenstabilität zu verbessern.
  - Temporales Blue-Noise-Dithering (TX_FogBlueNoise) im volumetrischen Nebel gegen Banding in Nachtverläufen.
- **FSR3:** Scattering-Pass gibt nun fogCompositionMask und fogReactiveMask für präzisere Rekonstruktion transparenter Nebelanteile aus.

## Build 219
- **Vegetations-Culling:**
  - Schieberegler "Vegetationsdichte" (Grass Details 0–4) im F11-Menü für abstandsabhängiges Ausdünnen von Gras- und Farnkarten implementiert.
  - Caching der Schattenkaskaden trackt Änderungen am Vegetationslevel (m_GrassDetailsShadowGeneration).
- **UI & Partikel:**
  - Layout-Raster für Profil und Sprache ausgerichtet; deutsche Bezeichnung "Kantenglättung" wiederhergestellt.
  - Kontrast von Wasser-Gischtpartikeln bei Nacht nachjustiert.

- Korrekturpush: Auto Type Deduction Restore (Build 219)
  - **Auto Type Deduction:** Der Wechsel auf aauto im Vegetation-Culling wurde auf ausdrücklichen Wunsch wiederhergestellt.

## Build 218
- **Vegetation:** "Gegenlicht Vegetation" (Backlit Vegetation / SSS) als Option in den erweiterten Leistungseinstellungen ergänzt.
- **UI:** Bezeichnungen im zweispaltigen Menü gestrafft.

## Build 217
- **Build-Abschluss:** Nightly-Build-Nummer auf 218 inkrementiert.
- **Shader-Permutationen:**
  - Anzahl der CSM-Kaskaden und PCF-Limits als dynamische Runtime-Parameter (SQ_ShadowCascadeRuntimeParams) an Shader übergeben, um Recompiles bei Qualitätswechseln zu vermeiden.
  - Wind, Charakterinteraktion und Godrays auf dynamische Verzweigungen umgestellt.
- **Wasser-Regeneffekte:** Prozedurale Regentropfen auf Wasserflächen optimiert (Berechnung auf direkte Zelle begrenzt, überflüssige Extra-Layer entfernt).
- **UI:** F11-Preset-Abgleich für AO-Parameter korrigiert; Checkbox für gestaffelte Fernschatten bei inaktiven dynamischen Punktlichtern deaktiviert.

## Build 216
- **NPC-Schatten:** In den Presets "Low" und "Very Low" werfen Pointlights keine Schatten mehr auf NPCs, um die GPU-Last in Nachtkämpfen zu senken.
- **Wet-Ground SSR:** Deaktivierung von "Wet-ground SSR" überspringt nur den Screen-Space-Trace; Pfützen behalten ihre Grundreflexion.
- **UI:** Presets beim Speichern synchronisiert (SyncGraphicsPresetSelection); Option für Regeneffekte im F11-Menü eingepflegt.

## Build 209
- **Pointlight PCSS & Schatten-Stabilisierung:**
  - PCSS (Percentage-Closer Soft Shadows) für Pointlights mit 5-Tap Blocker-Suche zur dynamischen Penumbra-Berechnung und 13-Tap-Filterung eingeführt.
  - Fallback für weit entfernte Lichtquellen über schnellen Far-PCF-Pfad; Kontakt-Hardening überarbeitet.
  - Atmospheric Scattering: Subtile indirekte Nachtausleuchtung integriert, um das Absaufen von Innenräumen und abgewandten Flächen zu verhindern.
  - Cascaded Shadow Maps für animierte Charaktere stabilisiert (ComputeCascadedShadowValueCharacter: Kaskadenauswahl nach Originalposition, Normal-Offset und Bias erst für gesampelte Kaskade; Übergabe von SQ_CascadeLightDirectionWS).
  - Rollback auf Build 209 (Build 210 verworfen).

## Build 208
- **Vegetations-Wind:**
  - Wind-Physik in VS_ExInstancedObj.hlsl auf räumliche Berechnung (World-XZ) und Slenderness-basiertes Biegemodell umgestellt.
  - 3x3-Velocity-Dilation in PS_PFX_Velocity.hlsl durch direkten Lookup ersetzt.

- Korrekturpush: Feinschliff in Build 208. Das Vegetation-Wind-System wurde perfektioniert (VS_ExInstancedObj.hlsl): Der Wind wird nun zwingend im World-Space nach der Instanz-Transformation angewendet, sodass sich Pflanzen bei Rotation im Spacer nicht mehr "in die falsche Richtung biegen". Das System nutzt nun zudem die reale Bodenneigung (WindGroundPlane aus WorldObjects.h), anstatt stur die lokale Bounding-Box für das Biegungsverhalten heranzuziehen. In PS_Water.hlsl fadet der Jharkendar-Ocean-Tint in flachem Wasser sanfter aus, damit die Küstenübergänge weich bleiben. Für die PCSS-Schatten wurden die Tap-Zahlen (ShadowSampling.h) für das No-TAA-Fallback auf robustere Werte kalibriert.
- Korrekturpush: Weiterer Feinschliff in Build 208. In D3D11GraphicsEngine.cpp wurde die Zuweisung für die WindGroundPlane repariert (direkte float3 -> XMVECTOR Konvertierung statt XMLoadFloat3, um eventuelle Alignment-Probleme zu umgehen). In der Deferred Shading Pipeline (Legacy & Tiled) wurde ein genialer Kniff eingebaut: Der Distanz-Tier für die Pointlight-Schatten wird nun direkt in der ShadowSoftness encodiert (über einen 16.0f Marker). So können die Shader erkennen, ob sie weite Schatten mit weniger Taps rendern sollen, ohne dass der ohnehin heiße Constant-Buffer wachsen muss!
- Korrekturpush: Abschließender Wind-Polishing in Build 208. In VS_ExInstancedObj.hlsl (und D3D11GraphicsEngine.cpp) wurde zwischen "Gras" und "Bäumen" unterschieden (abgeleitet aus VisualCamAlign). Gras-Patches nutzen nun eine affine Terrain-Scherung (signedTerrainShear), was die hässlichen inversen Deformationen bei fehlerhafter Platzierung verhindert und den Boden-übergang stationär hält. Bäume behalten ihr non-lineares Biegungsverhalten. Außerdem wurde die Root-Gewichtung nun endlich auch auf die dynamische Charakter-Interaktion (Hero läuft durch Busch) angewandt, sodass Büsche nicht mehr an der Wurzel vom Boden abreißen!
- Korrekturpush: Performance- & Stabilitäts-Update für das neue Wind-System (Build 208). In VS_ExInstancedObj.hlsl wurde für Bäume (grassShearProfile < 0.5f) wieder ein strikt rigider Stamm (untere 12%) erzwungen, damit Gelände-Ankerpunkte niemals Baumstämme verschieben. Es wurde ein Distanz-LOD für den Wind eingeführt (windDistanceFade & detailLod), das hochfrequente Blatt-Turbulenzen auf Entfernung ausblendet und auf extreme Distanzen den Wind komplett abschaltet. Zur massiven Einsparung von ALU-Zyklen wird der vorherige Wind-Zustand (previousWorldWindOffset) für Motion Vectors nur noch berechnet, wenn accurateWindVelocity (TAA/FSR) aktiv ist. In C++ wird die WindGroundPlane nun über WindGroundPlaneInitialized effizient gecacht.
- Korrekturpush: Finales PointLight-Shadow & Lighting Update in Build 208. In PointLightShadows.h wurde der Poisson-Kernel von 12 auf 8 Taps reduziert, dafür aber mit einer kamera-unabhängigen Rotation (PLS_StableWorldNoise) versehen. Das bricht das Banding auf, ohne temporäres Flimmern zu verursachen! In ForwardPlusLighting.hlsl wurde das Auslesen der ShadowCube-Arrays anhand der neuen Bitmask-Tiers (StaticLow, Dynamic) implementiert. Zudem wurde der veraltete, rechenintensive ComputeIndoorDoorFloorBleed entfernt – Indoor-Lichter bleiben nun durch einen simplen Maskierungs-Check sauber vom Outdoor-Terrain getrennt, ohne unschöne Contour-Bands an Türen zu erzeugen.
- Korrekturpush: Rollback des Shadow-Noise und Slot-Fix in Build 208. Die Rotation des PointLight-Kernels (PLS_StableWorldNoise) und das Distance-Tier-Encoding wurden vorerst wieder entfernt, da sie offenbar Nebeneffekte hatten. Der Kernel bleibt nun bei statischen 8-Taps, was stabiler sein sollte. Zusätzlich wurde das FP_DynamicShadowCubeArray in ForwardPlusLighting.hlsl von Slot 	13 auf 	21 verschoben, um potenzielle Kollisionen im Ressourcen-Binding (z.B. mit anderen Texturen) zu verhindern.
- Korrekturpush: Software-PCF für PointLight-Schatten in Build 208. Da die Cubemaps ohnehin lineare radiale Tiefenwerte speichern, wurde das Hardware-PCF (SampleCmpLevelZero) durch einen manuellen Software-PCF Ansatz über einen linearen Sampler (SampleLevel) und smoothstep ersetzt. Das verhindert effektiv, dass sehr weite Shadow-Softness-Kernels die binären PCF-Coverage-Level sichtbar freilegen (Banding-Artefakte). Dementsprechend wurden die Sampler in Tiled-Shading, Forward-Plus und DynShadow auf SS_Linear umgebogen.
- Korrekturpush: Hardware-PCF Comeback und Adaptive Shadow-Taps in Build 208. Das Software-PCF-Experiment wurde verworfen und auf Hardware-PCF (SampleCmpLevelZero) zurückgerollt. Stattdessen wurde nun ein adaptives Distance-LOD für die Pointlight-Schatten eingebaut (ReceiverCameraDistance): Ab einer Softness > 0.75 interpolieren im Nahbereich (< 5m und < 2m) dynamisch bis zu 8 zusätzliche, dichte Filter-Taps stufenlos hinzu. Dadurch bleibt das Shadow-Sampling in der Ferne bei performanten 8 Taps, während Kanten im Nahbereich butterweich verschmelzen und Banding verstecken. Die Sampler wurden entsprechend wieder auf SS_Comp zurückgesetzt.
- Regulärer Push: Abschließende Optimierungen in Build 208. Das adaptive Shadow-LOD wurde verworfen und stattdessen durch eine massive Hardware-Beschleunigung ersetzt: Für extrem weiche Pointlight-Schatten (shadowSoftness > 0.75) nutzt die Engine nun die GatherCmp-Instruktion. Damit werden mit nur 4 Texture-Fetches gleich 16 Tiefenwerte gesampelt (Hardware-PCF x4 pro Fetch). So erhalten weite Pointlight-Schatten nun butterweiche 16 Taps zum Preis von 4, ohne jegliche Distanz-Zonen oder komplexes LOD-Management!

## Build 207
- **City-Windows:** Screen-Space-Prüfung für Sky-Protection in PS_Simple implementiert (unteres Bildschirmdrittel über Himmelstiefe wird opak).
- **Motion Vectors:** Statische Instanzen in VS_ExInstanced rekonstruieren ihre World-Space-Position für saubere FSR3-Reprojektion.
- **Motion Blur:** 3x3-Velocity-Dilation in PS_MotionBlur durch direkten Lookup ersetzt.

- Regulärer Push: Abschließende Optimierungen in Build 207. Einführung eines dynamischen Ocean-Climates: Je nach Welt (ADDONWORLD vs Khorinis) hat das Ozean-Wasser in PS_Water nun unterschiedliche Streu- und Absorptionsprofile sowie einen spezifischen Luma-neutralen Tint, der bei Regen und Nacht physikalisch korrekt abnimmt. In D3D11ShaderManager wurde die Shadow-Kernel-Qualität (PCF/PCSS Taps & Blue Noise) nun direkt an die TAA/FSR3-Rekonstruktion gekoppelt: Ohne TAA nutzt der Renderer deutlich mehr Taps und verzichtet auf stochastisches Noise, um Stippling auf animierten Charakteren zu vermeiden.

## Build 206
- **Wasser:** Geometrie-Split in D3D11GraphicsEngine und PS_Water.hlsl vereinfacht (Flackern an schmalen Übergängen behoben).
- **Schatten:** Penumbra-Dithering in ShadowSampling.h zur Reduktion von PCF-Banding ergänzt.
- **Nebel:** Gothic-Heightfog verdeckt weit entfernte Geometrie tageszeitunabhängig vollständig.

- Korrekturpush: Revert und Redesign in Build 206. Einige experimentelle Features des ersten Pushes (CSM-Schatten-Dither, Water-RenderMode-Split, Compute-Fallback in PS_Simple) wurden wieder verworfen. Dafür wurde das Output-Dithering in PS_PFX_GammaCorrectInv deutlich nachgeschärft: Es bleibt nun bis in die hellsten Bereiche (0.995f) aktiv, da Quantisierung auch in hellen, flachen Gradienten stark sichtbar war. Der Daytime Gothic-Fog (PS_PFX_Heightfog) nutzt nun weichere Fade-Grenzen für das Ausblenden der Geometrie und erzwingt im Hintergrund zwingend die Nebelfarbe, um unschöne blaue Silhouetten in der Ferne zu tilgen.
- Korrekturpush: Redesign der City-Window Sky-Protection (PS_Simple). Der Compute-Shader Ansatz (TX_WindowSkyVisibility) wurde verworfen und durch einen genialen, analytischen Texture-Space Raycast ersetzt. Die 5 Fensterreihen der City_Window Textur werden nun über Derivate (ddx, ddy) präzise in den Screen-Space projiziert, wodurch die Sichtbarkeit jeder Reihe unabhängig ermittelt werden kann. In ShadowSampling.h wurde das Penumbra-Dithering wieder entfernt und stattdessen eine dynamische Weichzeichnung bei flachen Einfallswinkeln (grazingFootprint) eingeführt. Zudem wurde in PS_World.hlsl der WorldGeometryMask Export aufgeräumt.
- Korrekturpush: Perspektivisch korrekte Fensterprojektion & Nebel-Tuning. In PS_Simple.hlsl wurde die Screen-Space Projektion der Fenster-UVs massiv verbessert (WindowUvToScreenPosition rechnet nun mit UV/W statt linearen UVs, um Verzerrungen zu vermeiden). In PS_PFX_Heightfog.hlsl blockt Gothic-Nebel Geometrie in der Ferne nun unabhängig von der Tageszeit konsistent aus (worldFogOpacity greift das FarOcclusion-Gewicht auf). In ShadowSampling.h wurde das Grazing-Footprint-Experiment wieder verworfen.

## Build 205
- **Partikel:** GetGothicTexture() in MyDirectDrawSurface7.h für Partikel-Texture-Swaps in GothicAPI ergänzt.

- Korrekturpush: Feintuning in Build 205. Die Output-Dithering-Kurve in PS_PFX_GammaCorrectInv wurde angepasst (Dithering bleibt nun über einen weiten Luminanzbereich aktiv), was Banding-Artefakte an Wänden und Laub extrem reduziert. In PS_ParticleDistortion und PS_ParticleSimple wurde die Tag/Nacht-Wichtung für Wasserpartikel optimiert. In PS_Simple wurde der alte Screen-Space Connectivity-Test für das untere Bildschirmdrittel der City_Windows verworfen, da Tiefe nahe Null dort dem Gothic-Void/Boden entspricht und nicht dem Himmel.
- Regulärer Push: Abschließende Optimierungen in Build 205. In PS_Water.hlsl wurde die Shore-Fade-Logik repariert (Wasser verblasst nun nicht mehr fälschlicherweise in die leere Szene, wenn flaches Land dahinter liegt, und schmale Wasserfälle werden nicht mehr weichgezeichnet). In ShadowSampling.h nutzt der Fallback-Pfad ohne Blue-Noise nun ebenfalls Hash-Rotation, um hartes Banding der PCF-Schatten zu verhindern. In WorldObjects.h wurden neue Properties (WaterGeometryClass) für das kommende Water-Refactoring vorbereitet.

## Build 204
- **Shader-Konstanten:** MAX_SHADER_CB von 6 auf 14 (D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT) erhöht, um Out-of-Bounds-Zugriffe bei Shader-Reflektionen zu verhindern.

- Korrekturpush: Compute Shader WindowSkyVisibility. Die teure Screen-Space Raycast-Logik für City_Windows wurde in einen eigenen Compute Shader (CS_WindowSkyVisibility.hlsl) ausgelagert, der das Ergebnis nun pro Frame in einer reduzierten Maske cacht. PS_Simple greift nur noch auf diese Textur zu, was die Fragment-Shader deutlich entlastet. Zudem wurde der Alpha-Bereich für das Glas sanft abgedunkelt.
- Korrekturpush: Feintuning an WindowSkyVisibility und OilLamps. In D3D11GraphicsEngine wird das SRV nun explizit ungebunden, BEVOR der Compute-Shader aufgerufen wird, um D3D11 Hazard-Warnungen (gleichzeitiges Binden von SRV und UAV) zu beheben. Die SkyGuard-Distanz für Fenster wurde von 2000-3000 auf 1000-1500 reduziert, um den Effekt im Nahbereich schneller greifen zu lassen. Für Öllampen (GothicAPI) wurde die StrongChromaticSaturation von 0.55 auf 0.75 erhöht, damit leicht gefärbte Flammen öfter den weißen Fallback nutzen.
- Regulärer Push: Abschließende Optimierungen in Build 204. Implementierung eines Textur-Swap-Mechanismus für Partikel (Dark/Bright Textures wie FIRESMOKE_DARK zu FIRESMOKE) in GothicAPI, um dynamische Verdunkelung zu unterstützen. Öllampen mischen nun einmalig die Farben aus statischen und dynamischen Lichtquellen (MixOilLampEmissionColors), sodass die Lampen-Emission stabil bleibt und nicht mehr unangenehm mit Licht-Animationen flackert.

## Build 203
- Korrekturpush: CBuffer/CPU-InverseView Architektur für Window-Cutouts. Die WorldPosition-Exporte aus den Vertex Shadern wurden zurückgerollt, stattdessen wird die Inverse View Matrix einmalig pro Frame auf CPU-Seite berechnet und in den CutoutConstants-CBuffer hochgeladen. Dies vermeidet Vertex-Shader-Aufblähungen und erzielt das gleiche Ergebnis robuster im Pixel Shader.

- Korrekturpush: Tile-based Window Cutouts implementiert. Die Cutout-Schleife im Pixel Shader wertet nun eine 2D-Tile-Maske aus, um nur noch die Fensterausschnitte zu berechnen, die den aktuellen Bildschirm-Tile schneiden. Das verbessert die Shader-Performance massiv.
- Korrekturpush: OilLamp Emission Logic überarbeitet. Bei der Zuweisung von Lichtern an Öllampen (ConfigureAllPointlightShadowSources) werden nun statische Lichter im Suchradius bevorzugt. Nur wenn kein statisches Licht gefunden wird, greift das System auf das nächste dynamische Licht zurück.
- Korrekturpush: MeshVisualInfo um WindowGlassBounds erweitert für präziseres Bounding-Box-Tracking von Fensterscheiben.
- Korrekturpush: C++ Typecast-Fix in der Window-Cutout-Logik (Korrektur von BaseVisualInfo auf MeshVisualInfo per dynamic_cast).
- Korrekturpush: C++ XMMATRIX Transpose-Fixes. Gothic speichert Vob-Matrizen standardmäßig für Shader (Column-Major) transponiert ab. Für Berechnungen auf der CPU (DirectXMath) müssen diese zurück-transponiert werden. Das korrigiert die Orientierung der Window-Cutout-Bounds und die Richtung der Öllampen-Licht-Offsets. Zusätzlich wurde der Y-Offset für Öllampen-Schatten auf 20.0f reduziert.
- Korrekturpush: Ocean Water Drift gefixt (PS_Water.hlsl). Die Weltkoordinaten für die Distortion-Animation werden für den Ozean nun um 90 Grad gedreht, damit die Wellenbewegung in die korrekte Richtung verläuft.
- Korrekturpush: City-Window Validierung & Counterparts (GothicAPI). Gegenüberliegende City_Window-Instanzen (innen/außen) werden nun automatisch verknüpft, um zu prüfen, ob die Transparenz an dieser Stelle gültig ist (verhindert Löcher in Wänden, wo keine Cutouts existieren). In PS_Simple.hlsl wurde ein spezieller FFDATA-Pfad für den transparenten Teil ergänzt.
- Korrekturpush: Feintuning der Öllampen-Schatten und Ocean-Water-Drift. Der Y-Offset für Öllampen-Schatten wurde wieder auf 50.0f und der Forward-Offset auf 25.0f angepasst. Die Ozean-Rotation in PS_Water.hlsl wurde um weitere 180 Grad gedreht (-Z, X), um die exakte Original-Laufrichtung zu treffen.
- Korrekturpush: City-Window Validierung extrem vereinfacht. Die fehleranfälligen Sonden für Fußböden und Decken wurden entfernt. Die Fenster fallen nun standardmäßig auf transparent (fail open) zurück, es sei denn, eine Mehrheit der Abtastpunkte findet definitiv eine parallele Wand hinter dem Cutout.
- Korrekturpush: Öllampen-Emission gelockert. Bei der Suche nach statischen oder dynamischen Lichtern für die Öllampen-Emission wird nicht mehr streng gefiltert, ob das Licht schon von einer normalen Flamme "verbraucht" wurde, was zuvor dazu führte, dass viele Lampen dunkel blieben. Es wurde auch ein Log hinzugefügt, das die Emission-Links aufschlüsselt.
- Korrekturpush: Ocean Water Animation (PS_Water.hlsl) finalisiert. Statt die Koordinaten noch einmal umständlich zu drehen, wird nun für den Ozean einfach die Animationszeit (RI_Time) umgekehrt (-1.0f). Dadurch laufen die Wellen exakt auf die Küste zu, ohne dass sich die Form der Wellen verzieht oder Flüsse beeinträchtigt werden.
- Korrekturpush: Shader-Refactoring und Optimierungen. Die Window-Cutout-Schleife (PS_Diffuse.hlsl) nutzt nun 'firstbitlow' für noch schnellere Iterationen durch die Tile-Maske. Die Öllampen-Emission wurde in einen dynamischen Branch ausgelagert und eine harte Discard-Regel für den Alphatest von City_Windows (auch im Forward-Path) eingeführt. Kleinere Aufräumarbeiten im Water-Shader (Variablennamen).
- Korrekturpush: City-Window Backface-Culling anstatt Counterpart-Pairing (GothicAPI). Die umständliche Logik, gegenüberliegende City_Window-Instanzen zu paaren, wurde komplett verworfen. Stattdessen liest die Engine nun direkt beim Parsen der Mesh-Geometrie die Front-Normale der realen Glasscheibe aus. In CollectVisibleVobs werden nach hinten zeigende Fenster (die untexturierte Rückseite) nun direkt aussortiert und gar nicht erst in die Render-Queue aufgenommen. Das ist extrem elegant und performant!
- Korrekturpush: City-Window Backface-Culling Feintuning und RenderState-Refactoring. Das Backface-Culling für Fenster hat nun eine Toleranz von 5 Grad (BackfaceToleranceSinSq), um ein hartes Aufpoppen an scharfen Kanten zu vermeiden. In DrawFrameAlphaMeshes wurde das State-Management optimiert (CullMode für Glas auf CULL_NONE gesetzt, um falsche Winding-Orders von Innen-Meshes abzufangen, und redundante State-Updates eliminiert). Zusätzlich wurden BSP-Tree-Guards eingefügt, um Abstürze in frühen Ladephasen von Gothic zu verhindern.
- Korrekturpush: City-Window Sky-Safeguard und Particle-Lighting (Nightly). In PS_Simple.hlsl wurde ein intelligenter Sky-Safeguard für City_Windows implementiert: über eine vertikale Probe (EvaluateWindowSkyPath) wird nun im Screen-Space geprüft, ob der Weg zum Himmel durch Weltgeometrie verdeckt ist; falls ja, blendet das Fenster auf opak über. In den Particle-Shadern wurde eine stärkere nächtliche Verdunkelung für Bodennebel (groundFog) integriert. Außerdem schreibt opake Weltgeometrie nun korrekterweise WorldGeometryMask = 1.0f.
- Korrekturpush: City-Window Textur Update und Sky-Safeguard Verbesserungen. Die vertikalen Probes in PS_Simple.hlsl wurden von 8 auf 24 erhöht, um keine Wandstreifen mehr zu überspringen. Die DDS-Textur City_Window.dds wurde aktualisiert und der Shader erzwingt nun ein Minimum an Alpha (0.18f), damit das Glas immer sichtbar bleibt, selbst wenn die Textur voll transparent ist.
- Korrekturpush: Particle Night-Tint und Transparency-CBuffer. Bodennebel (und andere Partikel) nutzen nun GetAmbientNightWeight() und erhalten über ApplyAmbientNightTint eine echte nächtliche Tönung. In PS_Transparency wurde zudem ein GA_LightingTint Parameter in den Constant Buffer aufgenommen.
- Korrekturpush: Legacy Waterfalls Hue-Correction (PS_Water.hlsl). Steile Wasserfälle erhalten nun dieselbe Scene-Hue-Korrektur wie horizontales Wasser, allerdings erst nach der Reflection/Cubemap-Komposition, um Farbabweichungen in den Rot/Blau-Kanälen zu verhindern.
- Regulärer Push: Abschließende Optimierungen in Build 203. EvaluateWindowSkyPath bricht nun im [loop] frühzeitig ab, wenn das Ergebnis feststeht, und die horizontalen Probes werden nur ausgeführt, wenn der mittlere Pfad blockiert ist. Die Cache-Logik in GothicAPI nutzt std::atomic_bool. Atmospheric Scattering wird nun korrekt NACH der Öllampen-Emission angewendet.

## Build 202
- Korrekturpush: Nightly-Fix für Shader. Die WorldPosition wird nun korrekt als echte Weltkoordinate aus dem Vertex Shader (VS_Ex und VS_ExInstancedObj) exportiert, wodurch die Window-Cutout-Logik und das korrekte Clipping in den Pixel Shadern (z. B. GBuffer und Forward-Rendering) wieder fehlerfrei arbeiten.

## Build 200
- Korrekturpush: GitHub-Build repariert / korrigiert.
- Korrekturpush: Weitere Fixes (zweiter Upload).
- Korrekturpush: D3D11PfxRenderer Fix.
- Korrekturpush: Low-Cloud-Dimensionen vorerst auf /2 zurückgesetzt (Fehlersuche für /4 dauert an).
- Korrekturpush: Manuelle Code-Zentralisierung für cloudRes und Anpassungen auf /4 durch den Benutzer übernommen.
- Korrekturpush: Viewport in RenderLowCloudLayer liest Dimensionen nun laufzeitsicher direkt aus der RenderTarget-Textur, um Asynchronitäten bei /4-Auflösung auszuschliessen.
- Korrekturpush: Viewport in RenderLowCloudLayer greift statt auf dynamisches RenderTarget-Auslesen wieder auf eine lokale cloudRes-Berechnung zurück (synchronisiert auf /4).
- Korrekturpush: Signatur-Mismatch behoben (cloudRes-Parameter in D3D11PfxRenderer.h analog zur .cpp entfernt).
- Korrekturpush: Erneuter Rollback zur dynamischen Viewport-Dimensionierung via RenderTarget-GetDesc() in RenderLowCloudLayer (Sicherstellung der Sync-Integrität).
- Korrekturpush: Diverse manuelle Feinabstimmungen (DoF default an, dynamische Wolken default aus, reduzierte RainFog-Opacity, erweiterte Tag/Nacht-Farbtonkorrektur für WetGroundSSR und Ozean-Reflexionen).
- Korrekturpush: Kaskaden-Frustum für Shadow-Weltmesh wiederhergestellt (behebt massiven Triangle-Overhead durch AlwaysContainingFrustum-Workaround).
- Korrekturpush: Sky-Edge-Blur im DoF-Composite implementiert und statische Silhouette-Confidence entfernt, um Himmelsübergaenge unscharfer Objekte weicher zu mischen.
- Regulärer Push: Umstellung der Release-Tags auf explizite Versionierung (v18.0) und Entfernung des Build-Prefixes aus Release-Artefakten.
- Korrekturpush: Access Violation beim Prozessende behoben (Shutdown-Logik aus DllMain entfernt). Inverse-Gamma-Korrektur für Groundfogs korrigiert (35% Sichtbarkeit wiederhergestellt). SSR-Fallback-Cubemap für Wasser bei Nacht abgedunkelt.
- Korrekturpush: Groundfog-Erkennung erweitert (Swampfog/Dunst in NewWorld wird wieder vom Renderer als echter Nebel mit korrektem Culling und Lighting behandelt). Weichere Uferübergaenge beim Color-Blending des Legacy-Wassers hinzugefügt.
- Korrekturpush: Groundfog-Erkennung (NewWorld Sumpfnebel) überarbeitet (sichere Erkennung über Firesmoke-Textur und BlendMode statt generischer Namenssuche, um False-Positives bei magischen Effekten/Feuer zu vermeiden).
- Korrekturpush: Groundfog-Erkennung weiter verfeinert (Vermeidung von False-Positives bei echten Rauch-Effekten wie humansmoke durch zusätzliche Namensausnahmen, Emissive-Logik bereinigt).

## Build 199
- Regulärer Push: BspPortalCuller (Portal Culling System) implementiert und in GothicAPI, WorldObjects, ShadowMap und PointLight verknüpft.

## Build 198
- Regulärer Push: NightFogRainFade Polynom-Glättung in D3D11PfxRenderer.cpp (verhindert stotternde Nebelübergaenge durch nicht-lineare Interpolation); Anpassung der Fade-Speeds (0.35/0.55) für ein flüssigeres Ingame-Erlebnis.

## Build 197
- Regulärer Push: NightFogRainFade-Tracking in D3D11PfxRenderer und PS_PFX_Composition.hlsl (weicheres Ein-/Ausblenden des Nebels nachts bei Regen); manuelle Anpassungen an PS_PFX_WetGroundSSR.hlsl; Caching-Optimierung in D3D11ShadowMap.cpp (Update-Threshold).

## Build 196
- Regulärer Push: Pfützen-Zeitsteuerung in GothicAPI von Echtzeit auf Ingame-Zeit umgestellt; SSR-Raymarching in SSR.h mit Viewport-Clipping optimiert; WetGroundSSR-Tracing auf 256 Steps/2.0 Stride verfeinert, Wet-Mask-Exposure an RainFXWeight gekoppelt und GetRainExposure auf PCF-Filter umgebaut; ContactShadow-Tageszeitkopplung in D3D11PfxRenderer gelöst; PS_ParticleSimple.hlsl manuell aktualisiert.

## Build 195
- Regulärer Push: SSR-Raymarching in SSR.h zentralisiert; LowCloud-Refinement in PS_PFX_LowCloudComposite.hlsl auf 4 Steps optimiert; AdvancedSettings in ImGuiShim und GSky.cpp entfernt; NPC-Tagging in Diffuse und LightingTrace integriert.

## Build 194
- CS_PFX_GodRayZoom.hlsl: Radial Blur wiederhergestellt, Legacy-Lens-Flare entfernt, volumetrische Phasenfunktion vereinheitlicht, Gewichtung bei Combine angepasst.
- PS_PFX_LowClouds.hlsl: Breiter Sun-Backlight-Pfad (Broad Sun Mask) mit separaten Dichtemasken für Body und Thin-Edge implementiert.
- PS_Water.hlsl: Shore-Intervalle vereinheitlicht, Reflexionen für Ocean-Geometry gedämpft und stabilisiert.
- PS_PFX_LowCloudComposite.hlsl: Geometrie-Wolken erhalten bei niedriger Konfidenz im 5x5-Fenster einen volumetrischen Fallback (ComputeRefinedLowClouds) mit voller Raymarch-Integration (inkl. PFXBuffer-Übergabe in D3D11PfxRenderer.cpp).

## Build 193
- Regulärer Push: Radiale GodRays überarbeitet: AC_SunVisibility aus der Gewichtung entfernt. Sky-Alpha-Maskierung in CS_PFX_GodRayMask eingeführt. Zoom-Pass extrahiert shaftProfile über Sampling-Varianz zur Objektkantenerkennung. Combine-Pass nutzt shaftProfile für saubere, trennscharfe Überblendung zwischen gedämpftem Himmels-Radial und ungedämpften Lichtstrahlen an Kanten, ohne das Lens-Flare zu beeinflussen.

## Build 192
- Implementiert: Contact Shadows sind (bei aktivem FSR 3) auf Innenräume beschränkt. Ein weicher Überblendungseffekt (Transition) sorgt für fließende Übergänge.
- Implementiert: Nahbereichs-Godrays (Near Shaft Scattering) für Lichtstrahlen in direkter Kameranähe.
- Anpassung: Legacy-Wasser-Nachthelligkeit wird nun ausschließlich uferabhängig relativ skaliert (kein globales Aufhellen).
- Anpassung: Cubemap-Spiegelungen auf Wasser beachten nun die Hemisphäre (hemi).
- UI: "Volumetric Lighting" in "Light Shafts" umbenannt. Tooltip für Kontaktschatten aktualisiert.

## Build 191
- Add invalid sky layer marker to low clouds
- Fix validity-aware 2x2 filter for stable sky low clouds
- Enhance water glint transmission based on reflected cloud coverage
- Improve Godrays with radial luminance compression and dynamic litFraction
- Add Temporal Reprojection (TAA) for Volumetric Godrays to increase sample stability

## Build 190
- Regulärer Push: Low Clouds um SkyClouds-Target in PS_PFX_LowClouds.hlsl und PS_PFX_LowCloudComposite.hlsl erweitert, um Artefakte an Alpha-Test-Silhouetten zu beheben. D3D11PfxRenderer und RenderGraph in D3D11GraphicsEngine.cpp an das neue Target angepasst; E_GodRayMode Persistenz und Menü (GothicAPI.cpp, GothicGraphicsState.h, ImGuiShim.cpp) korrigiert.

## Build 189
- Volumetric Lighting (Godrays) überarbeitet: UI auf einen einzigen Ein/Aus-Schalter mit gekoppeltem Stärkeregler vereinfacht.
- Interne Feature-Level-Normalisierung: DX10-Hardware fällt automatisch auf radiale (Low) Lichtstrahlen zurück, während DX11-Hardware volumetrische (High) Godrays nutzt.
- Presetvergleiche und INI-Speicherung auf den neuen EnableGodRays-Master-Schalter umgestellt.

## Build 188
- Regulärer Push: Korrektur der GodRay-Volumen-Berechnung (MaxDistance, LightColor, GlobalDensity, WeightZNear/Far) in D3D11PFX_GodRays.cpp; Anpassung des LightDirection-Skalarprodukts in CS_PFX_GodRayZoom.hlsl; Fallback-Logik für fehlende GodRay-Composition in D3D11GraphicsEngine.cpp.

## Build 187
- Regulärer Push: Neue Vegetationsdichte-Option über topologische Nachgruppierung (Disjoint-Set) integriert, um Mesh-Flackern durch gezieltes Entfernen ganzer Äste/Büsche zu vermeiden. E_GodRayMode für volumetrische GodRays eingeführt und in die F11-Menü-Presets integriert.

## Build 186
- Editor-Widget-Klassen und ImGuiEditorView rückstandsfrei entfernt (inklusive BaseWidget, EditorLinePrimitive, GVegetationBox, WidgetContainer, Widget_TransRot).
- GothicAPI und Launcher-Schnittstellen vollständig von LoadCustomZENResources und weiteren Editor-Exporten bereinigt.
- Wet Ground SSR: Puddle Geometric World Normal-Berechnung integriert, um Normal-Verzerrungen aus den Puddle-Masken für Wasseroberflächenreflexionen zurückzunehmen.

## Build 185
- Implement volumetric height fog candidate selection for world and rain (Blocks 1-18)

## Build 184
- Regulärer Push: Korrektur der `ApplyMaterialCompatibility`-Funktion, Überarbeitung der WetGroundSSR-Materialabhängigkeiten und Integration neuer Texturen/Material-Updates für verbesserte Stabilität und Konsistenz.

## Build 183 (Korrekturpush)
- Korrekturpush: DoF-Composite repariert, fehlerhafter SkyEdgeBlur-Rekonstruktionspfad entfernt und durch stabilen early-out für Sky-Pixel (sharpColor) ersetzt, um Artefakte an Alpha-Test-Silhouetten zu beheben.

- Regulärer Push: F11-Menü aufgeräumt, Preset-Entkopplung für Regendarstellung durchgeführt, kompakte Anti-Aliasing-Zeile implementiert, VSync/FPS-Limit umstrukturiert und doppelte Rain-Rendering-Zeile entfernt.

## Build 182
- Scene Wetness: Fallback auf distortion.dds ohne AC_RainFXWeight bei deaktivierten Surface Details.
- Wet Material Reflections: Standardstärke auf 1.5f angehoben.
- Wet Ground SSR: Materialien mit wetGroundSSRStrength 0.0 vollständig von prozeduralen Pfützen ausgeschlossen (materialPuddleEligibility via step).
- HDR: Dateiname von hdr.h zu HDR.h in Git korrigiert.

- Korrekturpush: Fehlende Struct-Member Definition und Reset-Zuweisung für DisableTransparentWorldMeshDepthFogReplay in RendererTestSettings.h sowie zugehörige F11-Checkbox in ImGuiShim.cpp korrigiert (Fix für Compilerfehler C2039/C2737 in Release_G1_AVX2).

## Build 181
- Wet Ground SSR: Constant-Buffer-Layout WetGroundSSRConstantBuffer und F11-Teststärken (WG_WetMaterialReflectionsStrength, WG_ProceduralPuddlesStrength, WG_PuddleReflectionsStrength, WG_WetGroundRainImpactsStrength) in HLSL (PS_PFX_WetGroundSSR.hlsl) und C++ (ConstantBufferStructs.h, D3D11PfxRenderer.cpp) vollständig synchronisiert (288 Bytes) und für materialWetMask, puddleMask, rippleDistortion und puddleReflectionBlend angewendet.

## Build 180
- Wet Ground SSR: Sampler s0/s1 werden vor dem DrawFullScreenQuad gesichert und danach wiederhergestellt, um Shadow-Comparison-Sampler Leaks im Transparenz-Pass zu verhindern.
- Rain Shadowmap: FF_AlphaRef auf 0.75f korrigiert, damit Alpha-Test-Vegetation regendurchlässig bleibt.
- Atmospheric Scattering: Einheitliche Umschaltkurve `GetRainCloudTransitionWeight()` eingeführt und Test-Flag `UseNightlyGroundRainInput` restlos entfernt.

## Build 179
- Korrekturpush: Fix GitHub-Buildfehler C2338 (16-byte aligned WetGroundSSRConstantBuffer). Padding auf CPU- und HLSL-Seite von float3 auf float2 korrigiert, um exakt 272 Byte Größe und 16-Byte-Ausrichtung zu erreichen.
- Atmosphere & Sky: AtmosphericRainWeight-Zustandsmaschine in GSky::RenderSky() und ResetWeatherState() entfernt. Himmel, RainClouds, Ausblendung der Dynamic Clouds, Sonnen-/Mondsichtbarkeit, atmosphärische Bodenabdunklung und entfernte Geometrie verwenden nun pro Frame ausschließlich den gemeinsamen, bereinigten Wert aus GothicAPI::GetRainFXWeight().

## Build 178
- PfxRenderer: RainFogColor, RainFogDensity und FogRange Parameter übergeben.
- Wet Ground SSR: ApplyWetGroundRainHaze für echten volumetrischen Regennebel angepasst.
- GothicAPI: Die DRAGONISLAND Wolkendeaktivierung entfernt.
- GothicAPI: Zehnminütige Pfützennachwirkung (SceneWetness) nach Regenende eingeführt.
- Water: Prozedurale Regenringe auf horizontalem Ozean- und Legacywasser eingefügt, inkl. Unterwasserkorrektur.

## Build 177
- D3D11GraphicsEngine: Beschädigten duplizierten Block in OnStartWorldRendering vollständig entfernt.
- Wet Ground SSR: Regenplätschern durch weltverankerte Regentropfeneinschläge ersetzt, künstliche Pseudoreflexionen entfernt und korrekte Inaktivierung bei aussetzendem Regen sichergestellt.

## Build 176
- Vollständiger und restloser Rückbau der Transparent-World-Coverage-Sonderlösung für Wet Ground SSR.
- Implementierung der DDA-freien Raymarch-Pfützenlogik.

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

- Transparent World Meshes: Made PS_Simple_FF shader standard for BLEND and ADD alpha functions; removed diagnostic UseNightlyBlendShaderForTransparentWorldMeshes toggle.
- Transparent World Meshes: Added Transparent World Mesh Brightness multiplier to F11 diagnostics, strictly targeting RGB.
- Diagnostics: Added Disable Wet Ground SSR and Disable Transparent World Mesh Depth/Fog Replay toggles to F11 transparency menu.

- Entkopplung von Wet Ground SSR von den Specular-Werten (eigenständiger Wert wetGroundSSRStrength 0.0 - 1.0).
- Überarbeitung des Regentropfen-Grizzle-Effekts für Wet Ground SSR (kurze statische Impulse statt UV-Verschiebung).

## Build 170 (Korrekturpush)
- Korrekturpush für Build 169: C2679 Compilerfehler in GSky.cpp behoben, indem inkompatible float3-Zuweisungen an AC_NightRain-Farbwerte durch XMFLOAT3 ersetzt wurden.

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
- Vegetation Push: Umwandlung in eine reine Checkbox im F11-Menü direkt vor Rain Rendering. Der Verdrängungsradius ist fest auf 1200.0f eingestellt. FixupSettings überschreibt HeroAffectsObjects nicht mehr, auch bei deaktiviertem Wind nicht.
- Screen-Space GI: Umwandlung in eine reine Checkbox ohne Stärke-Slider im F11-Menü. FixupSettings stellt ScreenSpaceGIStrength bei aktivierter Checkbox fest auf 1.0f und bei deaktivierter Checkbox auf 0.0f ein. ScreenSpaceGIStrength wurde aus Preset-Vergleichen entfernt.
- Ambient Particles & [ENGINE]/noAmbientPFX: Vollständige Anbindung von Ambient Particles an Gothics native [ENGINE]/noAmbientPFX-Option via zCOption.h Hook und SyncAmbientParticlesOption in ImGuiShim.cpp. Die Zuordnung der Partikeleffekte erfolgt 1:1 durch Gothics m_bIsAmbientPFX-Feld.
- Dynamic Clouds Sonnen-Transmission: Überarbeitung des Sonnenblocks in PS_PFX_LowClouds.hlsl mit realistischer Transmission (Sonnenkern 18 %, Halo 6 %) und sanfter Wolken-Rückseiten-Durchleuchtung (backlitCloudColor).
- Prüfung: Statische Diff-, HLSL-String-, Inhalts-, Zeilenzahl-, Byte- und SHA-256-Hash-Prüfung lokal und direkt im OneDrive-Backup-Ziel; kein vollständiger lokaler C++-Build.

## Build 155
- Partikelbeleuchtung: Trennung von Nacht-RGB-Dimmung (nightDim) und Regen-Alpha-Dimmung (rainAlpha) in PS_ParticleSimple.hlsl und PS_ParticleDistortion.hlsl. Rauch bleibt über particleLightingScale (1.0) stärker betroffen als Wasserpartikel (0.25).
- F11-Menü & Presets: Water Reflections steht genau einmal direkt nach HDR Tone Mapping und bleibt aus allen Presets ausgeschlossen. Dynamic Clouds und Ambient Particles stehen direkt unter Depth of Field und sind vollständig in Presets integriert (Low=false, sonst true). Ambient Particles deaktiviert bei false ausschließlich atmosphärischen Ground Fog.
- Vegetationsverdrängung: Der F11-Regler steuert HeroAffectsObjectsRadius (Display -> HeroAffectsObjectsRadius), welche den Such- und Verdrängungsradius um den Spieler skaliert.
- Dynamic Clouds Optimierung: Schleifeninvariante Werte vor die Raymarching-Schleife verlagert, exakt leere Dichte-Samples (density <= 0.0f) per continue übersprungen und Schleife bei voller Deckkraft (transmittance <= 0.001f) per break beendet. Exakt 8 Marching-Schritte und alle visuellen Details bleiben 100% erhalten.
- Prüfung: Statische Diff-, HLSL-String-, Inhalts-, Zeilenzahl-, Byte- und SHA-256-Hash-Prüfung lokal und direkt im OneDrive-Backup-Ziel; kein vollständiger lokaler C++-Build.

## Build 154
- **DoF Composite Shader Fixes (PS/CS_PFX_DoF_Composite.hlsl)**: `GetSkyEdgeBlurSample`-Aufruf im `IsSkyDepth`-Block ergänzt und `OutputComposite` UAV-Register (`u0`) wiederhergestellt.
- **Wasser-Reflection 39°/50°-Kurve & Sky-Reflection (PS_Water.hlsl)**: Neigungsabhängige Ausblendung auf 39°–50° zurückgestellt (`0.64278761f` / `0.77714596f`) und Low-Cloud-Komposition über `skyBase` wiederhergestellt.
- **Ocean-Regenfarben & Tint-Anpassungen (PS_Water.hlsl)**: Ocean-Nachtregen-Fallback und Volume-Grading auf kühles Blaugrau (`0.86f, 0.99f, 1.16f`) umgestellt; Ocean-Tint bei Regen weich auf 35% abgeschwächt.
- **Weltbezogene Ocean-Profile (GothicGraphicsState.h)**: Standard-Wasserfarben und Stärken für OldWorld (`0.72, 0.82, 0.84`, `0.65f`), NewWorld (`0.78, 0.90, 0.92`, `0.55f`) und AddonWorld (`0.72, 0.88, 0.95`, `0.0f`) hinterlegt.
- **GSky Savegame & OldWorld Fix (GSky.cpp)**: Statisches Makro in `LoadSkyResources()` durch dynamische `DaySkyTexture`-Prüfung (`ST_OldWorld` vs `ST_NewWorld`) und `ApplyDaySkyColorProfile()` ersetzt.
- **SSR 5-Tap Kreuzfilter (PS_Water.hlsl)**: SSR-Treffersampling durch tiefengeführten 5-Tap-Kreuzfilter zur Weichzeichnung von Reflexionskanten erweitert.

## Build 153
- **Steilwasser-SSR/Cubemap-Ausblendung (PS_Water.hlsl)**: Neigungsabhängige Ausblendung auf 33°–67° korrigiert (`smoothstep(waterfallSsrOffCos, waterfallSsrFullCos, waterGeometryUp)`).
- **Entfernung alter Wasserfall-Renderpfad (D3D11GraphicsEngine.cpp/.h)**: `FrameTransparencyMeshesWaterfall`, `waterfallTransparencyMeshes` und der Rendergraph-Pass `Draw FrameTransparencyMeshesWaterfall` vollständig entfernt; `MT_WaterfallFoam`-Geometrie läuft nun durch die normale Transparenzsortierung.
- **Rücknahme DoF-Reactive-Mask & Shader-Fixes (D3D11PFX_DepthOfField.cpp/.h, CS/PS_PFX_DoF_Composite.hlsl, D3D11PfxRenderer.cpp/.h)**: DoF-Reactive-Mask-Erweiterung vollständig zurückgenommen, UAV-Deklaration `OutputComposite` (u0) in `CS_PFX_DoF_Composite.hlsl` ergänzt und `GetSkyEdgeBlurSample`-Aufruf in `PS_PFX_DoF_Composite.hlsl` korrigiert.

## Build 151
- **Entfernung wirkungsloser Wasserfall-Marker (D3D11GraphicsEngine.cpp & PS_Water.hlsl)**: Marker-Funktionen (`IsWaterfallTexture`, `IsWaterTextureExcludedFromSSR`, `TextureNameContainsMarker`) und wirkungsloser `else if (isWaterfall > 0.5f)` Shader-Pfad entfernt. `WaterMaterialInfoConstantBuffer` auf 48 Byte angepasst. Steile Wasserflächen nutzen die Legacy-Wasserdarstellung.
- **Ocean Edge Sky Smoothing (PS_Water.hlsl)**: Sanfte Angleichung dunkler seitlicher Ocean-Ränder an weiter innen liegende Himmelsspiegelungsproben (`oceanSideSkyBlend`).
- **SSR & Cubemap Steilwasser-Ausblendung (PS_Water.hlsl)**: Ausblendung von SSR und Cubemap auf stark geneigtem Wasser harmonisiert (`cubeStrength*=steepWaterSsrFactor`).

## Build 150
- **Wasser- und SSR-Korrekturen (PS_Water.hlsl)**: Ocean edge fade vertikal geglättet, Legacy-Nachtverdunkelung angepasst, SSR auf steilem Wasser ausgeblendet, Wasserfall-Mischungen korrigiert und ssrActive nach der Neigungsreduktion berechnet.
- **DoF-Maskierung & RenderGraph Safety**: DoF-Pass maskiert SSR- und Specular-Flächen über WaterMask/SpecularMask SRVs. RenderGraph gegen ungültige Handles abgesichert (IsHandleRegistered mit GetHandleIndex).

## Build 149
- **Umstrukturierung**: Die "include"-Ordner wurden in "Include" umbenannt.
- **Hook Safety (zCWorld.h)**: Die Hook-Funktionen hooked_zCWorldDisposeVobs und hooked_LoadWorld wurden überarbeitet, um direkte Rohzugriffe auf Engine::GAPI->GetLoadedWorldInfo()->MainWorld zu vermeiden und stattdessen sichere, null-geprüfte Zugriffe über  uto* worldInfo zu verwenden.

## Build 148
- **Codebase Cleanup & Humanisierung**: Redundante Kommentare und KI-Auffälligkeiten in C++- und HLSL-Dateien bereinigt. Ordnerstruktur auf einheitliches Include umgestellt.
- **Shader Formatierung (PS_Water.hlsl)**: Wasserfall-Shader-Logik formatiert und bereinigt.
- **Diagnosetest FSR3 Contact Shadows**: Testweise Umgehung der Albedo-Alpha-Indoor-Klassifizierung unter FSR3 in PS_PFX_ScreenSpaceLightingTrace.hlsl und PS_PFX_ScreenSpaceLightingTemporal.hlsl.

## Build 147 (Dedizierte C++ Render-Pipeline für Wasserfälle)
- **C++ Engine (D3D11GraphicsEngine.cpp)**: Wasserfälle (OWODWAT, WATERFALL, WASSERFALL) rendern jetzt über eine vollständig entkoppelte und exklusive Render-Pipeline. Sie umgehen jegliche Standard-Wasser- oder Material-Shader (PS_Water / MT_Water).
- Stattdessen wird nun gezielt PS_Simple und VS_Ex verwendet, wodurch sämtliche (teils unerwünschte) Screen-Space-Reflektionen (SSR), Cubemap-Spiegelungen, Normal-Maps und Specular-Eigenschaften für animierte Wasserfälle hart und sicher auf Engine-Ebene ausgeschlossen werden.

## Build 146 (Wasserfall Animations- und Material-Fixes)
- **C++ Engine**: Wasserfälle (OWODWAT, WATERFALL, WASSERFALL) verwenden nun konsistent das MT_WaterfallFoam Material anstelle des Standardwasser-Shaders (MT_Water).
- **Animierte Texturen**: Die Engine prüft jetzt korrekt auf animierte Texturen (GetAniTexture()) bei der Wasserfall-Erkennung, sodass animierte Wasserfälle nun korrekt aus dem generischen PS_Water- und Wasser-SSR-Pfad ausgeschlossen werden.

## Build 145 (Waterfall-Override und lokale Fixes)
- **PS_Water.hlsl**: Dedizierter else if (isWaterfall > 0.5f) Block hinzugefügt, um Wasserfälle vom regulären Ocean/Legacy-Refraction- und Fresnel-Handling abzutrennen und SSR/Cubemap explizit zu überschreiben.
- **D3D11GraphicsEngine.cpp**: Weitere C++-seitige Logikanpassungen für Wasser und Fallbacks durch den Benutzer lokal integriert.

## Build 144 (Lokales Refactoring)
- **PS_Water.hlsl**: Umfassendes manuelles Refactoring und Strukturänderungen (Ocean/Legacy Code-Blöcke reorganisiert).
- **C++ Engine & Header**: Lokale Änderungen in D3D11GraphicsEngine.cpp und AtmosphericScattering.h durch den Benutzer integriert.

## Build 142 (Stabile Wolkenkanten und globale Beleuchtung wie Build 139)
- Dynamische Wolken: unsichere halbaufgelöste Tiefensamples an weit entfernten, alpha-getesteten Baumkronen werden nicht mehr als einzelner Treffer auf volle Deckung normalisiert; ein begrenzter tiefen- und raumgewichteter 5x5-Rekonstruktionsfilter stabilisiert die Wolkendeckung gegen grüne flackernde Blattpixel.
- Wolken-Tiefenregeln: Sky und Geometrie bleiben strikt getrennt, solide Geometrie erhält keinen Sky-Farbfallback und Wolken können weiterhin räumlich korrekt vor Weltgeometrie und VOBs liegen.
- Tages-/Nachtbeleuchtung: die NW_CITY_WINDOW-Sonderregel und ihre BSP-/VOB-Scans sowie GBuffer-Marker wurden vollständig entfernt; globale Tagesaufhellung, Nachtaufhellung, Mondlicht, SSS und atmosphärische Einfärbung entsprechen wieder Build 139.
- Indoor-Klassifizierung: Punktlichter sowie die eigenständige Indoor-Begrenzung von SSGI/Contact Shadows verwenden wieder die klassische Build-139-Alpha-Schwelle; andere Build-141-Fixes für Meerwasser/SSR und den Regen-Sonnenfade bleiben erhalten.
- Prüfung: Build 139 gezielt als Referenz verglichen; elf Beleuchtungs-/GBuffer-Dateien stimmen byte-identisch mit Build 139 überein; Wolkenfilter, Sky-/Geometrie-Trennung, Ocean-Präfix/-Körper, gemeinsamer SSR-Abschluss, Regen-Sonnenprofil, Aufrufer, Escape-Artefakte und `git diff --check` statisch kontrolliert. Kein vollständiger lokaler C++-/Shader-Build.

- Modifizierte D3D11GraphicsEngine.cpp: 'OWODWAT' wird von SSR ausgeschlossen.
- Modifizierte PS_Water.hlsl: Dedicated Waterfall-Override hinzugefügt.
- Modifizierte PS_Water.hlsl: Legacy-Wasser um Volumetric Murkiness und Shoreline Foam erweitert.
- Modifizierte PS_Water.hlsl: Ocean SSR Contact Fade Artifact Fix übernommen.

## Build 141 (Indoor-Beleuchtung, Meerwasser, dynamische Wolken und Regen-Sonne)
- Indoor-/Outdoor-Abgrenzung: reservierte GBuffer-Marker ersetzen die mit dunklen Outdoor-Vertexfarben kollidierenden Build-140-Werte; die Sonderregel greift nur bei tatsächlichen Indoor-Receivern, während Outdoor sowie Räume mit `NW_CITY_WINDOW*` die globale Tages-/Nachtaufhellung und Einfärbung behalten.
- Indoor-Erkennung: pauschale BSP-Leaf- und Bounding-Box-VOB-Scans wurden entfernt; im Outdoor-BSP werden nur lightmapped Worldpolys kontrolliert und VOBs nur über ihren tatsächlichen Indoor-Status markiert.
- Meerwasser: Texturen mit Präfix `NW_WATER_LAKE` erhalten wieder den blauen Ocean-Volumenkörper aus Build 139, unabhängig von der optionalen Tint-Stärke; Reflexionen bleiben im gemeinsamen, tiefen-/kanten-/hitqualitätsgesicherten Build-140-SSR-Pfad gegen NPC-/Objektartefakte.
- Dynamische Wolken: tiefenkompatible Wolkensamples können wieder vor Landschaft und Weltgeometrie liegen; Sky-/Geometrieklassen und relative Tiefen werden getrennt, damit Baumkanten keinen Sky-/Cloud-Hintergrund vermischen.
- Regen-Sonne: das vollständige Mie-/Sonnenprofil behält seine feste Größe und wird nach der Phasenberechnung nur in der Amplitude bis exakt null überblendet; beim Regenende erscheint es über dieselbe Kurve wieder, ohne einen verbleibenden Punkt.
- Prüfung: Build 139 und 140 gezielt als Referenzen verglichen; `origin/master` war Fast-Forward-kompatibel; Änderungsumfang, GBuffer-Marker, Indoor-Klassifizierung, Ocean-Präfix/-Körper und gemeinsamer SSR-Abschluss, Cloud-Tiefenklassen, Sonnenprofil-Regenfade, Shader-Klammern, Escape-Artefakte und `git diff --check` statisch kontrolliert. Kein vollständiger lokaler C++-/Shader-Build; finale Renderprüfung erfolgt im Spiel.

## Build 140 (Arbeitsbuild)
- Arbeitsbuild aus dem gepushten Build-139-Stand 82d6e67 angelegt; weitere Änderungen folgen in diesem Build.
- Indoor-Tageslicht: die bisherige künstliche Tagesaufhellung greift in Indoor-Räumen nur noch, wenn ein NW_CITY_WINDOW*-VOB/Visual im Raum oder innerhalb der 30f-Toleranz erkannt wird; Worldmesh, VOBs, MOBs und Skeletal-Receiver nutzen denselben Marker.
- SSGI/Contact Shadows: Screen-Space-GI und Kontaktschatten werden per Albedo-GBuffer-Maske nur noch für Indoor-Receiver inklusive 30f-Außentoleranz berechnet; Outdoor-Pixel laufen im Trace/Temporal frühzeitig auf 0.
- F11-Menü: die Tooltips für Screen Space GI und Contact Shadows weisen knapp darauf hin, dass beide Effekte nur indoor wirken.
- Prüfung: Register-/Aufrufer-Bindings für Screen-Space-Lighting, Indoor-Daylight-Polymarker, PowerShell-Escape-Artefakte und git diff --check statisch kontrolliert. Kein vollständiger lokaler C++-/Shader-Build.

## Build 139 (Wasserreflektionen, Regen-/Cloud-Stabilisierung und Materialdaten)
- Wasser: `NW_WATER_LAKE01` bleibt im Ocean-Wasserpfad; Wasserreflektionen und kameranahe Objekt-/NPC-Kontakte werden stabilisiert, ohne auf den alten Legacy-Wasserpfad zurückzugehen.
- Wasser/Regen/Nacht: Meerwasser wird bei Regen stärker an den grauen beziehungsweise blau-dunklen Wetter-/Nachtschleier gebunden; der Atmosphere-Unterhorizont wird tiefer gehalten, damit Sky-Farben nicht zu früh orange/schwarz durch das Wasser laufen.
- Wasserfälle: Wasserfall-Foam wird aus Distanz dauerhaft angefordert, damit Wasserfalltexturen nicht erst nah an der Kamera erscheinen.
- Regenhimmel/Clouds: der Sonnenspot wird bei Regen komplett über `AC_SunVisibility` ausgeblendet; dynamische Wolken bleiben optisch erhalten, werden nachts aber weich vom Horizont angehoben, und diskrete Regen-Cloud-Sample-Sprünge wurden entfernt.
- Rain Ground SSR: Bodenreflektionen nutzen die GBuffer-Specular-Werte als Materialfilter; bewegtes Wabern wurde entfernt und durch feines Regen-Krizzeln ersetzt.
- Materialien: `system\GD3D11\textures\materials.json` wurde als bereinigte Fallback-Datenbank eingebaut; Wasser, Foam, Alpha/Vegetation und IceDragon-Schnee erzeugen kein Rain-Ground-SSR, und Materialwerte werden im Loader gekappt.
- Normal-/Displacementmaps: echte Normalmaps werden nur genutzt, wenn das Material sie erlaubt; der alte Wet-Distortion-Normalfallback ist abgeschaltet, `displacementFactor=0` bleibt wirklich 0, und Displacementmaps werden bei 0 nicht geladen/gebunden.
- Prüfung: AGENTS-Regeln gelesen; Buildnummer aus outputs ermittelt; Materialien-JSON geparst und auf Wertebereiche, Keys, IceDragon-/Wasser-SSR-Treffer geprüft; Material-/Normal-/POM-Bindings, Wet-Ground-SSR-Bindings, Shader-Escape-Artefakte und git diff --check statisch kontrolliert. Kein vollständiger lokaler C++-/Shader-Build.

## Build 138 (LowClouds in Wasser-SSR/Godrays und Wasserlook aus Build 134)
- LowClouds: dynamische Wolken werden vor Wasser und Godrays als eigene Layer-/Depth-Ressource erzeugt und später tiefenbewusst über das Bild komponiert.
- Wasser-SSR: der Wasser-Pixelshader übernimmt den Build-134-Wasserlook und kann LowClouds in SSR-Treffern reflektieren; die bestehende WaterMaterialInfo-Logik für Ocean-Tint, Wasserfall-SSR-Sperren und Regenmasken bleibt erhalten.
- Godrays: LowClouds werden in Pixel- und Compute-Godray-Masken berücksichtigt, damit Wolken die Lichtstrahlen sichtbar formen können.
- LowCloud-Kanten: entfernte alpha-getestete Vegetation bekommt beim LowCloud-Compositing einen konservativen Farbfallback, damit an Baumkonturen nicht roher Sky durchscheint, wenn Wolken dahinter liegen.
- Konsistenz: der seltene Standalone-Godray-Fallback verwendet wieder den Depth-SRV statt des alten Normal-SRV-Parameters.
- Prüfung: AGENTS-Regeln gelesen; Buildnummer aus outputs ermittelt; Wasser-/LowCloud-/Godray-Bindings, WaterMaterialInfo-Pfade, Merge-Marker, PowerShell-Escape-Artefakte und git diff --check statisch kontrolliert. Kein vollständiger lokaler C++-/Shader-Build.
- Korrekturpush: ungültige `IsValid()`-Prüfungen auf RenderToTextureBuffer/RenderToDepthStencilBuffer durch vorhandene Texture-/View-Prüfungen ersetzt; Wasser-Shader ist wieder byte-identisch zu Build 134.
- Korrekturpush: Wasser-SSR ist wieder näher am Build-132-Fallback abgestimmt; rohe Sky-/Sonnen-/Mond-Screenhits werden nicht mehr als harte Wasserreflektion genutzt, LowClouds laufen nur bei aktiven Wasserreflektionen in den SSR-Pfad, und Nicht-`NW_WATER_LAKE01`-Wasser bleibt beim 132-artigen Wasserlook.
- Korrekturpush: Regenwasser bei Tag/Nacht wurde grauer/blauer abgestimmt, LowCloud-Baumkonturen und der LowCloud-Horizont-Fill wurden nachgezogen, und LowClouds maskieren Godrays bei tief stehender Sonne stärker.
- Korrekturpush: der Resize-Pfad selbst bleibt unverändert; Texture-Pool-Clear entfernt keine aktiven Targets mehr, um den R6025-Absturz beim Auflösungswechsel zu vermeiden.
- Korrekturpush: `D3D11PFX_GodRays.cpp` verwendet für die Atmosphere-CB-Abfragen wieder einen nicht-const `GSky`-Pointer, damit `Release_G1_AVX` nach dem Godray-LowSun-Boost kompiliert.

## Build 137
- Reset auf Build 132 als stabile Basis.
- Build 134/136-Render-, Sky-, Wetter-, FSR3- und Shader-Portierungen bewusst verworfen.
- Keine Startpfad-, Launcher-, DirectDraw-, Hooking-, Detours- oder Memory-Patching-Änderungen aus 134 übernommen.
- F11-Menü: Vegetations-/Wasser-/Regenoptionen bereinigt, Wasserreflektionen und Regendarstellung umbenannt, D3D11-Version 18.0 angezeigt und Presets auf sichtbare Optionen unterhalb der Trennlinie begrenzt.
- Vegetation: Objektinteraktion wird automatisch über Windeffekte gesteuert, Backlight bleibt dauerhaft aktiv und der Interaktionsradius entspricht 0,5 m.
- Regen/Wetter: Savegame-Laden setzt den Wetterzustand stabil zurück, RainClouds bleiben bei Regen sichtbar und Regen klingt mit Wolken-/Sonnen-Rückkehr sauber ab.
- FSR3/Contact Shadows: Contact Shadows bleiben unter FSR3 sichtbar, werden dort abgeschwächt und NPC-/Gesichtsreceiver werden deutlich entschärft beziehungsweise ausgeschlossen.
- Transparenz/Wet Ground SSR: transparente Weltgeometrie bekommt eine eigene Wet-Blocker-Maske, damit Regen-/Nachtflackern und Durchschimmern durch solide Geometrie reduziert werden, ohne funktionierende Sperrmasken zu entfernen.
- Dynamische Wolken/Wasser/Schatten: Wolken-Compositing nutzt tiefenbewusstes Upsampling gegen helle/dunkle Objektkanten, Wasser ist bei Tag/Nacht/Regen neutraler abgestimmt und NPC-nahe Schatten werden stabiler weichgefiltert.
- Text: der deutsche F11-Hinweis nutzt CP1252-kompatible Umlaute für Gothics Textausgabe.
- Prüfung: AGENTS-Regeln gelesen; Buildnummer aus outputs ermittelt; Projekt-XML, Shader-/Projektpfade, Shaderregistrierungen, F11-/INI-/Preset-Pfade, Regen-/RainCloud-Pfade, FSR3-/Contact-Shadow-Gates, Transparenz-/Wet-SSR-Bindings, Escape-Artefakte und git diff --check statisch kontrolliert. Kein vollständiger lokaler C++-/Shader-Build.

## Build 132 (Performance-Basis, F11-Aufräumen und interne Schattenfilter-Auswahl)
- Aufräumen: die nicht erfolgreichen Occlusion-Culling- und Motion-Blur-Systeme wurden vollständig aus Code, Shaderregistrierung, Projektdateien, F11-Menü und INI-Persistenz entfernt.
- Performance: Deferred-Z-Prepass ist als Standard aktiv; FSR3-Velocity-/Reactive-/Transparency-Masken werden in Deferred und Forward+ nur noch bei aktivem FSR3 als MRTs erzeugt und gebunden.
- Schattenfilter: die F11-Option `Shadow Filter` wurde entfernt; PCSS bleibt intern Standard, Simple PCF wird nur als Feature-Level-10 beziehungsweise Shadow-Atlas-Fallback verwendet und alte INI-Werte werden ignoriert.
- Settings: tote `DrawThreaded`-Einstellung wurde entfernt; `SortRenderQueue` bleibt unverändert.
- Grenzen: Render Scale, Aspect-/Viewport-, Kamera- und echte Auflösungswechsel-Pfade bleiben unverändert.
- Prüfung: AGENTS-Regeln gelesen; Buildnummer aus outputs ermittelt; Kirides-Nightly/17.9.7 für Performance-Pfade verglichen; Occlusion-/MotionBlur-/ShadowFilter-Reste, Projekt-XML, Render-Scale-/Aspect-Grenze, Escape-Artefakte und git diff --check statisch kontrolliert. Kein vollständiger lokaler C++-/Shader-Build.

## Build 131 (HZB-Occlusion-Snapshot, konsistente VOB-Schatten und Motion-Blur-Sampling)
- Occlusion: die HZB-Auswertung nutzt nun einen stabilen Readback-Snapshot vom vorher abgeschlossenen Frame; neue HZB-Daten werden erst nach dem World-Depth-Pass für den nächsten Frame erfasst, damit Shadow- und Main-Collect nicht gegen wechselnde Tiefendaten laufen.
- Occlusion: die Bounding-Box-Projektion nutzt die zur Renderer-CPU-Projektion passende Projection-mal-View-Reihenfolge; wenn kein frischer Readback verfügbar ist, wird konservativ nicht gecullt.
- Schatten: kleine VOBs/Mobs, die durch dieselbe HZB-Entscheidung ausgeblendet werden, werden auch aus Sun- und Pointlight-Shadow-Collects genommen; grosse VOBs bleiben weiterhin von Occlusion ausgeschlossen.
- Motion Blur: das Sampling ist symmetrisch um den aktuellen Pixel statt einseitig in Richtung vorheriger Pose, die horizontale stabile Zone ist breiter und der Übergang zu den Rändern weicher.
- Aufräumen: die nicht erfolgreichen Main-View-Frame-Stamp-/Shadow-Kopplungen aus dem vorherigen Versuch sind entfernt.
- Grenzen: Render Scale, Aspect-/Viewport-, Kamera- und echte Auflösungswechsel-Pfade sowie DoF bleiben unverändert.
- Prüfung: AGENTS-Regeln gelesen; Buildnummer aus outputs ermittelt; origin/master mit PortableGit/OpenSSL abgeglichen; Occlusion-/Shadow-Pfade, Render-Scale-/Aspect-Grenze, Shader-Escape-Artefakte und git diff --check statisch kontrolliert. Kein vollständiger lokaler C++-/Shader-Build.

## Build 130 (HZB-Occlusion-Fix, konsistente VOB-Schatten und Motion-Blur-Maske)
- Occlusion: die HZB-Bounding-Box-Projektion nutzt dieselbe Projection-mal-View-Reihenfolge wie der restliche Renderer, damit sichtbare VOBs nicht durch falsche Clip-Projektion verschwinden.
- Schatten: VOBs, die durch die Main-View-Occlusion ausgeblendet werden, werden im Shadow-Collect ebenfalls übersprungen; grosse nicht-occludable VOBs bleiben davon unberührt.
- Motion Blur: die stabile Bildzone ist horizontal breiter und der Übergang zu den geblurten Rändern weicher; kleine Restgeschwindigkeiten werden gedämpft, um Nachziehen nach Kamerastopps abzuschwächen.
- Grenzen: Render Scale, Aspect-/Viewport-, Kamera- und echte Auflösungswechsel-Pfade bleiben unverändert.
- Prüfung: AGENTS-Regeln gelesen; Buildnummer aus outputs ermittelt; origin/master mit PortableGit/OpenSSL abgeglichen; Occlusion-/Shadow-Pfade, Render-Scale-/Aspect-Grenze, Shader-Escape-Artefakte und git diff --check statisch kontrolliert. Kein vollständiger lokaler C++-/Shader-Build.

## Build 129 (Konservative HZB-Occlusion, DoF-Rückbau und Motion-Blur-Randlook)
- Occlusion: die HZB-Reduktion nutzt für reversed-Z nun konservative Mindest-Tiefen statt nächster Max-Tiefen, damit einzelne nahe Pixel keine sichtbaren VOBs/Vegetation über ganze Kacheln wegcullen; das versteckte Tiny-Screen-Culling unter derselben Option ist entfernt.
- DoF: die Depth-of-Field-Shader sind wieder auf den Build-127-Look zurückgesetzt, inklusive vorheriger CoC-/Sky-Edge-Logik.
- Motion Blur: Randmaske, Heldenbereich, Samplingrichtung und Konstanten folgen wieder Build 127; die aktuelle depth-aware Kantenabsicherung gegen Silhouettenartefakte bleibt erhalten.
- Grenzen: Render Scale, Aspect-/Viewport-, Kamera- und echte Auflösungswechsel-Pfade bleiben unverändert.
- Prüfung: AGENTS-Regeln gelesen; Buildnummer aus outputs ermittelt; origin/master mit PortableGit/OpenSSL abgeglichen; DoF-Shader gegen Build 127 verglichen; HZB-/Tiny-Cull-, Motion-Blur- und Shader-Escape-Artefakte sowie git diff --check statisch kontrolliert. Kein vollständiger lokaler C++-/Shader-Build.

## Build 128 (DoF-Kanten, Motion-Blur-Stabilität, HZB-Occlusion, Schatten-Updates und Wasser-SSR-Rückbau)
- DoF: Vorder- und Hintergrundunschärfe nutzen eine kleine CoC-Erweiterung, damit geblurte Objektkanten weniger hart ausgeschnitten wirken, während die bestehende Sky-Maske erhalten bleibt.
- Motion Blur: Stärke und maximale Länge sind reduziert; Velocity-Dilation und Tiefenkanten-Gewichtung sollen Helden-/Silhouettenartefakte und weisse Übergangsbereiche abschwächen.
- Occlusion: das alte BSP-Predicate-/Query-Culling wurde entfernt; stattdessen baut der Renderer nach dem Welt-Depth-Pass eine kleine Depth-Hierarchie und cullt nur kleine VOBs/Mobs konservativ pro Bounding-Box. Wenn der asynchrone Readback nicht frisch verfügbar ist, bleibt alles sichtbar.
- Schatten: entfernte Outdoor-Schatten-Cascades werden bei relevanter Kamera-Bewegung oder -Drehung sofort aktualisiert.
- Wasser-SSR: der nicht erfolgreiche Vordergrund-Occluder-/Thin-Fill-Fix wurde wieder auf den Build-124-Wasser-SSR-Stand zurückgesetzt, ohne Wet-Ground-SSR anzufassen.
- Grenzen: Render Scale, Aspect-/Viewport- und echte Auflösungswechsel-Pfade bleiben unverändert.
- Prüfung: AGENTS-Regeln gelesen; Buildnummer aus outputs ermittelt; origin/master mit PortableGit/OpenSSL abgeglichen; PS_Water.hlsl gegen Build 124 verglichen; HZB-/Occlusion-Projektpfade, alte Query-Reste, SSR-Fallback-Marker, Render-Scale-/Aspect-Grenze, Shader-Escape-Artefakte und git diff --check statisch kontrolliert. Kein vollständiger lokaler C++-/Shader-Build.

## Build 127 (Motion-Blur-Heldenschutz und Wasser-SSR Thin-Occluder-Fill)
- Motion Blur: die stabile Zone schützt nun zusätzlich den unteren mittigen Heldenbereich, statt nur exakt um die Bildmitte zu liegen.
- Wasser-SSR: dünne Vordergrund-Occluder wie Seile oder Pfosten bekommen einen kleinen bilateralen Nachbar-Fill aus direkten SSR-Trefferumgebungen; breite Occluder bleiben beim bisherigen Cubemap-Suppression-Fallback.
- Grenzen: Nebel/Fog, Render Scale, Aspect-/Viewport- und Auflösungswechsel-Pfade bleiben unverändert.
- Prüfung: AGENTS-Regeln gelesen; Buildnummer aus outputs ermittelt; origin/master mit PortableGit/OpenSSL abgeglichen; Fog-/Render-Scale-/Aspect-Grenze, Shader-Escape-Artefakte und git diff --check statisch kontrolliert. Kein vollständiger lokaler C++-/Shader-Build.

## Build 126 (Dynamic-Cloud-Regen, Motion Blur, Wasser-SSR und Vegetationsradius)
- Dynamic Clouds: Tiefwolken blenden bei Regen gegen die Raincloud-Textur aus, werden im Composite auch vom Nachtnebel mitgenommen und der Low-Cloud-Pass wird bei starkem Regen übersprungen.
- Regen-Schleier: der globale Regenschleier bleibt ohne Dither, bekommt aber eine räumlich weichere Wirkung über die bestehende Fog-Maske.
- FSR3/Contact Shadows: Contact Shadows nutzen bei aktivem FSR3 wieder die reduzierte Stärke `0.35`, während andere Modi beim Standard `0.50` bleiben.
- Motion Blur: die Bildmitte bleibt stabiler und Bewegungsunschärfe nimmt zu den Rändern zu; Tiefenkanten werden strenger gewichtet, um Helden-/Silhouettenartefakte zu reduzieren.
- Wasser-SSR: Vordergrundobjekte lassen den Cubemap-Fallback an den betroffenen Wasserstellen weniger stark durchbrechen, ohne Wet-Ground-SSR anzufassen.
- Vegetation: der Interaktionsradius für Hero/NPC-Vegetation ist kleiner, während Stärke und gaussscher Übergang unverändert bleiben.
- Prüfung: AGENTS-Regeln gelesen; Buildnummer aus outputs ermittelt; origin/master mit PortableGit/OpenSSL abgeglichen; Render-Scale-/Aspect-Grenze, Shaderpfade, Dither-/Escape-Artefakte und git diff --check statisch kontrolliert. Kein vollständiger lokaler C++-/Shader-Build.

## Build 125 (Wasser-SSR, Regenwolken, FSR3-Masken und 4:3-Seitenverhältnis)
- Wasser-SSR: Vordergrundobjekte vor Wasserreflexionen reduzieren nur die betroffenen Screen-Space-Treffer weich, damit fehlender Hintergrund kaschiert wird, ohne Wet-Ground-SSR anzufassen.
- Regen-Schleier: der globale Regen-/Sky-Schleier ist ohne Dither, beginnt etwas näher und läuft weicher/länger in die maximale Wirkung; tagsüber bleibt die Regenwolken-Textur sichtbar abgeschwächt erhalten.
- Dynamische Wolken: Low Clouds werden im Composite vom gleichen Regen-/Nacht-Schleier mitgenommen und laufen bei Regen beziehungsweise starker Nacht mit weniger Raymarch-Schritten, während klare Tageswolken ihre volle Schrittzahl behalten.
- FSR3: Alpha-getestete Weltflächen und Contact Shadows schreiben gezielte Transparency-/Composition-Masken für die Rekonstruktion; die Contact-Shadow-Stärke ist wieder der normale Standardwert.
- Seitenverhältnis: der fehleranfällige World-Projection-Aspect-Hack ist entfernt; echte Auflösungswechsel aktualisieren die Gothic-Kamera wieder direkt, Render Scale bleibt davon getrennt.
- Prüfung: AGENTS-Regeln gelesen; Buildnummer aus outputs ermittelt; origin/master mit PortableGit/OpenSSL abgeglichen; Projekt-XML, Shader-/Projektpfade, FSR3-Maskenfluss, Render-Scale-Grenze, Konflikt-/Escape-Artefakte und git diff --check statisch kontrolliert. Kein vollständiger lokaler C++-/Shader-Build.

## Build 124 (Nachtregen-Schleier, Auflösungswechsel, NPC-Vegetation, Motion Blur und TAA-Rückbau)
- Nachtregen/Tagregen: der Regen-Schleier liegt als sanfter Overlay über Welt und Himmel; nachts stärker, tagsüber abgeschwächt, damit entfernte Geometrie und Sky wieder zusammenhängender wirken.
- Seitenverhältnis: echte Auflösungswechsel synchronisieren Gothic-Viewport und Kamera nach World-Load wieder mit der Backbuffer-Auflösung, ohne den Render-Scale-Pfad anzufassen.
- Vegetation: der Hero-Affects-Vegetation-Effekt ist wieder näher am Kirides-Nightly-Verhalten, wirkt aber weiter auch für weitere NPCs.
- Motion Blur: ein optionaler F11-Schalter aktiviert Bewegungsunschärfe als persönliche Option; Grafik-Presets setzen diese Option nicht.
- TAA/FSR3: TAA wurde aus UI, Shadern und PostFX-Code entfernt; FSR3 nutzt einen eigenen Temporal-State für Jitter/Velocity und bleibt als `AA_FSR3 = 2` der einzige FSR3-Anti-Aliasing-Modus.
- Prüfung: AGENTS-Regeln gelesen; Buildnummer aus outputs ermittelt; TAA-/AA-FSR-Reste, Projekt-XML, Shader-/Projektpfade, F11-/INI-Pfade, Konflikt-/Escape-Artefakte und git diff --check statisch kontrolliert. Kein vollständiger lokaler C++-/Shader-Build.

## Build 123 (Render Scale, Detail-Menü-Rückbau und Sky-Farbprofile)
- Render Scale: der Skalierungspfad wurde wieder auf das Build-117-Verhalten ausgerichtet, damit nicht-100-Prozent-Render-Scale keine unterschiedlichen Bildbestandteile fehlerhaft skaliert.
- F11/INI: das Detail-Untermenü wurde entfernt; versteckte Detail-/Entwicklerwerte werden nicht mehr aus normalen User-INI-Einträgen gelesen und der sichtbare F11-Hauptmenü-Umfang bleibt massgeblich.
- Nachtregen und Wolken: Nachtregen ist näher am Build-061-Eindruck, Cloud Day/Rain/Night Defaults wurden festgezogen und die Tagwolkenfarbe folgt dem aktiven Sky-Profil.
- Sky/Fog: `Cloud Day Color` und `FogColorMod` hängen am aktiven Tag-Sky-Profil; OldWorld/World behalten optisch den G1-Sky-Look, sonstige Welten den SkyDay.dds-Look. OldWorld/World nutzen nachts `NightFogBrightness 0.35`, Standard bleibt `0.70`.
- Prüfung: AGENTS-Regeln gelesen; Buildnummer aus outputs ermittelt; Render-Scale-, F11-/INI-, Sky-/Fog- und Shaderpfade statisch kontrolliert; Konflikt-/Escape-Artefakte und git diff --check geprüft. Kein vollständiger lokaler C++-/Shader-Build.

## Build 121 (Detail-Menü, Render-Scale-Fix, Wolken/Wasser-Tuning und Regenpartikel)
- F11: das Standardmenü bleibt erhalten und bekommt nur einen Detail-Button; darin liegen Feinregler für Nachtregen, Nacht-/Nebelhelligkeit, dynamische Wolken, Vegetationseinfluss und Meerwasserfarbe.
- Dynamische Wolken: Dichte, Größe, Höhe, Reichweite, Geschwindigkeit, Sonnenlicht sowie Tag-/Regen-/Nachtfarbe werden gespeichert und direkt im Low-Cloud-Shader ausgewertet.
- Meerwasser: der Wasserpass kann Meerwasser separat färben, während Fluss-/Bach-/Wasserfall-Marker ausgeschlossen bleiben; die Standardstärke ist neutral.
- Seitenverhältnis/Render Scale: die Gothic-Kamera wird nur mit der echten Backbuffer-Auflösung synchronisiert; Render Scale/FSR bleibt intern und greift nicht mehr in die Spielprojektion ein.
- Nachtregen und Partikel: Nachtregen nutzt die dunklere Mittelbereichsfarbe; nicht-emissive Partikel werden bei Regen tagsüber genauso stark abgedunkelt wie nachts, wobei Rauch stärker und Wasserpartikel weiterhin schwächer reagieren.
- Prüfung: AGENTS-Regeln gelesen; Buildnummer aus outputs ermittelt; Build 117 als Render-Scale-Referenz verglichen; Settings-/INI-Pfade, Wasser-/Wolken-/Partikel-Shader, CBuffer-Layout, Konflikt-/Escape-Artefakte und git diff --check statisch kontrolliert. Kein vollständiger lokaler C++-/Shader-Build.

## Build 120 (Load-stabile Projektion, dichtere Wolken und Nachtregen-Mitteldistanz)
- Seitenverhältnis/Load: die Session-Kamera schreibt nach echter Auflösungsänderung und nach `zCCamera::Activate` wieder die logische Spielauflösung in den Gothic-Viewport; Render Scale bleibt dabei strikt von der Gothic-Kamera-Projektion getrennt.
- Render Scale/FSR: reine Render-Scale-Änderungen bauen nur die DX11-Renderziele neu und lösen keine zusätzliche Gothic-Kamera-Projektionsaktualisierung mehr aus.
- Tiefwolken: Wolkenfelder sind dichter, tagsüber neutraler/weisslicher, bei Regen grauer und nachts etwas dunkler; die langsame world-space Bewegung bleibt erhalten.
- Nachtregen: die mittlere entfernte Weltgeometrie wird bei Regen nachts stärker in dunkles kaltes Blaugrau gezogen, während der sehr ferne Hintergrund nicht weiter abgedunkelt wird.
- Prüfung: AGENTS-Regeln gelesen; Buildnummer aus outputs ermittelt; Kamera-/Viewport-Hooks, Render-Scale-Pfad, Cloud-/Nachtregen-Shader, Konflikt-/Escape-Artefakte und git diff --check statisch kontrolliert. Kein vollständiger lokaler C++-/Shader-Build.

## Build 119 (Dynamische Wolken, Nachtregen und robuste Auflösungswechsel)
- F11: Dynamische Wolken sind ein eigener Schalter mit INI-Persistenz; Himmelseffekte/Sky Effects steuert wieder nur Regen und Regeneffekte.
- Tiefwolken: Wolken laufen unabhängig vom Regen, sind nachts dunkler, bewegen sich langsamer sichtbar in Weltkoordinaten und verzichten auf die teure globale Wolkenschatten-Abdunklung am Boden.
- Nachtregen: sehr entfernte Weltgeometrie wird bei Regen nachts weiter in ein dunkleres kaltes Blaugrau gedrückt, ohne in Schwarzgrau zu kippen.
- Seitenverhältnis/Render Scale: Gothic-Kamera und zVidResFullscreenX/Y nutzen wieder die echte Spielauflösung statt der intern skalierten Renderauflösung; nach Resize wird die Kamera-Projektion erneuert, damit 16:9/4:3-Wechsel und Savegame-Loads nicht zwischen schmal/breit kippen.
- Prüfung: AGENTS-Regeln gelesen; Buildnummer aus outputs ermittelt; Settings-/INI-Pfade, Low-Cloud-Gates, Nachtregen-Shader, Viewport-/Optionspfad, Konflikt-/Escape-Artefakte und git diff --check statisch kontrolliert. Kein vollständiger lokaler C++-/Shader-Build.

## Build 118 (Tiefwolken, Nachtregen und stabile 4:3-Projektion)
- Tiefwolken: die Wolkenfelder bekommen stärker lokale Höhenformen, weichere Oberkanten und dichtere Bänke in mittlerer bis weiter Entfernung, damit weniger flache Wolkendecken und weniger harte Feldkanten entstehen.
- Nachtregen: sehr weit entfernte Weltgeometrie wird bei Regen nachts nochmals dunkler und kühler blaugrau begrenzt, ohne in Schwarzgrau zu kippen.
- Seitenverhältnis: die fehleranfällige Desktop-/Output-Aspekt-Heuristik für die Weltprojektion wurde entfernt; Gothic aktualisiert die Kamera-Projection vor dem DX11-World-Stage, damit 16:9/4:3-Wechsel und Savegame-Loads nicht mehr zwischen schmaler und breiter Darstellung kippen.
- Prüfung: AGENTS-Regeln gelesen; Buildnummer aus outputs ermittelt; Kirides 17.9.7 als Projektions-/Viewport-Referenz verglichen; Shaderpfade, Projektionspfad, Konflikt-/Escape-Artefakte und git diff --check statisch kontrolliert. Kein vollständiger lokaler C++-/Shader-Build.

## Build 117 (Nachtregen, Tiefwolken und Windowed-Projektion)
- Tiefwolken: Wolkenfelder sind kamerastabiler, nutzen eine feste Welt-Höhenschicht, feinere Raymarch-Schritte und können Sonne beziehungsweise Mond sichtbar verdecken.
- Wolkenschatten: direkte Sonnen-/Mondlichtwirkung wird bei Wolkenüberdeckung weicher und stärker gedimmt, einschliesslich sichtbarer Lichtscheiben im Himmel.
- Nachtregen: entfernte Weltgeometrie fällt bei Regen nachts in ein dunkleres kaltes Blaugrau mit niedrigerem Luma-Deckel, ohne den Himmel-/Regenwolken-Haze mitzudrehen.
- Windowed-4:3: die Weltprojektionskompensation wird gegen bereits passende Projektionen abgesichert, damit Laden/Resolution-Wechsel keine zu breite Figurendarstellung erzeugt.
- Prüfung: AGENTS-Regeln gelesen; Buildnummer aus outputs ermittelt; Shaderpfade für Heightfog/Composition/Low-Clouds, Projektionspfad, Konflikt-/Escape-Artefakte und git diff --check statisch kontrolliert. Kein vollständiger lokaler C++-/Shader-Build.

## Build 116 (Low-Cloud-Pass, Shader-Reloads und Windowed-Projektion)
- Tiefwolken: die world-space Tiefwolken laufen nun über einen eigenen PostFX-Pass/Shader, statt in Composition/Heightfog mitzustecken; das reduziert die teuren Shader-Recompiles beim Umschalten von Contact Shadows und Screen Space GI.
- Shader-Kategorien: Heightfog und Low Clouds sind als `SkyEffects` kategorisiert, während Contact Shadows/SSGI weiter nur die `Other`-Composition-Permutation betreffen.
- Windowed-4:3: die Weltprojektionskompensation greift nun für jeden echten Windowed-Modus mit `StretchWindow=false`, nicht nur für den Startup-Windowed-Pfad; HUD/F11 bleiben weiterhin an die Fensterfläche gekoppelt.
- Prüfung: AGENTS-Regeln gelesen; Buildnummer aus outputs ermittelt; Projekt-XML, Shaderregister, Low-Cloud-Aufrufer, Windowed-Aspektpfad, Konflikt-/Escape-Artefakte und git diff --check statisch kontrolliert. Kein vollständiger lokaler C++-/Shader-Build.

## Build 115 (Tiefwolken-Felder, Nachtregen und Windowed-4:3-Projektion)
- Tiefwolken: die vorherige durchgehende Nebelwirkung wurde zu eigenständigen, weltkoordinatenbasierten Wolkenfeldern mit sichtbaren Lücken, höhenversetzten Bänken, einfacher Selbstabschattung und sonnenzugewandter Oberseitenaufhellung umgebaut.
- Wolkenschatten: die Tiefwolken dunkeln entfernte Weltgeometrie weich und breit ab, ohne harte Shadowmap-Kanten und weiterhin unabhängig vom Regen.
- Nachtregen: der Regennebel bleibt beim Tag näher am Build-112-Verhalten; nachts wird die entfernte Weltgeometrie dunkler grau statt hellgrau, und der 360-Grad-Haze wirkt weich auch auf den Himmel.
- Windowed-4:3: die horizontale Weltprojektionskompensation gilt nun auch im Windowed-Modus, damit 800x600-Ingame nicht schmal zusammengedrückt wirkt; HUD/F11 bleiben an die Fensterfläche gekoppelt.
- Prüfung: AGENTS-Regeln gelesen; Buildnummer aus outputs ermittelt; Projektionspfad, Shaderaufrufe, Konflikt-/Escape-Artefakte und git diff --check statisch kontrolliert. Kein vollständiger lokaler C++-/Shader-Build.

## Build 114 (G1-AVX2 Buildfix für 4:3-Weltprojektion)
- Buildfix: die 4:3-Weltprojektionskompensation nutzt nun `rendererState.RendererSettings.StretchWindow` statt einer nicht vorhandenen freien `StretchWindow`-Variable, damit Release_G1_AVX2 wieder kompiliert.
- Prüfung: angehängten GitHub-Buildlog ausgewertet; betroffene D3D11GraphicsEngine-Stelle und vorhandene StretchWindow-Aufrufer kontrolliert; git diff --check statisch kontrolliert. Kein vollständiger lokaler C++-/Shader-Build.

## Build 113 (Himmelseffekte, volumetrische Tiefwolken und 4:3-Weltprojektion)
- Himmelseffekte: der bisherige F11-Schalter für Regen heisst nun Himmelseffekte/Sky Effects und steuert Regen, Regeneffekte und die neuen tiefen atmosphärischen Wolkenschichten gemeinsam.
- Volumetrische Tiefwolken: world-space Raymarching für dunkle Tal-/Bergnebelkörper mit Noise-Dichte, Distanzbegrenzung, Regen-/Nachtgewichtung, einfacher Selbstabschattung und Depth-Begrenzung gegen sichtbare Weltgeometrie integriert.
- Nachtregen: entfernte Weltgeometrie wird bei Regen in der Nacht deutlich dunkler und der 360-Grad-Regennebel dichter, damit Silhouetten nicht hellgrau aus dem Nebel leuchten.
- 4:3-Darstellung: World-Rendering bekommt bei gestrecktem Fenster eine horizontale Projektionskompensation, während HUD/F11 wieder die originale UI-Projektion nutzt.
- Prüfung: AGENTS-Regeln gelesen; Buildnummer aus outputs ermittelt; Build 111 gegen 113 für die Nachtregen-Änderung verglichen; Shaderaufrufe, Constant-Buffer-Layout, F11-Abhängigkeit, Konflikt-/Escape-Artefakte und git diff --check statisch kontrolliert. Kein vollständiger lokaler C++-/Shader-Build.

## Build 112
- Seitenverhältnis: Logische Spielauflösung und physische Borderless-Swapchain sind getrennt. Welt, HUD, Gothic-Menü und F11 werden gemeinsam in der gewählten Auflösung gerendert und erst zur Ausgabe proportional mit schwarzen Balken eingepasst.
- Nachtregen: Die zusätzlichen flachen Regen- und Tiefennebelschichten wurden entfernt. Der 360-Grad-Entfernungsnebel folgt wieder dem bewährten Verlauf, ist nachts dichter und lässt die Wolkendecke nur noch leicht durchscheinen.
- Regenübergang: In Gothic 2 hält der echte Wettertyp den Atmosphärenzustand während aktiven Regens stabil. Nach bestätigtem Wetterende bleibt der Ausklang strikt monoton und ignoriert verspätete RainFX-Pulse.
- NPC-Schatten: PCSS bleibt für die Welt aktiv; markierte animierte NPC-/Skeletal-Empfänger verwenden automatisch den stabilen PCF-Abtastpfad.
- Objektsichtweite und VisualFX: Die F11-Skala verwendet zehn gleichmäßige 2500er-Schritte von 2500 bis 25000. Niedrig nutzt Stufe 2, Mittel und Default Stufe 4, Hoch Stufe 6 und Extrem Stufe 8. VisualFX folgt bis Stufe 4 der Objektsichtweite und bleibt darüber bei 10000.
- Prüfung: Statische Diff-, Aufrufer-, Shaderstruktur-, Persistenz- und Seitenverhältnisprüfung; kein vollständiger lokaler C++-Build.

## Build 111 (Korrekturpush)
- Startstabilität: Die absturzverdächtige MENU.DAT-Spracherkennung über Gothics VDFS wurde entfernt; die Renderer-Sprache wird nun im F11-Menü manuell zwischen Englisch und Deutsch gewählt und global persistiert.
- F11-Speicherung: Die ungewöhnliche STRG-Weltspeicherung samt Welt-Schreibpfad wurde entfernt; sichtbare Renderer-Einstellungen werden ausschließlich global gespeichert.
- Mod-Weltformat: Weltdateien dürfen nur noch Fog, Atmosphere und Rain setzen; der alte Atmoshpere-Schreibfehler und die entsprechende interne Benennung wurden ohne Legacy-Fallback korrigiert.
- Dokumentation: README auf Installation, aktuelle Buildvoraussetzungen, Mod-Weltformat, Abhängigkeiten, Lizenz und die gleichwertige Autorenliste reduziert.
- Prüfung: statische Diff-, Aufrufer-, INI-, Symbol- und Projektkonsistenzprüfung; kein vollständiger lokaler C++-Build.
- CI-Korrektur: Die lokalisierten std::array-Tabellen im F11-Menü verwenden nun die von MSVC benötigte doppelte Aggregatklammerung.
- Auflösungskorrektur: F11-Auflösungen werden wieder wirklich übernommen; Menu und HUD füllen Borderless vollständig aus, während nur die 3D-Projektion ein abweichendes Fensterseitenverhältnis ausgleicht. Der Sprachwert bleibt am Ende der Settings-Struktur, damit bestehende Feld-Offsets erhalten bleiben.
- Schattenkorrektur: EVSM4-Momente bei erhaltener neuer Cascade-Logik wiederhergestellt; CSM-Empfängerbias für alle Flächen stabilisiert und Pointlight-Shadowmaps mit Reichweitenhysterese sowie festem FSR-3-tauglichem Filter beruhigt.
- F11: Überlange deutsche Beschriftungen wurden gekürzt und nur die Sprachauswahl verbreitert. Der Blue-Noise-Fog bleibt unverändert.
- Ground Fog und VisualFX: Die originale Ground-Fog-Partikeldeckkraft bleibt erhalten; für alle Partikeleffekte gilt ohne Boundingbox-Sonderfall ausschließlich VisualFXDrawRadius als Distanzgrenze.
- Pointlight-Schatten: Die Shadowmap bleibt bis zur VisualFX-gesteuerten Lichtausblendung zugeteilt; statt der harten 9x-/10x-LightRange-Grenze reduziert ein stabiler Distanzfade in Legacy und Tiled die Schattenstärke im letzten Viertel der realen Sichtweite.
- Schattenfilter: EVSM wurde vollständig aus Auswahl, Persistenz, Shaderregistrierung, Ressourcenverwaltung und HLSL entfernt; alte EVSM-Konfigurationswerte fallen auf Simple PCF zurück.
- Schattenstreifen: Die seit Build 109 ergänzten, wirkungslosen globalen Geometrienormalen- und Mindestbias-Patches wurden gezielt auf das Build-109-Verhalten zurückgenommen.
- F11-Sprache: Beschriftung und Auswahlfeld verwenden exakt dieselben Breiten wie Grafikprofil.
- Regennebel: Zusätzlich zur Geometrieüberblendung wirkt bei Regen wieder ein globaler Nahschleier, nachts stärker als tagsüber und für Himmelspixel reduziert, damit die Wolkendecke sichtbar bleibt.
- Interaktive Vegetation: Horizontaler Vollbereich auf 0-20 und der weiche Übergang auf 20-25 Welt-Einheiten begrenzt; die maximale Verschiebung bleibt bei 38.
- Nachtnebel: Der horizontale Entfernungsnebelschleier ist nachts 20 Prozent dunkler; vertikaler Ground Fog und Fernwelt-Dunkelung bleiben unverändert.

- Regen bei Nacht: Der globale Regenschleier wird um eine entfernungsabhängige Geometrieschicht ergänzt; Himmel und Wolkendecke bleiben schwacher betroffen. Kompositions- und Height-Fog-Fallback verwenden dieselbe Logik.
- Regennebel-Übergang: Die Geometrieverdichtung erreicht ihre maximale Deckkraft erst nach mindestens 3500 Welteinheiten beziehungsweise bei 90 Prozent der Fog-Reichweite; eine quintische S-Kurve verhindert einen sichtbaren Beginn oder harten Fernübergang.
- Seitenverhältnis: Bei gestrecktem Borderless-Betrieb wird nur das fertige Weltbild vor HUD und Menü mit schwarzen Balken proportional eingepasst; 800x600 auf 1920x1080 ergibt 600x600 plus je 100 logische Pixel Seitenrand und damit physisch korrektes 4:3.
- Pointlight-Schatten: Festes 12-Tap-PCF sowie renderauflösungsabhängige Mindestweichheit und ein breiterer Distanzübergang beruhigen FSR-3-/TAA-Flackern; Shadowmaps und Licht bleiben weiterhin bis zur VisualFX-Reichweite erhalten.
- Schattenfilter-Standard: PCSS ist für neue beziehungsweise auf Standardwerte zurückgesetzte Konfigurationen vorausgewählt; vorhandene explizite Benutzerauswahlen bleiben erhalten, Feature Level 10 fällt weiterhin auf Simple PCF zurück.
- World-Shadow-Bewegung: Die geglättete Sonnen-/Mondrichtung wird nicht mehr auf harte 1/500-Schritte quantisiert und die CSM-Kamera nicht mehr in 64-/160-Einheiten versetzt. Die vorhandene globale Shadow-Texel-Ausrichtung stabilisiert weiterhin gegen Subpixel-Flimmern; Cascade-Renderintervalle und GPU-Last bleiben unverändert.
- Occlusion Culling: Für neue Konfigurationen standardmäßig aktiv; die bewussten Profilwerte bleiben erhalten, also aktiv bei Niedrig/Mittel und inaktiv bei Hoch/Extrem. Individuell gespeicherte Benutzerwerte bleiben beim normalen Laden ebenfalls erhalten.
- Rauch und Fog: Erkannte Smoke-, Rauch-, Steam-, Dampf-, Fog-, Nebel-, Dunst- und Ground-Fog-Partikel werden gamma-korrekt auf exakt 75 Prozent ihrer bisherigen finalen Deckkraft reduziert; Feuer- und Wasserpartikel bleiben unverändert.
- FSR-3-Himmel: Nur im niedrigen FSR-3-Skalierungsbereich unter 67 Prozent wird Fog-Blue-Noise nicht mehr vor der Rekonstruktion eingemischt. Dort erfolgt stattdessen ein schwaches, dunkelheitsgewichtetes Blue-Noise-Dithering im Ausgabeformat gegen diagonale Rekonstruktionsstreifen; höhere Qualitätsstufen bleiben unverändert.

- Regenferne-Korrektur: Nur weit entfernte Geometrie wird bei Nachtregen entlang des bestehenden weichen Distanzverlaufs bis auf 65 Prozent der bisherigen Nebelfarbe abgedunkelt; Himmel und Wolkendecke bleiben unverändert.
- Dithering-Korrektur: Das doppelte Fog-/Ausgabedithering und der auffällige einzelne DDS-Farbkanal sind entfernt. Ein einziges pixelstabiles, kachelfreies Hash-Dither wirkt nach Gamma in allen Darstellungsmodi mit maximal einer halben 8-Bit-Stufe.
- Seitenverhältnis-Korrektur: Nicht mehr nur die Welt, sondern der vollständige Frame samt Gothic-HUD, Hauptmenü und F11 wird in Borderless proportional eingepasst. 4:3 erzeugt auf 16:9 seitliche Balken, Ultrawide auf 16:9 obere und untere Balken; Fenstermodus und native Seitenverhältnisse bleiben ohne Balken.
- World-Shadow-Stabilisierung: Die kontinuierliche Lichtrichtung wird pro Kaskade erst nach etwa einer projizierten Texelbreite zusammen mit Matrix und Shadowmap übernommen. Bestehende Kaskadenintervalle bleiben erhalten, das Light-Space-Raster rundet symmetrisch und es entstehen keine zusätzlichen regulären Shadowmap-Renderings.
- Rauch und Fog: Die vorherige 75-Prozent-Korrektur wird ersetzt; erkannte Rauch-, Steam-, Dampf-, Fog-, Nebel-, Dunst- und Ground-Fog-Partikel enden nun gamma-korrekt bei exakt 50 Prozent ihrer bisherigen finalen Deckkraft. Feuer- und Wasserpartikel bleiben unverändert.

## Build 110 (Korrekturpush)
- CI-Fix: Deutsche UTF-8-UI-Texte bleiben als `u8`-Literals erhalten, werden aber für ImGui/Gothic gezielt als `const char*` übergeben, damit Release_G1_12f unter C++20 nicht an `char8_t` scheitert.
- Inhaltlich keine zusätzlichen Renderer-Änderungen gegenüber Build 110.
- Prüfung: CI-Fehlerstellen in `GothicAPI.cpp` und `ImGuiShim.cpp` statisch gegen die gemeldeten C2664/C2446-Fehler kontrolliert; kein vollständiger lokaler C++-Build.

## Build 110
- Pointlights und ihre Shadowmaps folgen wieder der VisualFX-Sichtweite mit festem Nightly-Maximum, ohne eigenen F11-Regler.
- Schatten- und Vegetationspfade wurden weiter gegen die verbliebenen Streifen- und Flimmerartefakte nachgezogen; EVSM, Contact Shadows und die zugehörigen Shaderpfade wurden konsistenter angebunden.
- Anzeige- und F11-UI-Verhalten wurden vereinheitlicht, inklusive Display-Mode- und Tooltip-Polish sowie lokalisierter Startmeldung.
- Partikel- und Fog-Fälle sowie Distanz-/Dither-Verhalten wurden weiter geglättet, ohne die vorhandenen Effekte pauschal abzuschalten.
- Prüfung: statische Diff-, Aufrufer-, Binding- und Shaderpfadkontrolle; kein vollständiger lokaler C++-Build.

## Build 109
- Vegetation: die gestreiften Schattenartefakte auf vertikaler Alpha-Vegetation werden nur auf rückwärtigen Vegetationsflächen abgefangen; allgemeine Schatten auf anderen Oberflächen bleiben erhalten.
- Contact Shadows: die feste interne Stärke bleibt bei `0.35` für FSR3 und TAA, sonst bei `0.50`; der F11-Regler für Contact Shadows bleibt entfernt und die Option ist nur noch ein einfacher Enabler.
- F11: Tooltiptexte wurden kurz und allgemein formuliert; Displaymodus und Versionsanzeige bleiben auf den vereinbarten sichtbaren Positionen.
- EVSM: echter EVSM4-Filterpfad mit mehrstufigen Kaskaden und qualitativer Auflösungsstaffelung je nach Shadow-Quality weiter integriert.
- Prüfung: nur statische Diff-/Binding-/Aufrufer- und Shaderpfadkontrollen; kein lokaler Vollbau.

## Build 108 (Vegetation-Schatten, Contact Shadows und Versionsanzeige)
- Vegetation: die Schattenbehandlung ist auf Vegetations-Receiver eingegrenzt; die störenden gestreiften Artefakte auf vertikaler Alpha-Vegetation werden nur dort abgefangen, nicht auf allgemeinen Oberflächen.
- Contact Shadows: FSR3 nutzt fest intern `0.35`, ohne FSR3 fest `0.50`; der F11-Stärkeregler ist entfernt, der Schalter bleibt als einfacher Enable-Eintrag.
- F11/Version: die Versionsanzeige steht oben rechts auf Preset-Höhe, und die öffentliche Version ist fest auf `Version 18.0` ohne Git-/Datumszusatz gesetzt.
- Build-Pipeline: die Nightly-Build-Konfiguration gibt `18.0` als feste Versionsnummer weiter, damit der veröffentlichte Stand stabil bleibt.
- Prüfung: nur statische Diff-/Binding-/Aufrufer- und Shaderpfad-Kontrollen; kein lokaler Vollbau.

## Build 106 (MSM, Backlit Vegetation, Contact Shadows und Oillamp-Schatten)
- MSM: der Filter bleibt auch bei Shadow Softness ganz links im MSM-Pfad aktiv und nutzt dort einen sehr kleinen Momenten-Footprint statt eines Hard-/Simple-Fallbacks; höhere Softness-Werte skalieren den Momentenfilter stärker.
- Backlit Vegetation: Rückseiten-Transmission ignoriert normale Schattenstreifen nur auf der Rückseite, Vorderseiten-Schatten bleiben normal. Die allgemeine Vegetationsmaske wurde breiter gefasst, damit Baumlaub wieder sichtbarer auf dem Niveau der älteren Backlit-Wirkung bleibt, aber mit reduzierter interner Stärke 0,5.
- Contact Shadows und FSR3: Contact-History ist von der Farb-History getrennt und bleibt auch bei FSR3 Native AA stabiler, nicht nur bei Render Scale unter 100 Prozent.
- Indoor-Pointlights: NW_CITY_OILLAMP_01.3DS-Oillamps dürfen analog zu Flammen je das nächste statische und dynamische Licht bis 150 Einheiten als schattenberechtigt verankern; der Schattenursprung wird auf den Oillamp-Mittelpunkt plus 50 Höheneinheiten gezogen.
- Offene Ingame-Grenzen: Oillamp-Lichtzuordnung, MSM-Bildruhe bei niedriger Softness, Backlit-Wirkung auf normale Bäume und FSR3-Contact-Stabilität sollten im Spiel final gegengeprüft werden.
- Prüfung: Buildnummer aus outputs ermittelt; AGENTS-Regeln gelesen; gezielte Status-, Diff-, Shader-/Binding-, Oillamp-Anker- und git diff --check-Prüfungen statisch kontrolliert. Kein vollständiger lokaler C++-/Shader-Build.

## Build 105 (MSM-Hard-Softness, Backlit-Gate und Wind-Motion)
- MSM: bei Shadow Softness ganz links wird kein Momentfilter und keine Moment-Erzeugung mehr genutzt; der Pfad fällt auf direkten Shadow-Compare zurück, damit harte Schatten Simple-näher und günstiger bleiben.
- Backlit Vegetation: Sonnen-/Mondlicht, Forward+, Deferred-, Tiled- und dynamisch schattierte Pointlights koppeln die Rückseiten-/Ausnahme-Durchleuchtung vollständig an den F11-Schalter Backlit Vegetation.
- FSR3/Wind: instanzierte Vegetation schreibt Motion Vectors mit der vorherigen Windphase, damit starker Regenwind weniger History-Schlieren erzeugt.
- 12-Uhr-Weltschatten: Weltgeometrie im Sonnen-Shadowpass wird nicht mehr zusätzlich gegen das Kaskaden-Frustum verworfen; VOB-/NPC-Culling bleibt unverändert.
- Prüfung: AGENTS-Regeln gelesen; gezielte MSM-, Backlit-, Wind-CBuffer-, Weltmesh-Shadow-Culling- und Aufruferprüfungen sowie git diff --check statisch kontrolliert. Kein vollständiger lokaler C++-/Shader-Build.

## Build 104 (MSM-, Noon-, Backlit- und Preset-Korrekturen)
- MSM: Moment-Sampling auf direkte 4-Moment-Auswertung ohne grobe Moment-Mips/Blocker-Mip-Suche zurückgeführt, damit der Filter weniger pixelig und leichter wird.
- 12-Uhr-Weltschatten: wirkungslosen World-Mesh-Culling-Sonderfall entfernt und die Shadow-Kamera/Kaskadenprojektion am Zenith stabilisiert, ohne sichtbare Sonnen-/Mondbeleuchtung umzubiegen.
- Backlit Vegetation: gemeinsamer Transmissionspfad für Sonne/Mond, Forward+/Deferred-/Tiled-Pointlights und dynamisch schattierte Pointlights; benannte Ausnahmen nutzen dieselbe konturbetonte Durchleuchtung statt einfacher Rückseitenaufhellung.
- F11/Presets: Standard bleibt SSGI aus und Contact Shadows an. Presets setzen Low beide aus, Medium/High Contact Shadows an und SSGI aus, Extreme beide an.
- Prüfung: AGENTS-Regeln gelesen; Defaultwerte, Presetzuordnung, Shader-/Aufruferpfade, betroffene Diffs und git diff --check statisch kontrolliert. Kein vollständiger lokaler C++-/Shader-Build.

## Build 103 (MSM-, Noon-, Partikel- und Ressourcen-Korrekturen)
- MSM: Moment-Mip-Footprints an den tatsächlichen Filterdurchmesser gekoppelt, damit das sichtbare Shadowmap-Texelraster bei weichen Schatten reduziert wird.
- 12-Uhr-Weltschatten: Kaskaden wieder aus den echten Kamera-Frustum-Slices mit stabilem Texel-Snapping und dynamischen Tiefengrenzen aufgebaut; Weltgeometrie wird am exakten Zenith zweiseitig in die Shadowmap geschrieben.
- Partikel und Backlit: die normale Partikelbeleuchtung einschliesslich bestehender Tag-/Nachtabdunklung gilt wieder für Groundfog; die benannten Vegetationsausnahmen verwenden wieder die funktionierende identische Vorder-/Rückseitenbeleuchtung.
- Ressourcen: optimierte RainCloud.dds übernommen, ungenutzte VDB-Atlasdaten aus dem Partikel-Instanzlayout entfernt (60 statt 72 Byte) und den leeren fünften Sky-SRV-Slot entfernt.
- Offene Ingame-Grenzen: MSM-Bildruhe, 12-Uhr-Weltschatten, Groundfog-Nachtabdunklung und Backlit-Ausnahmen müssen im Spiel bestätigt werden.
- Prüfung: DDS-Format, Abmessungen, Mip-Anzahl und Quellhash kontrolliert; CPU-/Shader-Partikellayout, Sky-Bindings, betroffene Shaderpfade und git diff --check statisch geprüft. Kein vollständiger lokaler C++-/Shader-Build.

## Build 102 (MSM-Pipeline, Shadow-/PFX-Korrekturen und F11-Feinschliff)
- MSM: echte Moment-Shadow-Map-Pipeline mit optimierter 4-Moment-Kodierung, separaten Moment-Ressourcen, per-Cascade-Mips und lazy Cascade Updates nach valider Erstbefüllung integriert. Shadow Quality bleibt unabhängig vom Filter und erlaubt auch Extreme/8192 mit MSM.
- MSM-Korrektur: Receiver vor dem ersten Moment werden wieder explizit als beleuchtet bewertet, damit Licht und Schatten nicht invertiert wirken und der Singular-Fallback keine hellen Flächen abdunkelt.
- 12-Uhr-Schatten: den wirkungslosen Projektions-Clamp entfernt und stattdessen die World-Mesh-Shadow-Culling-Entscheidung im kritischen Noon-Fenster entschärft, passend zur beobachteten Beschränkung auf Weltgeometrie.
- Groundfog-PFX: Build-099-Texturinterpolation für nebelartige Partikel wiederhergestellt, damit großflächige Groundfog-/Rauch-PFX über Wasser wieder sichtbar werden.
- Contact Shadows: F11-Layout korrigiert, steile/vertikale Flächen weniger hart ausgeschlossen und die FSR3-History für Contact-Alpha enger geklemmt, um Nachziehen und Flackern zu reduzieren.
- F11: Shadow Filter sauber über Shadow Softness platziert, Titelbalken entfernt und Settings-Cursor auf den OS-Cursorpfad gelegt, damit er nicht mit niedriger Render-FPS mitruckelt.
- Prüfung: AGENTS-Regeln gelesen; gezielte Shader-, UI-, Workflow-/Buildnummer-, Diff- und Präprozessorprüfungen sowie git diff --check statisch kontrolliert. Kein vollständiger lokaler C++-/Shader-Build.

## Build 102 (Schatten-, PFX-, UI- und Screen-Space-Korrekturen)
- 12-Uhr-Schatten: den wirkungslosen Noon-Culling-Sonderfall entfernt und die Welt-Kaskadenprojektion auf den stabilen Kirides-17.9.7-Pfad mit festem Tiefenbereich und Texel-Snapping zurückgeführt.
- Groundfog-PFX: eindeutige Groundfog-/Fog-Smoke-Effekte behalten ihren originalen Blendmodus und lineare Emitter-Deckkraft; die allgemeine Nachtabdunklung normaler Partikel bleibt unverändert.
- F11/Maus: auflösungsunabhängige virtuelle Menüfläche eingeführt und OS-Mauskoordinaten auf dieselbe Fläche abgebildet, damit Größe und Trefferposition nach Auflösungswechseln stabil bleiben.
- Contact Shadows: FSR3-History tiefen-/normalvalidiert, Contact-Raymarch und Softfilter ressourcenschonender ausgelegt und wirkungslose Traces früh beendet.
- SSGI: vier rotierende Low-Discrepancy-Strahlen mit temporaler Akkumulation ersetzen acht zufällige Strahlen; Reichweitengewichtung und Energiebegrenzung reduzieren breite Lichtlecks bei deutlich weniger Raytests.
- Backlit Vegetation: vollständig aufgehellte Rückseiten durch konturbetonte, nach innen abklingende Lichttransmission ersetzt.
- Offene Ingame-Grenzen: 12-Uhr-Weltschatten und Groundfog-PFX müssen im Spiel bestätigt werden; SSGI und Contact Shadows sind getrennt auf Bildruhe, dünne Occluder und Leistung zu vergleichen.
- Prüfung: Referenzpfade gegen Kirides 17.9.7 abgeglichen; Aufrufer, Shaderregister, Constant-Buffer-Layouts, Optionen, Ressourcenpfade, Escape-Sequenzen und git diff --check statisch kontrolliert. Kein vollständiger lokaler C++-Build.

## Build 101 (MSM-Testfilter, Shadow-/PFX-Fixes und Velocity-Fallback)
- Schattenfilter: MSM als testbarer Shadow-Filter-Modus ergänzt und im F11-Menü als eigener Enabler eingebunden; Standard bleibt der bisherige einfache Schattenfilter.
- Contact Shadows: sichtbare raybasierte Contact-Shadow-Wirkung aus Build 099 wiederhergestellt und mit deterministischen Mehrfachrays stabilisiert, damit harte Unterbrechungen reduziert werden ohne die Funktion praktisch auszublenden.
- Groundfog-PFX: GROUNDFOG- und nebelartige additive PFX werden breiter erkannt, nicht durch die normale Distanz-/Frustum-Logik verworfen und bleiben ohne unpassende Partikelbeleuchtung sichtbar.
- 12-Uhr-Schatten: Shadow-Kamera-Pullback und Cascade-Update nahe Zenith stabilisiert, damit die Sonnen-Schattenmap um Punkt 12 Uhr nicht kurz ausfällt.
- TAA/Velocity: der Depth-Motion-Vector-Fallback nutzt nun dieselbe Reprojektionskonvention wie Geometry- und Sky-Velocities (`previousUV - currentUV`). Der normale MRT-Velocity-/FSR3-Pfad bleibt unverändert.
- Prüfung: AGENTS-Regeln gelesen; Buildnummer aus outputs ermittelt; gezielte Shader-/Konventionsprüfung, Diffkontrolle, BOM-/Escape-Sequenzprüfung und git diff --check statisch kontrolliert. Kein vollständiger lokaler C++-/Shader-Build.

## Build 100 (Rückbau VDB, Contact Shadows, Groundfog und Schattenstabilität)
- VDB-Rückbau: VDB-Wolken und VDB-Feuer vollständig aus Runtime-Code, Shadern, Paketdateien und Lizenzhinweisen entfernt. FIRE.PFX, FIRE_HOT.PFX und FIRE_COMPLETE_A0.TGA laufen wieder über die normalen Gothic-Partikel-/Decalpfade.
- Contact Shadows: der lange richtungsbasierte Screen-Space-Trace wurde durch eine kurze, weiche und deterministische Kontaktabdunklung ersetzt, damit die unterbrochenen Muster, Blickwinkel-Sprünge und FSR-3-Flackerstellen reduziert werden.
- Backlit Vegetation: NW_NATURE_WATERGRASS_56P als weitere Prefix-Ausnahme für identische Vorder-/Rückseitenbeleuchtung ergänzt.
- Schatten um 12 Uhr: die Shadow-Kamera-Up-Richtung wird nahe Zenith früher stabilisiert, ohne Sonne-/Mondzeiten oder Lichtübergaenge zu verändern.
- Groundfog-PFX: GROUNDFOG-Partikel werden nicht mehr durch die zu harte PFX-BBox-Frustum-Prüfung verworfen; Draw-Radius und showVisual bleiben erhalten.
- F11/Starttext: Startanzeige weist zusätzlich auf F11 für Grafikeinstellungen hin; das F11-Fenster zeigt keine Versionsnummer mehr im Titel.
- Prüfung: AGENTS-Regeln gelesen; gezielte VDB-Symbol-/Asset-/Lizenzprüfungen, Shaderregister-/Escape-Sequenzprüfung, relevante Git-Diffs und git diff --check statisch kontrolliert. Kein vollständiger lokaler C++-/Shader-Build.

## Build 099 (Clouds, VDB-Feuer, Backlit-Menü und Contact-Shadow-Stabilisierung)
- VDB-Wolken: die Atlaswolken wurden von übergrossen Flächen auf mehrere kleinere lokale Schichten umgestellt. Die Randmaske ist weich, die Kerne bleiben dichter, Tageshimmel und Sternenhimmel bleiben zwischen den Wolken sichtbar; Nacht- und Regenfärbung bleiben erhalten.
- VDB-Feuer: FIRE_HOT.PFX für Lagerfeuer wird robuster auch bei Pfad-/Quote-/Namenszusatz erkannt. FIRE.PFX und FIRE_HOT.PFX nutzen globale fraktionale Atlas-Animation mit Frame-Interpolation und vertikaler VDB-Orientierungskorrektur gegen Stocken und falsche Ausrichtung; FIRE_MEDIUM und TORCH bleiben unverändert.
- Backlit Vegetation: der F11-Eintrag steht direkt über Enable Rain. Die zweitseitige Beleuchtung matcht die genannten Vegetationsnamen als Wortstamm/Prefix, etwa NW_NATURE_GRASSGROUP_01, OW_NATURE_BUSH_02*, OW_NATURE_BUSH_03*, NW_NATURE_PLANT_03*, NW_KORN* und OW_GRASS_WINTER*.
- Contact Shadows: das pixelgebundene Contact-Jitter wurde im Contact-Shadow-Pfad entfernt, der ferne Weltgeometrie-Bereich früher ausgeblendet und die Contact-Alpha-Maske normal-/tiefenbewusst weich gefiltert. SSGI-Sampling und FSR-3-Pfad bleiben unverändert.
- Prüfung: AGENTS-Regeln gelesen; gezielte Status-/Diff-/Shaderstellenprüfungen, BOM-/Zeilenumbruchkontrolle, literal eingefügte Escape-Sequenzen und git diff --check statisch kontrolliert. Kein vollständiger lokaler C++-/Shader-Build.

## Build 098 (CI-Korrektur)
- CI-Korrektur: NPC-Materialmarker für per-draw Node-Attachments im gültigen Vob-Kontext erzeugt und den unveränderten VDB-Feuer-Instanzvektor explizit an die const-inkorrekte VertexBuffer-Upload-Schnittstelle angepasst.
- Inhaltlich keine weiteren Renderer-, Shader- oder Ingame-Änderungen gegenüber Build 097.
- Prüfung: beide gemeldeten Release_G1_AVX2-Compilerstellen, Variablengültigkeit, UpdateBuffer-Signatur, betroffene Aufrufer und git diff --check statisch kontrolliert; kein vollständiger lokaler C++-/Shader-Build.

## Build 098 (Wolken, Feuer, Backlit, Contact Shadows und DoF)
- VDB-Wolken: die Atlaswolken werden als mehrere kleinere, lokal vorbeiziehende und dichter dargestellte Schichten verteilt; blauer Tageshimmel und Sternenhimmel bleiben zwischen den Wolken sichtbar, Nacht- und Regenfärbung bleiben erhalten.
- VDB-Feuer: FIRE_HOT und FIRE werden zusätzlich über den exakten Vob-Namen erkannt, damit Atlasflamme und Unterdrückung der alten Partikel beziehungsweise nahen FIRE_COMPLETE_A*-Decals verlässlich greifen; FIRE_MEDIUM und TORCH bleiben unverändert.
- Backlit Vegetation: F11 nutzt einen reinen Enabler mit festem internen Wert 0,5; ein alter INI-Stärkewert beeinflusst die Funktion nicht mehr. Identische Vorder-/Rückseitenbeleuchtung gilt exakt für NW_NATURE_GRASSGROUP, OW_NATURE_BUSH_02, OW_NATURE_BUSH_03, NW_NATURE_PLANT_03, NW_KORN und OW_GRASS_WINTER.
- Contact Shadows: der wirkungslose FSR-3-Sonderpfad wurde entfernt. Deterministisches Sampling, dichtere Ray-Schritte und robustere Nahkontakt-Toleranzen reduzieren Blickwinkel-Sprünge und FSR-3-Flackern; NPC-Kontakte bleiben sichtbar, aber gegen harte Gesichtsartefakte begrenzt.
- Depth of Field: der F11-Stärkeregler beeinflusst nur noch die Hintergrund-/Fernunschärfe; die Vordergrundunschärfe behält ihren festen bisherigen Standardradius.
- Prüfung: betroffene Shaderregister, Constant-Buffer-Layouts, PFX-Erkennung, F11-/INI-Pfade, doppelte Hilfsdefinitionen, literal eingefügte Escape-Sequenzen und git diff --check statisch kontrolliert. Kein vollständiger lokaler C++-/Shader-Build.

## Build 097 (VDB-Feuer und -Wolken, Graslicht und Contact Shadows)
- VDB-Feuer: FIRE_HOT.PFX und FIRE.PFX werden durch kompakte animierte Atlas-Flammen für Lagerfeuer beziehungsweise Kamine ersetzt; FIRE_MEDIUM.PFX und TORCH.PFX bleiben unverändert. FIRE_COMPLETE_A0.TGA wird nur im Nahbereich eines FIRE.PFX-Kaminfeuers unterdrückt; bei fehlendem Atlas bleibt das originale Partikelfeuer als Fallback erhalten.
- VDB-Wolken: zehn gepackte Volumenwolkenvarianten als zwei animierte Atmosphärenschichten für Tages-, Nacht- und Regenstimmung integriert. Die bestehende Regenwolkendecke bleibt erhalten; es gibt keinen zusätzlichen Vollbild- oder Schattenpass.
- Backlit Vegetation: Visualnamen mit GRASSGROUP erhalten bei aktivierter Funktion identische Vorder- und Rückseitenbeleuchtung ohne zusätzlichen Backlit-Aufschlag. Der F11-Regler steuert weiterhin nur die Backlit-Stärke der übrigen Vegetation.
- Contact Shadows: NPC-Pixel werden gezielt erkannt und nutzen kürzere, weichere und schwächere Contact-Rays gegen harte Gesichts- und Halsartefakte. Weltgeometrie behält die bisherige Wirkung.
- FSR 3: nur das Contact-Shadow-Raymuster und dessen temporale Alpha-Historie wurden stabilisiert, um insbesondere fernes Flackern zu reduzieren; Upscaling, Schärfung, SSGI, Wasser und sonstige FSR-3-Pfade bleiben unverändert.
- Lizenzen/Paket: erforderlicher JangaFX-CC0-Hinweis kompakt in GD3D11/Licences.txt ergänzt; ausgeliefert werden nur die beiden komprimierten Laufzeitatlanten.
- Prüfung: Ressourcenpfade und Paketaufnahme, Atlasabmessungen/-inhalt, PFX-Zuordnung und Fallbacks, GBuffer-Marker und alle Beleuchtungsdecoder, Rendergraph-/Shaderregister-/Constant-Buffer-Bindings, Klammer-/Präprozessorpaare und git diff --check statisch kontrolliert. Kein vollständiger lokaler C++-/Shader-Build.

## Build 096 (Stabilität, HDR, Beleuchtung und Release-Lizenzen)
- Release-Lizenzen: Attributions- und Lizenztexte in einer einzigen GD3D11/Licences.txt gebündelt; doppelte MIT-Texte, nicht ausgelieferte Komponenten und der nicht benötigte Hinweis für die einzelne Regentextur entfernt. Die Paketierung entfernt zusätzlich die separate XeGTAO-Lizenzkopie.
- Sonne/Mond: Abendlicher Wechsel auf die korrekte Zeit 19:15-19:45 verschoben; Sonnenlicht/-schatten blenden 19:15-19:30 aus, Mondlicht/-schatten 19:30-19:45 ein. Godrays und Backlit Vegetation folgen den Lichtgewichten; Wasser-Glints bleiben unabhängig.
- Regennebel: Tages- und Nachtstärke auf den Stand vor der letzten Verstärkung zurückgeführt; der weichere, längere Distanzübergang bleibt erhalten.
- Contact Shadows: Nah- und Mittelbereich mit breiteren Rays, zehn Schritten und höherer Wirkung deutlich sichtbarer; Fernbereich endet früher, um Flackern nicht zu verstärken. FSR 3 selbst bleibt unverändert.
- Lade-/Exit-Stabilität: Welt-, BSP-, VobTree- und MeshManager-Zugriffe abgesichert; leere Geometrie wird ohne polys[0]-Zugriff behandelt. PostFX-/FSR-Ressourcen werden vor dem D3D-Gerät freigegeben und der optionale D3D-Debugzeiger sicher initialisiert.
- Water Effects: sichtbarer Standard und F11-Regler bleiben bei 1,0 beziehungsweise 0,0-2,0; 1,0 entspricht nun der bisherigen Wirkung von 1,4. Wasser- und Regenboden-SSR nutzen dieselbe Normalisierung.
- HDR: Luminanzhistorie startet und resettiert bei Welt-/Spielstandladen neutral auf 18-Prozent-Grau statt maximaler Startbelichtung. Helle und dunkle Adaption bleiben weich getrennt, erreichen das Ziel aber deutlich schneller.
- Prüfung: gezielte Git-Diffs, Projekt-/Shaderregistrierungen, Lade-/Ressourcenlebenszyklen, Feature-Gates, Definitionen, Konfliktmarker und git diff --check statisch kontrolliert. Kein vollständiger lokaler C++-/Shader-Build.

## Build 095 (Dämmerung, Screen-Space-Licht und Regennebel)
- Sonne/Mond: Licht, Schatten, Godrays und Backlit Vegetation nutzen getrennte 15-Minuten-Übergaenge. Mondlicht blendet morgens 04:15-04:30 aus, Sonnenlicht 04:30-04:45 ein; abends blendet Sonnenlicht 17:30-17:45 aus und Mondlicht 17:45-18:00 ein. Schattenquellen überlappen nicht.
- Himmel/Wasser: der Mondkörper bleibt positionsbasiert sichtbar statt über das Lichtgewicht ausgeblendet zu werden. Sonnen- und Mond-Glints auf Wasser laufen positionsbasiert unabhängig von den Licht-/Schatten-Fades weiter.
- Godrays/Backlit: Godrays folgen dem Sonnenlichtgewicht inklusive Regen-Ausblendung; Backlit Vegetation folgt den vorhandenen Sonnen-/Mond-Lichtgewichten inklusive Regen-Ausblendung.
- Contact Shadows/SSGI: SSGI sampelt breiter und verlässlicher mit weiter gedeckelter Energie, damit mehr Flächen subtil reagieren ohne die alten Lichtstreifen zurückzubringen. Contact Shadows werden stärker auf Nahkontakte begrenzt und in der Ferne ausgeblendet, ohne FSR 3 selbst zu verändern.
- Regennebel: die Geometrie-Nebelrampe bei Regen beginnt weicher über eine größere Distanz, nachts wirkt der Regennebel stärker und tagsüber bei Regen leicht. Himmel-/Wolkenpfad, FSR 3, normale Regen-Transitions und trockener Nachtnebel bleiben unverändert.
- Prüfung: Shader-Diff, Klammer-/Escape-Prüfungen, Screen-Space-Lighting-Constant-Buffer-Layouts, Zeitfenster-Samples und `git diff --check` statisch kontrolliert. Kein lokaler HLSL-Compiler und kein vollständiger lokaler C++-Build ausgeführt.

## Build 094 (CI-Korrektur)
- CI-Korrektur: Default-Renderer-Settings in LoadMenuSettings vor die optionale UserSettings-Dateiprüfung gezogen, damit der anschliessende Reset versteckter Kirides-/Advanced-Werte in jedem Pfad auf den gültigen Default-Snapshot ds zugreifen kann.
- Inhaltlich keine weiteren Renderer-, Shader- oder Ingame-Änderungen gegenüber Build 093.
- Prüfung: gemeldete Release_G1_12f-Compilerstellen, Deklarationsgültigkeit im gesamten Funktionspfad und git diff --check statisch kontrolliert; kein vollständiger lokaler C++-Build.

## Build 094 (Bereinigung und Licht-/Wetter-Korrekturen)
- Release-Bereinigung: 1043 nachweislich unbenutzte Drittanbieter-, Beispiel- und Dokumentationsdateien entfernt; erforderliche Lizenz- und Attributionshinweise in `blobs/licenses` erhalten und in die Release-Paketierung aufgenommen.
- Sonne/Mond: gerichtetes Licht, Schatten und Wasserreflexionen wechseln morgens 04:58-05:02 und abends 18:58-19:02 in getrennten weichen Zwei-Minuten-Fenstern; Sonnen- und Mondschatten überlappen nicht.
- Contact Shadows/SSGI: falsche Hauptlichtrichtung der Contact-Rays korrigiert und Treffer robuster gemacht. SSGI nutzt pixel-/framevariierte Strahlen, mindestens zwei Treffer, Energiebegrenzung und normalbewusstes Temporal-Clamping gegen horizontale Lichtstreifen.
- Regen: Nacht-Regentropfen rund 20 Prozent transparenter, gemeinsam für nativen und FSR-3-Pfad. Manuelles F11-Wiedereinschalten übernimmt den aktuellen Regenzustand sofort; natürliche Wetterübergaenge behalten ihre bisherige Rampe.
- Regennebel: nur die Deckkraft auf Weltgeometrie bei gleichzeitig Nacht und aktivem Regen leicht angehoben; Wolken-/Himmelpfad, Nachtnebel und sonstige Geometrie-/Himmelübergaenge bleiben unverändert.
- F11/HDR: HDR ist für neue Standardwerte aktiv. Deaktivierte gekoppelte Effektregler werden auch beim ersten Öffnen konsistent auf Null dargestellt.
- Prüfung: Projekt- und Shaderregistrierungen, Include-/Ressourcenpfade, Constant-Buffer-Layouts, Paketquellen, literal eingefügte Escapes und `git diff --check` statisch kontrolliert. Kein lokaler HLSL-Compiler und kein vollständiger lokaler C++-Build vorhanden.

## Build 093 (Bodenlicht und Regenwolken-Korrektur)
- Pointlights/Fackeln: gemeinsamer NdotL-Helfer für Deferred-, Dynamic-Shadow-, Tiled- und Forward+-Pfade ergänzt. Nach oben gerichtete Bodenflächen erhalten bei sehr bodennahen Lichtquellen einen kleinen lokalen Wrap-Anteil, damit eine abgelegte Handfackel den Boden beleuchtet; Reichweite und vorhandene Schattenauswertung bleiben erhalten.
- Regenwolken: zweite Detailwolken-Schicht samt Shaderbinding, Ressourcenpfad und unbenutzter `RainCloudDetail.dds` vollständig entfernt. Die einzelne Wolkendecke wird nun als `RainCloud.dds` ausgeliefert.
- Wolkentextur: gegenüberliegende Randbereiche der gelieferten Gothic-Wolke symmetrisch verblendet und erneut als BC1-DDS mit elf Mip-Stufen erzeugt; dadurch schließt die wiederholte Textur ohne sichtbare harte Kachelgrenzen.
- Regennebel: Standarddichte von `0.00050` auf `0.00078` angehoben und bestehende INIs mit exakt dem alten Standardwert migriert; individuell abweichende Werte bleiben erhalten. Die Sky-Abschwächung wurde so angepasst, dass der dichtere Nebel auch vor dem Himmel sichtbar ist, die Wolkendecke aber durchscheint.
- Occlusion Culling: Large-VOBs werden im Hauptsichtbarkeitspass nicht mehr occlusion-gecullt, damit grosse Objekte wie Bäume nicht spät aufpoppen; Small-VOBs und Mobs laufen weiter durch den strengeren Occlusion-Pfad für den eigentlichen Performancegewinn.
- Contact Shadows/SSGI: Screen-Space-Licht in einen eigenen Trace- und Temporalpass ausgelagert. Contact Shadows sind wasser-/normalgefiltert und begrenzt; SSGI nutzt stabile hemisphärische Strahlen, History-Reprojection, Depth-Validation und Neighborhood-Clamping gegen flackernde Striche und punktförmige Artefakte.
- UserSettings.ini: Welt-/Menüeinstellungen werden für den Build auf sichtbare F11-Werte begrenzt; versteckte Kirides-/Advanced-Optionen und alte Gamma/Brightness-Schlüssel werden beim Speichern entfernt. Display-Tuning bleibt ausschließlich über `DisplayContrast` und `DisplayBrightness` erhalten.
- Prüfung: alte Base-/Detail-Referenzen und unbenutzte Detaildatei entfernt; DDS-Format, Abmessungen, Mip-Anzahl und Randkontinuität kontrolliert; Atmosphären-, Pointlight-, Tiled-, Forward+-, Composition- und Screen-Space-Lighting-Shader statisch auf Ressourcen-, Register-, Klammer- und Präprozessor-Konsistenz geprüft; Occlusion-Flags, Rendergraph-Aufrufer und UserSettings-Lese-/Speicherpfade mit Kirides 17.9.7/Nightly abgeglichen; `git diff --check` sauber. Kein lokaler HLSL-Compiler und kein vollständiger lokaler C++-Build vorhanden.

## Build 092 (Regenwolken und VFX-Rollback)
- Regenwolken: Base-Projektion von einem nahezu konstanten Himmelsausschnitt auf eine klar wiederholte Dome-Projektion umgestellt; die transparente Detailbank nutzt eine dichtere, gedrehte Projektion mit schnellerer Eigenbewegung und stärkerer Parallaxe. Regennebel, Wetterübergang, Horizont und Nachtlogik bleiben unverändert.
- Presets: Medium nutzt im F11-Menü für FSR 3 nun Render Scale High Quality statt Quality.
- Fackel/VFX: den erfolglosen Indoor-/Outdoor-Transition-Fallback vollständig auf den stabilen Stand von Build 088 zurückgestellt. Die kurze bekannte Schattenlücke an der Grenze wird bewusst akzeptiert; der zusätzliche Frame-Light-Scan sowie die erweiterten Parent-/Remove-Sonderpfade sind entfernt.
- Contact Shadows/SSGI: Wasser- und Wasserfallmaske auch für Screen-Space-Lichteffekte erzeugt und in die Composition gereicht; Wasser wird als Empfänger und Ray-Treffer ausgeschlossen. Contact Shadows nutzen kürzere stabilere Rays mit begrenzter Abdunklung; SSGI nutzt stabile Richtungen, Mindesttreffer und Highlight-Kompression gegen punktförmige Artefakte.
- Prüfung: betroffene Fackel-/VFX-Funktionen blockweise exakt mit Build 088 verglichen; Wolken-UV-Abdeckung gegen beide ausgelieferten DDS-Dateien stichprobenartig ausgewertet; Shaderstruktur, Ressourcenpfade und `git diff --check` statisch kontrolliert. Kein vollständiger lokaler C++-/Shader-Build.

## Build 091 (Stabilitäts- und Darstellungsfixes)
- VFX-/Fackelstabilität: zusätzlichen rohen Transition-Light-Container entfernt; Inventarentfernung prüft nun das konkrete Vob. Dynamische am NPC getragene sowie echte VisualFX-Lichter bleiben wie die Handfackel über die autoritative Light-Map sichtbar und schattenberechtigt.
- Pointlights/NPCs: Indoor-Kennbit für Skeletal-GBuffer-Pixel wiederhergestellt, ohne die sichtbare NPC-Farbe oder atmosphärische Lichtstärke abzuschwächen.
- Screen-Space-Licht: bisherigen Nachbarschaftsfilter ersetzt. SSGI verfolgt sechs hemisphärische View-Space-Strahlen mit Depth-Treffern und sammelt sichtbare Ein-Bounce-Radiance; Contact Shadows verfolgen kurze View-Space-Sichtstrahlen zur aktiven Sonne beziehungsweise zum Mond. Nur High und Extreme aktivieren beide Effekte.
- Regen/Wetter: F11-Regen-Aus deaktiviert Wolkendecke und wetterabhängige Lichtdämpfung sofort. Fernwelt/Himmel folgen einem einzigen monotonen 60-Sekunden-Wetterwert; nasse Böden dürfen unabhängig davon weiter trocknen.
- Regenwolken: bereitgestellte Gothic-Base als langsam bewegte, wiederholte obere Decke integriert; transparente Detailwolke als tiefere, schneller driftende Schicht mit eigener Projektion und stärkerer Parallaxe. Unbenutzte PNG-Duplikate entfernt.
- Sky-Boden: wirkungslosen Schwarzreturn aus Build 089 wieder entfernt; Horizont, Nachtnebel und Weltüberblendung bleiben unverändert.
- Wasser: fehlerhaften Kamera-Depth-Occlusion-Trace entfernt, damit Vordergrundgeometrie Sonnen- und Mondglanz auf dahinterliegendem Wasser nicht mehr zerschneidet; Regenausblendung bleibt erhalten.
- HDR: Normwert 1,0 bleibt bei intern 7,5; die Stufen oberhalb intern 10 bis zum Maximum 15 verstärken LPM nun weiter statt am bisherigen Blend-Limit zu sättigen.
- Prüfung: statische Lebenszyklus-, Aufrufer-, Constant-Buffer-, Shaderregister-, Preset-, Ressourcen- und Diff-Prüfungen; kein vollständiger lokaler C++-/Shader-Build.

## Build 090 (Korrekturpush)
- CI-Korrektur: VFX-Fallback-Bounding-Sphere speichert die effektive Lichtposition nun explizit per `XMStoreFloat3` in `DirectX::XMFLOAT3`; damit ist die in Release_G1_12f gemeldete ungültige Zuweisung von `float3` behoben.
- Inhaltlich keine weiteren Renderer-Änderungen gegenüber Build 089.
- Prüfung: gemeldete Compilerstelle, alle weiteren Aufrufer von `GetEffectivePositionWorld`, Typgrenzen und `git diff --check` statisch kontrolliert; kein vollständiger lokaler C++-Build.

## Build 089
- F11/Presets: `Custom` aus der Auswahlliste entfernt, bleibt als erkannter Zustand sichtbar; Presets steuern nur sichtbare F11-Werte, während sichtbare AA-Modi ihre normale CAS-Folge anwenden dürfen.
- F11-Effektregler: einheitliche 0-2-Stufenskalen mit gekoppelten Enablern, Wiederherstellung des letzten aktiven Werts und dezenter inaktiver Skala; HDR neu normiert (`1.0` entspricht dem bisherigen Wert `7.5`, `2.0` entspricht `15`).
- Screen-Space-Licht: Contact Shadows und Screen-Space GI getrennt, mit eigenen Enablern, Reglern und kurzen Tooltips; High und Extreme aktivieren beide Effekte.
- Schatten/Lichter: Extreme-Pointlight-Schatten auf 256 gesetzt; VFX-Lichter bleiben am Indoor-/Outdoor-Übergang samt Bodenschatten gesammelt, ohne den funktionierenden Handfackelpfad oder atmosphärische Schattenregeln umzubauen.
- Himmel/Wetter: Unterseite des Taghimmels hart schwarz; neue mehrschichtige Regenwolkendecke, angepasste Backlit-Vegetation und sichtbare Wolken trotz Regennebel; Regenbeginn und monotones Aufklaren dauern jeweils mindestens 60 Sekunden und ignorieren auslaufende Controllerpulse.
- Wasser: terrainverdeckte Sonnenreflexion stabilisiert, passende Mondreflexion ergänzt und beide bei Regenwolken ausgeblendet; gemeinsamer optisch gleichwertiger Occlusion-Trace reduziert doppelten Shadercode.
- Composition: Heightfog nur outdoor angewendet, auch wenn Contact Shadows oder SSGI indoor aktiv bleiben; Nutzerfassung von `starsh.dds` unverändert übernommen.
- Prüfung: Atmosphären-, Wasser-, HDR- und Composition-Shader sowie mehrere Composition-Makrovarianten erfolgreich mit DXC kompiliert; statische Aufrufer-, Binding-, Ressourcen-, Escape- und `git diff --check`-Prüfungen; kein vollständiger lokaler C++-Build.

## Build 088
- F11/Presets: Presets auf Low/Medium/High/Extreme bereinigt; Shadow Quality steuert Welt- und Pointlight-Schatten gemeinsam, Reset-to-Defaults sowie Pointlight-/World-Shadow-/Shadow-Filtering-Auswahl entfernt.
- Schatten: Pointlight-Schatten laufen fest dynamisch; Simple-PCF bleibt Standard, PCSS-Codepfad entfernt; sichtbare berechtigte dynamische Lichter aktualisieren ihre Schatten pro Frame.
- HDR/LPM: HDR Tone Mapping wieder links bei Contrast/Brightness mit Enabler und Strength-Slider 1-10; LPM-Stärke und Bloom-Anteil folgen dem Slider, Auto-Exposure bleibt enger begrenzt.
- Preset-Erkennung: VSync, FPS-Limit, HDR, Display, Helligkeit und Kontrast sind preset-unabhängig; zurückgestellte Preset-Werte werden wieder als passendes Preset erkannt.
- Licht/SSR: unberechtigte Atmolichter gedämpft; Wasser-Sonnen-Glanz stabiler gegen SSR-Geometrie verdeckt, ohne den SSR-Pfad selbst umzubauen.
- Prüfung: statische UI-/INI-/Shader-/Constant-Buffer-/Aufrufer-, Escape- und git diff --check-Prüfungen; kein vollständiger lokaler C++-/Shader-Build.

## Build 087
- F11/Presets: Pointlight-Shadow- und Shadow-Filtering-Auswahl entfernt; Pointlight-Schatten laufen intern dauerhaft dynamisch, Presets setzen diese Modi nicht mehr.
- Presets/UI: Low nutzt FSR Balanced, Ambient Occlusion, Backlit Vegetation, Wind und Characters-affect-objects; VSync bleibt standardmäßig aus und preset-unabhängig; Object-Draw-Distance auf 1-10 skaliert.
- HDR/LPM: Auto-Exposure enger begrenzt, damit LPM weniger stark nach oben beziehungsweise unten regelt.
- Wasser: prozeduraler Sonnen-Glanz wird nur dort gedämpft, wo Wasser-SSR bereits valide Szenengeometrie reflektiert; SSR selbst bleibt unverändert.
- Pointlight-Schatten: schattenberechtigte sichtbare Lichter im aktiven Radius werden im Dynamic-Pfad pro Frame aktualisiert; nicht berechtigte Atmolichter bleiben ausgeschlossen.
- Prüfung: statische UI-/INI-/Shader-Aufrufer-, Escape- und git diff --check-Prüfungen; kein vollständiger lokaler C++-/Shader-Build.

## Build 086
- HDR/LPM: offiziellen FidelityFX-LPM-Pfad mit moderatem Kontrast, Highlight-Shoulder und Farbsättigung für kräftigere Tiefenwirkung abgestimmt; Belichtung und 18%-Mittelgrau bleiben unverändert.
- Pointlight-Modi: auf `Static` und `Dynamic` bereinigt; dynamische VisualFX-Lichter nutzen ohne künstliche Dreiergrenze die normalen Schatten-/Atlaslimits und bleiben bei fehlendem Schattenslot weiterhin als Licht sichtbar.
- Pointlight-Zuordnung: Weltflammen und Parent-VOB-Lichter global aufgelöst; pro Flamme höchstens das bevorzugt verwandte beziehungsweise nächste statische und nichtstatische Licht innerhalb 150 Einheiten verankert.
- Mehrflammen-/Konfliktfälle: TGA dominiert ein einzelnes TGA/PFX-Paar; mehrfach beanspruchte Lichter und Leuchten mit mehreren gleichartigen Flammen behalten ihre gesetzte Position und bleiben schattenfähig.
- Prüfung: statische Parameter-, Shaderflag-, Aufrufer-, Ressourcen-, UI-/INI- und `git diff --check`-Prüfungen; kein vollständiger lokaler C++-/Shader-Build.

## Build 086 (Korrekturpush)
- Korrekturpush: lokalen `ParentClaim`-Standardwert von der umgebenden Variable entkoppelt; MSVC kann den Vector damit regulär über `std::construct_at` default-konstruieren.
- Inhaltlich keine weiteren Renderer-Änderungen gegenüber Build 086.
- Prüfung: gemeldete Template-Instanziierung, ähnliche lokale Initialisierer und `git diff --check` statisch kontrolliert; kein vollständiger lokaler C++-Build.

## Build 085
- Korrekturpush: offizielle FidelityFX-CPU-Helfer für das LPM-Setup kompatibel zugeordnet und den vollständigen zCVobLight-Typ eingebunden.
- Inhaltlich keine weiteren Renderer-Änderungen gegenüber Build 084.
- Prüfung: gemeldete Compilerstellen, Präprozessor-Zuordnungen, Includes und Diff statisch kontrolliert; kein vollständiger lokaler C++-/Shader-Build.

## Build 085 (Inhaltspush)
- HDR/LPM: Shaderaufruf auf den offiziellen FidelityFX-LPM-Filterpfad korrigiert; LPM kompiliert wieder mit passender 6-Parameter-Signatur.
- Pointlight-Flammen: PFX-/TGA-Flammen werden anhand von Visual-/Textur-Namen robuster erkannt; nahe doppelte PFX/TGA-Flammen werden als ein Flammenpunkt behandelt, TGA-Position bevorzugt.
- Pointlight-Schatten: kleine berechtigte Lichter bleiben ohne 300er-Sperre schattenfähig; Flammen-/Parent-Zuordnung bleibt auf dauerhafte Weltlichter begrenzt.
- VFX-Schatten: VisualFX-erzeugte zCVobLight-Lichter können im Modus `Dynamic + VFX` dynamische Schatten werfen; bewegliche VFX-Lichter sind auf zwei aktive Schattenlichter mit 0,5s Fade begrenzt, stabile VFX-Lichter nutzen den normalen Dynamic-Pfad.
- F11/Presets: Pointlight-Modus `Dynamic + VFX` im Preset Very High, World-Draw-Distance-Skala auf 1-10 mit Low/Mid/High/Very High 3/4/5/6.
- Prüfung: statische Diff-, Aufrufer-, Projektdatei-, Shader-Signatur-, Escape- und `git diff --check`-Prüfungen; kein vollständiger lokaler C++-/Shader-Build.

## Build 085 (Korrekturpush)
- Korrekturpush: `SectionDrawRadius`-INI-Laden explizit als `int` geklemmt und danach auf den Zieltyp gecastet; Release_G1_AVX kompiliert damit über die gemeldete `std::clamp`-Mehrdeutigkeit hinaus.
- Inhaltlich keine weiteren Renderer-Änderungen gegenüber Build 085 (Inhaltspush).
- Prüfung: CI-Fehlerstelle und Diff statisch kontrolliert; kein vollständiger lokaler C++-Build.

## Build 084
- Korrekturpush: Pointlight-Quellenklassifizierung für dynamisch erfasste Lichter über die GothicAPI-Instanz aufrufbar gemacht; Release_G1 und Release_G1_12f kompilieren damit über die gemeldete Fehlerstelle hinaus.
- Inhaltlich keine weiteren Renderer-Änderungen gegenüber Build 083.
- Prüfung: beide CI-Fehlerstellen, Sichtbarkeit, Aufrufer und Diff statisch kontrolliert; kein vollständiger lokaler C++-/Shader-Build.

## Build 084 (Inhaltspush)
- HDR: bisherige Eigenbau-LPM-Kurve durch den offiziellen AMD-FidelityFX-LPM-Setup-/Filterpfad ersetzt; lineares 0,18-Mittelgrau und korrekte LPM-Ausgabe, Legacy bleibt vorerst zum Ingame-Vergleich erhalten.
- Pointlight-Schatten: genau eine zugeordnete Flamme bestimmt Licht- und Schattenposition; bei keiner oder mehreren Flammen bleibt die originale Light-VOB-Position.
- Pointlight-Hierarchie: Parent-Lichter werden bis zum ersten Nicht-Licht-Parent übersprungen; NPC-/VFX-Hierarchien bleiben ausgeschlossen.
- Pointlight-Reichweite: starre 300er-Sperre entfernt; kleine berechtigte Lichter legen Schattenressourcen erst bei Sichtbarkeit an.
- Prüfung: statische Diff-, Aufrufer-, Positions-, Constant-Buffer-, Shader- und Paketpfadprüfungen; kein vollständiger lokaler C++-/Shader-Build.

## Build 083
- Frame Generation: den unbrauchbaren DX11-FG-/Optical-Flow-Pfad samt exklusiven Quellen entfernt; FSR 3.1.2 bleibt unverändert als Upscaler erhalten.
- HDR: Legacy und LPM neu abgestimmt, ungenutzte Tone-Mapping-Modi entfernt und die kompakte Auswahl neben HDR im Display-Bereich angeordnet.
- F11-Menü: nach Öffnen und Auflösungswechsel verlässlich zentriert, danach weiterhin per Titelleiste verschiebbar.
- Pointlight-Schatten: Dynamic/Full-Verhalten des ursprünglichen Kirides-Nightlys wiederhergestellt; Schatten nur für dauerhafte VOB-Lichter oder vorhandene parentlose Lichter nahe erkannter Flammen-PFX/-TGA.
- Kaskadenschatten: jitterstabile Tiefenrekonstruktion für TAA/FSR sowie lit Border-Sampling und Z-Grenzprüfung gegen Flimmern und dunkle Kaskadenränder.
- Prüfung: statische Diff-, Aufrufer-, Constant-Buffer-, Shaderbinding-, Ressourcen- und Projektdateiprüfungen; kein vollständiger lokaler C++-/Shader-Build.

## Build 082
- FOV: normale F11-FOV-Funktion und Runtime-Override entfernt; alte INI-FOV-Werte werden gelöscht beziehungsweise ignoriert, Gothic bleibt bei der nativen Kamera/Projektion.
- F11-Menü: Fenster zentriert beim Öffnen und nach Auflösungswechsel, bleibt während der Sitzung aber wieder per Titelleiste verschiebbar.
- DoF: Schärfe-/Unschärfeübergaenge nutzen kameraradiale Tiefe statt nur View-Z, damit der Fokus beim Drehen stabiler bleibt.
- HDR: kurze Tone-Mapping-Auswahl `Legacy`/`LPM` im F11-Menü ergänzt; alte interne HDRToneMap-Werte werden auf Legacy normalisiert.
- Prüfung: statische Diff-, Escape-, Shaderpfad- und UI-Prüfungen; kein vollständiger lokaler C++-/Shader-Build.

## Build 081
- FOV/Weitwinkel: 100 bleibt exakt Gothics Originalprojektion; höhere Werte vergrößern horizontalen und vertikalen Projektionswinkel mit identischem Tangensfaktor, sodass keine achsenabhängige Stauchung oder Dehnung entsteht.
- F11-Menü: Einstellungsfenster bleibt konsequent mittig; FOV-Hilfetext und Endbezeichnung wurden kurz und wirkungsbezogen formuliert.
- FSR3/Dialoggesichter: Kopfaufsatz verwendet vorherige Kopfknochenmatrix; Morph-Meshes liefern vorherige lokale Vertexpositionen für echte Mimik-Bewegungsvektoren. Der bisherige Reactive-Wert bleibt nur ergänzend.
- Prüfung: statische Aufrufer-, Mehrpass-, Shaderbinding-, Projektionsverhältnis- und Diff-Prüfungen; FOV und Dialoggesichter müssen ingame validiert werden, kein vollständiger lokaler C++-/Shader-Build.

## Build 080
- FOV/Breitbild: 100 bleibt Gothics unveränderte Originalprojektion; andere Werte skalieren horizontalen und vertikalen Sichtwinkel kontinuierlich, während horizontal zusätzlich die ausgabeauflösungsabhängige Hor+-Korrektur einfliesst.
- F11-Menü: nach einem Auflösungswechsel wird das skalierte Fenster automatisch wieder mittig positioniert.
- FSR3/Dialoge: vorhandener Dialog-Reactive-Schalter wird mit Reactive 0.30 ohne T&C im Diffuse-Pfad ausgewertet, um Gesichts-Schlieren zu reduzieren.
- FSR3/DoF: Depth of Field wieder an seine frühere Position vor dem Upscaling und in interner Renderauflösung zurückgesetzt.
- Offen: FOV auf extremen Seitenverhältnissen, F11-Zentrierung, Dialoggesichter, DoF-Flimmern und transparente Regenausschlüsse müssen ingame geprüft werden; kein vollständiger lokaler C++-/Shader-Build.

## Build 079
- FSR3/Sky: echte rotationsbasierte Motion-Vektoren nur für Sky-Depth-Pixel ergänzt, um Bewegungsschlieren ohne erneute Reactive-/T&C-Maskierung zu reduzieren.
- FSR3/DoF: Depth of Field wird bei aktivem FSR3 erst nach dem Upscaling in Ausgabeauflösung angewendet; Nicht-FSR-Pfade bleiben in ihrer bisherigen Reihenfolge.
- Bestehende Kirides-Maskenwerte und die separate FSR3-Regentropfenanpassung bleiben unverändert.
- Prüfung: statische Shaderregistrierungs-, Projektdatei-, Aufrufer-, Binding- und Rendergraph-Prüfungen; kein vollständiger lokaler C++-/Shader-Build.

## Build 078
- Korrekturpush ohne neuen Folge-Build: IceRegion-Helper für Weltmesh- und sortierte DrawWorldMesh-Schlüssel typneutral gemacht, damit Release_G1_12f wieder kompiliert.
- Inhaltlich keine weiteren Renderer-Änderungen gegenüber Build 077.
- Prüfung: CI-Fehlerstelle statisch gegen `WorldMeshKey`/`MeshKey` kontrolliert; kein vollständiger lokaler C++-Build.

## Build 078 (Folgepush)
- F11-Menü: Fenster, Bedienelemente und Text skalieren bei niedrigen Ausgabeauflösungen gemeinsam und bleiben insbesondere bei 800x600 bedienbar.
- Regen/Materialien: wirkliche Blend-Transparenz wird ohne Textur-Namensheuristik von Regentropfen, Oberflächennässe und Rain-Ground-SSR ausgeschlossen; den wirkungslosen `ICEREGION*`-/Alpha-Test-Sonderweg entfernt.
- FSR3: Welt-, Alpha-Test- und Sky-Maskierung auf das Kirides-Nightly-Verhalten zurückgesetzt; die separate FSR3-Regentropfenanpassung bleibt erhalten.
- FOV/Breitbild: `100` bleibt exakt Gothics Original; `101-120` blendet kontinuierlich bis zur vollständigen seitenverhältnisabhängigen Hor+-Korrektur, `121-130` bietet zusätzliche Weite. Vertikaler Original-FOV und Kamerahöhe bleiben erhalten.
- Offen: FSR3-Flimmern/Schlieren und die transparenten Regenausschlüsse müssen ingame validiert werden; kein vollständiger lokaler C++-/Shader-Build.

## Build 077
- Regen/Himmel: Mond-/Sonnenlicht-Schatten werden bei Regen mit ausgeblendet; Regenhimmel bleibt ohne zusätzliche Nebel-/Nachtsicht-Änderungen.
- Eisregion: Rain-/Wetness-/Rain-Ground-SSR-Effekte werden gezielt für `ICEREGION*`-Materialien blockiert; alter breiter `ICE`-/`EIS`-Heuristikfix ersetzt.
- FSR3: Sky-T&C auf 0.05 reduziert, Alpha-Test-Reactive auf Kirides-artige 0.10 gesetzt, Dialog-Reactive auf 0.30 ohne Dialog-T&C gestellt.
- FOV: Regler wieder als Hor+-Widescreen-FOV aufgebaut; `100` lässt Gothic original unangetastet, höhere Werte verbreitern das horizontale Sichtfeld.
- Frame Generation: im DX11-Build deaktiviert, weil der manuelle Pfad Optical Flow/Interpolation serialisiert und massive Framedrops verursacht.
- Offen: FSR3-Flimmern, Dialog-Schlieren, Eisregion und FOV müssen ingame final geprüft werden; vollständiger lokaler C++-/Shader-Build wurde nicht ausgeführt.

## Build 076
- Regen/Himmel: Sonne, Mond und Sterne werden bei Regen sauber ausgeblendet; bestehende Nebel-/Himmelsübergaenge bleiben unangetastet.
- Regen/Materialien: Regen-, Wetness- und Rain-Ground-SSR-Effekte auf Wasser/Eis/transparenten Flächen unterdrückt; Rain-Ground-SSR-Bewegung beruhigt.
- FSR3: Regenstärke bei aktivem FSR3 reduziert; Alpha-/Dialog-Reactive-Masken für weniger Baumflimmern und Gesichts-Krizzeln nachjustiert.
- FOV: 100 = originale Gothic-Kamera; kleinere Werte weiter, größere Werte enger, horizontal und vertikal aus der nativen Kamera skaliert.
- Frame Generation: F11-Position direkt über HDR; DXGI-Frame-Latency für manuelles DX11-FG angepasst.
- Offen: Vollständiger lokaler C++-Build wurde in dieser Umgebung nicht ausgeführt; FSR3-Flimmern und Frame Generation müssen ingame validiert werden.

## Build 075
- Grundlage: Kirides Nightly; 17.9.7 bleibt der letzte Stable-Vergleichsstand davor.
- FSR: Alpha-Test-Flimmern von Vegetation reduziert, indem stabile Tiefen-/Motion-Vector-Flächen nicht pauschal reaktiv markiert werden.
- Frame Generation: auf Flip-Swapchains begrenzt, unnötige Vollbildkopie entfernt und Present-/Pacing-Nebenpfade abgesichert.
- Kamera: `100` ist der UI-Wert Original und lässt Gothics native Projektion unverändert; andere Werte ändern nur den horizontalen FOV.
- NPC-Schatten: Codex-spezifische Aufweichung zurückgenommen.
- Regenhimmel: Sonnen-Mie-Anteil, Godrays und Sterne werden ausschließlich bei aktivem Regen ausgeblendet.
- Rain Ground SSR: `ICE`-/`EIS`-Weltmeshes blockieren nasse Bodenreflexionen; horizontale Reflexionsbewegung wurde beruhigt.
- Offene Grenze: Die vorhandene x86-DX11-FSR-Runtime bleibt 3.1.2, da das bereitgestellte offizielle 3.1.4-SDK keine ABI-kompatiblen x86-DX11-Binaries enthält.
- Prüfung: statische Diff-, Aufrufer-, Binding- und Projektdateiprüfungen; kein vollständiger lokaler C++-/Shader-Build.
- Korrekturpush: R11G11B10_FLOAT Kompatibilitäts-Fallback für HDR-Backbuffer hinzugefügt.
- Korrekturpush: XeGTAO nutzt bei FSR3 nun eine Reactive-Mask für korrekte Ghosting-Vermeidung.
- Korrekturpush: Cascade-Texel-Größe für Schatten wird nun laufzeiteffizient auf der CPU vorberechnet.
- Korrekturpush: FSR3-Preset-Bezeichnungen im F11-Menü korrigiert ("Native with AA", "Ultra Performance").
- Korrekturpush: Schatten-Berechnung (CSM) stabilisiert (Entfernung des fehleranfälligen WorldShadowCaster-Caches und Rückkehr zur robusten kaskadenbasierten Update-Logik).
- Korrekturpush: CSM-Update-Frequenz für entfernte Kaskaden über das F11-Menü konfigurierbar gemacht und Schattenberechnung am Sonnen-Zenith stabilisiert.
