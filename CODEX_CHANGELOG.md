# Codex Build Changelog

Kurze, append-only Dokumentation der von Codex gepushten Renderer-Builds.

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

## Build 076
- Regen/Himmel: Sonne, Mond und Sterne werden bei Regen sauber ausgeblendet; bestehende Nebel-/Himmelsuebergaenge bleiben unangetastet.
- Regen/Materialien: Regen-, Wetness- und Rain-Ground-SSR-Effekte auf Wasser/Eis/transparenten Flaechen unterdrueckt; Rain-Ground-SSR-Bewegung beruhigt.
- FSR3: Regenstaerke bei aktivem FSR3 reduziert; Alpha-/Dialog-Reactive-Masken fuer weniger Baumflimmern und Gesichts-Krizzeln nachjustiert.
- FOV: 100 = originale Gothic-Kamera; kleinere Werte weiter, groessere Werte enger, horizontal und vertikal aus der nativen Kamera skaliert.
- Frame Generation: F11-Position direkt ueber HDR; DXGI-Frame-Latency fuer manuelles DX11-FG angepasst.
- Offen: Vollstaendiger lokaler C++-Build wurde in dieser Umgebung nicht ausgefuehrt; FSR3-Flimmern und Frame Generation muessen ingame validiert werden.

## Build 077
- Regen/Himmel: Mond-/Sonnenlicht-Schatten werden bei Regen mit ausgeblendet; Regenhimmel bleibt ohne zusaetzliche Nebel-/Nachtsicht-Aenderungen.
- Eisregion: Rain-/Wetness-/Rain-Ground-SSR-Effekte werden gezielt fuer `ICEREGION*`-Materialien blockiert; alter breiter `ICE`-/`EIS`-Heuristikfix ersetzt.
- FSR3: Sky-T&C auf 0.05 reduziert, Alpha-Test-Reactive auf Kirides-artige 0.10 gesetzt, Dialog-Reactive auf 0.30 ohne Dialog-T&C gestellt.
- FOV: Regler wieder als Hor+-Widescreen-FOV aufgebaut; `100` laesst Gothic original unangetastet, hoehere Werte verbreitern das horizontale Sichtfeld.
- Frame Generation: im DX11-Build deaktiviert, weil der manuelle Pfad Optical Flow/Interpolation serialisiert und massive Framedrops verursacht.
- Offen: FSR3-Flimmern, Dialog-Schlieren, Eisregion und FOV muessen ingame final geprueft werden; vollstaendiger lokaler C++-/Shader-Build wurde nicht ausgefuehrt.

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

## Build 079
- FSR3/Sky: echte rotationsbasierte Motion-Vektoren nur fuer Sky-Depth-Pixel ergaenzt, um Bewegungsschlieren ohne erneute Reactive-/T&C-Maskierung zu reduzieren.
- FSR3/DoF: Depth of Field wird bei aktivem FSR3 erst nach dem Upscaling in Ausgabeaufloesung angewendet; Nicht-FSR-Pfade bleiben in ihrer bisherigen Reihenfolge.
- Bestehende Kirides-Maskenwerte und die separate FSR3-Regentropfenanpassung bleiben unveraendert.
- Pruefung: statische Shaderregistrierungs-, Projektdatei-, Aufrufer-, Binding- und Rendergraph-Pruefungen; kein vollstaendiger lokaler C++-/Shader-Build.
## Build 080
- FOV/Breitbild: 100 bleibt Gothics unveraenderte Originalprojektion; andere Werte skalieren horizontalen und vertikalen Sichtwinkel kontinuierlich, waehrend horizontal zusaetzlich die ausgabeaufloesungsabhaengige Hor+-Korrektur einfliesst.
- F11-Menue: nach einem Aufloesungswechsel wird das skalierte Fenster automatisch wieder mittig positioniert.
- FSR3/Dialoge: vorhandener Dialog-Reactive-Schalter wird mit Reactive 0.30 ohne T&C im Diffuse-Pfad ausgewertet, um Gesichts-Schlieren zu reduzieren.
- FSR3/DoF: Depth of Field wieder an seine fruehere Position vor dem Upscaling und in interner Renderaufloesung zurueckgesetzt.
- Offen: FOV auf extremen Seitenverhaeltnissen, F11-Zentrierung, Dialoggesichter, DoF-Flimmern und transparente Regenausschluesse muessen ingame geprueft werden; kein vollstaendiger lokaler C++-/Shader-Build.
## Build 081
- FOV/Weitwinkel: 100 bleibt exakt Gothics Originalprojektion; hoehere Werte vergroessern horizontalen und vertikalen Projektionswinkel mit identischem Tangensfaktor, sodass keine achsenabhaengige Stauchung oder Dehnung entsteht.
- F11-Menue: Einstellungsfenster bleibt konsequent mittig; FOV-Hilfetext und Endbezeichnung wurden kurz und wirkungsbezogen formuliert.
- FSR3/Dialoggesichter: Kopfaufsatz verwendet vorherige Kopfknochenmatrix; Morph-Meshes liefern vorherige lokale Vertexpositionen fuer echte Mimik-Bewegungsvektoren. Der bisherige Reactive-Wert bleibt nur ergaenzend.
- Pruefung: statische Aufrufer-, Mehrpass-, Shaderbinding-, Projektionsverhaeltnis- und Diff-Pruefungen; FOV und Dialoggesichter muessen ingame validiert werden, kein vollstaendiger lokaler C++-/Shader-Build.
## Build 082
- FOV: normale F11-FOV-Funktion und Runtime-Override entfernt; alte INI-FOV-Werte werden geloescht beziehungsweise ignoriert, Gothic bleibt bei der nativen Kamera/Projektion.
- F11-Menue: Fenster zentriert beim Oeffnen und nach Aufloesungswechsel, bleibt waehrend der Sitzung aber wieder per Titelleiste verschiebbar.
- DoF: Schaerfe-/Unschaerfeuebergaenge nutzen kameraradiale Tiefe statt nur View-Z, damit der Fokus beim Drehen stabiler bleibt.
- HDR: kurze Tone-Mapping-Auswahl `Legacy`/`LPM` im F11-Menue ergaenzt; alte interne HDRToneMap-Werte werden auf Legacy normalisiert.
- Pruefung: statische Diff-, Escape-, Shaderpfad- und UI-Pruefungen; kein vollstaendiger lokaler C++-/Shader-Build.

## Build 083

- Frame Generation: den unbrauchbaren DX11-FG-/Optical-Flow-Pfad samt exklusiven Quellen entfernt; FSR 3.1.2 bleibt unveraendert als Upscaler erhalten.
- HDR: Legacy und LPM neu abgestimmt, ungenutzte Tone-Mapping-Modi entfernt und die kompakte Auswahl neben HDR im Display-Bereich angeordnet.
- F11-Menue: nach Oeffnen und Aufloesungswechsel verlaesslich zentriert, danach weiterhin per Titelleiste verschiebbar.
- Pointlight-Schatten: Dynamic/Full-Verhalten des urspruenglichen Kirides-Nightlys wiederhergestellt; Schatten nur fuer dauerhafte VOB-Lichter oder vorhandene parentlose Lichter nahe erkannter Flammen-PFX/-TGA.
- Kaskadenschatten: jitterstabile Tiefenrekonstruktion fuer TAA/FSR sowie lit Border-Sampling und Z-Grenzpruefung gegen Flimmern und dunkle Kaskadenraender.
- Pruefung: statische Diff-, Aufrufer-, Constant-Buffer-, Shaderbinding-, Ressourcen- und Projektdateipruefungen; kein vollstaendiger lokaler C++-/Shader-Build.

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

## Build 087

- F11/Presets: Pointlight-Shadow- und Shadow-Filtering-Auswahl entfernt; Pointlight-Schatten laufen intern dauerhaft dynamisch, Presets setzen diese Modi nicht mehr.
- Presets/UI: Low nutzt FSR Balanced, Ambient Occlusion, Backlit Vegetation, Wind und Characters-affect-objects; VSync bleibt standardmaessig aus und preset-unabhaengig; Object-Draw-Distance auf 1-10 skaliert.
- HDR/LPM: Auto-Exposure enger begrenzt, damit LPM weniger stark nach oben beziehungsweise unten regelt.
- Wasser: prozeduraler Sonnen-Glanz wird nur dort gedaempft, wo Wasser-SSR bereits valide Szenengeometrie reflektiert; SSR selbst bleibt unveraendert.
- Pointlight-Schatten: schattenberechtigte sichtbare Lichter im aktiven Radius werden im Dynamic-Pfad pro Frame aktualisiert; nicht berechtigte Atmolichter bleiben ausgeschlossen.
- Pruefung: statische UI-/INI-/Shader-Aufrufer-, Escape- und git diff --check-Pruefungen; kein vollstaendiger lokaler C++-/Shader-Build.

## Build 088

- F11/Presets: Presets auf Low/Medium/High/Extreme bereinigt; Shadow Quality steuert Welt- und Pointlight-Schatten gemeinsam, Reset-to-Defaults sowie Pointlight-/World-Shadow-/Shadow-Filtering-Auswahl entfernt.
- Schatten: Pointlight-Schatten laufen fest dynamisch; Simple-PCF bleibt Standard, PCSS-Codepfad entfernt; sichtbare berechtigte dynamische Lichter aktualisieren ihre Schatten pro Frame.
- HDR/LPM: HDR Tone Mapping wieder links bei Contrast/Brightness mit Enabler und Strength-Slider 1-10; LPM-Staerke und Bloom-Anteil folgen dem Slider, Auto-Exposure bleibt enger begrenzt.
- Preset-Erkennung: VSync, FPS-Limit, HDR, Display, Helligkeit und Kontrast sind preset-unabhaengig; zurueckgestellte Preset-Werte werden wieder als passendes Preset erkannt.
- Licht/SSR: unberechtigte Atmolichter gedaempft; Wasser-Sonnen-Glanz stabiler gegen SSR-Geometrie verdeckt, ohne den SSR-Pfad selbst umzubauen.
- Pruefung: statische UI-/INI-/Shader-/Constant-Buffer-/Aufrufer-, Escape- und git diff --check-Pruefungen; kein vollstaendiger lokaler C++-/Shader-Build.

## Build 089

- F11/Presets: `Custom` aus der Auswahlliste entfernt, bleibt als erkannter Zustand sichtbar; Presets steuern nur sichtbare F11-Werte, waehrend sichtbare AA-Modi ihre normale CAS-Folge anwenden duerfen.
- F11-Effektregler: einheitliche 0-2-Stufenskalen mit gekoppelten Enablern, Wiederherstellung des letzten aktiven Werts und dezenter inaktiver Skala; HDR neu normiert (`1.0` entspricht dem bisherigen Wert `7.5`, `2.0` entspricht `15`).
- Screen-Space-Licht: Contact Shadows und Screen-Space GI getrennt, mit eigenen Enablern, Reglern und kurzen Tooltips; High und Extreme aktivieren beide Effekte.
- Schatten/Lichter: Extreme-Pointlight-Schatten auf 256 gesetzt; VFX-Lichter bleiben am Indoor-/Outdoor-Uebergang samt Bodenschatten gesammelt, ohne den funktionierenden Handfackelpfad oder atmosphaerische Schattenregeln umzubauen.
- Himmel/Wetter: Unterseite des Taghimmels hart schwarz; neue mehrschichtige Regenwolkendecke, angepasste Backlit-Vegetation und sichtbare Wolken trotz Regennebel; Regenbeginn und monotones Aufklaren dauern jeweils mindestens 60 Sekunden und ignorieren auslaufende Controllerpulse.
- Wasser: terrainverdeckte Sonnenreflexion stabilisiert, passende Mondreflexion ergaenzt und beide bei Regenwolken ausgeblendet; gemeinsamer optisch gleichwertiger Occlusion-Trace reduziert doppelten Shadercode.
- Composition: Heightfog nur outdoor angewendet, auch wenn Contact Shadows oder SSGI indoor aktiv bleiben; Nutzerfassung von `starsh.dds` unveraendert uebernommen.
- Pruefung: Atmosphaeren-, Wasser-, HDR- und Composition-Shader sowie mehrere Composition-Makrovarianten erfolgreich mit DXC kompiliert; statische Aufrufer-, Binding-, Ressourcen-, Escape- und `git diff --check`-Pruefungen; kein vollstaendiger lokaler C++-Build.