# Codex Build Changelog

Kurze, append-only Dokumentation der von Codex gepushten Renderer-Builds.

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

## Build 090 (Korrekturpush)

- CI-Korrektur: VFX-Fallback-Bounding-Sphere speichert die effektive Lichtposition nun explizit per `XMStoreFloat3` in `DirectX::XMFLOAT3`; damit ist die in Release_G1_12f gemeldete ungueltige Zuweisung von `float3` behoben.
- Inhaltlich keine weiteren Renderer-Aenderungen gegenueber Build 089.
- Pruefung: gemeldete Compilerstelle, alle weiteren Aufrufer von `GetEffectivePositionWorld`, Typgrenzen und `git diff --check` statisch kontrolliert; kein vollstaendiger lokaler C++-Build.

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

## Build 092 (Regenwolken und VFX-Rollback)

- Regenwolken: Base-Projektion von einem nahezu konstanten Himmelsausschnitt auf eine klar wiederholte Dome-Projektion umgestellt; die transparente Detailbank nutzt eine dichtere, gedrehte Projektion mit schnellerer Eigenbewegung und staerkerer Parallaxe. Regennebel, Wetteruebergang, Horizont und Nachtlogik bleiben unveraendert.
- Presets: Medium nutzt im F11-Menue fuer FSR 3 nun Render Scale High Quality statt Quality.
- Fackel/VFX: den erfolglosen Indoor-/Outdoor-Transition-Fallback vollstaendig auf den stabilen Stand von Build 088 zurueckgestellt. Die kurze bekannte Schattenluecke an der Grenze wird bewusst akzeptiert; der zusaetzliche Frame-Light-Scan sowie die erweiterten Parent-/Remove-Sonderpfade sind entfernt.
- Contact Shadows/SSGI: Wasser- und Wasserfallmaske auch fuer Screen-Space-Lichteffekte erzeugt und in die Composition gereicht; Wasser wird als Empfaenger und Ray-Treffer ausgeschlossen. Contact Shadows nutzen kuerzere stabilere Rays mit begrenzter Abdunklung; SSGI nutzt stabile Richtungen, Mindesttreffer und Highlight-Kompression gegen punktfoermige Artefakte.
- Pruefung: betroffene Fackel-/VFX-Funktionen blockweise exakt mit Build 088 verglichen; Wolken-UV-Abdeckung gegen beide ausgelieferten DDS-Dateien stichprobenartig ausgewertet; Shaderstruktur, Ressourcenpfade und `git diff --check` statisch kontrolliert. Kein vollstaendiger lokaler C++-/Shader-Build.

## Build 093 (Bodenlicht und Regenwolken-Korrektur)

- Pointlights/Fackeln: gemeinsamer NdotL-Helfer fuer Deferred-, Dynamic-Shadow-, Tiled- und Forward+-Pfade ergaenzt. Nach oben gerichtete Bodenflaechen erhalten bei sehr bodennahen Lichtquellen einen kleinen lokalen Wrap-Anteil, damit eine abgelegte Handfackel den Boden beleuchtet; Reichweite und vorhandene Schattenauswertung bleiben erhalten.
- Regenwolken: zweite Detailwolken-Schicht samt Shaderbinding, Ressourcenpfad und unbenutzter `RainCloudDetail.dds` vollstaendig entfernt. Die einzelne Wolkendecke wird nun als `RainCloud.dds` ausgeliefert.
- Wolkentextur: gegenueberliegende Randbereiche der gelieferten Gothic-Wolke symmetrisch verblendet und erneut als BC1-DDS mit elf Mip-Stufen erzeugt; dadurch schliesst die wiederholte Textur ohne sichtbare harte Kachelgrenzen.
- Regennebel: Standarddichte von `0.00050` auf `0.00078` angehoben und bestehende INIs mit exakt dem alten Standardwert migriert; individuell abweichende Werte bleiben erhalten. Die Sky-Abschwaechung wurde so angepasst, dass der dichtere Nebel auch vor dem Himmel sichtbar ist, die Wolkendecke aber durchscheint.
- Occlusion Culling: Large-VOBs werden im Hauptsichtbarkeitspass nicht mehr occlusion-gecullt, damit grosse Objekte wie Baeume nicht spaet aufpoppen; Small-VOBs und Mobs laufen weiter durch den strengeren Occlusion-Pfad fuer den eigentlichen Performancegewinn.
- Contact Shadows/SSGI: Screen-Space-Licht in einen eigenen Trace- und Temporalpass ausgelagert. Contact Shadows sind wasser-/normalgefiltert und begrenzt; SSGI nutzt stabile hemisphaerische Strahlen, History-Reprojection, Depth-Validation und Neighborhood-Clamping gegen flackernde Striche und punktfoermige Artefakte.
- UserSettings.ini: Welt-/Menueeinstellungen werden fuer den Build auf sichtbare F11-Werte begrenzt; versteckte Kirides-/Advanced-Optionen und alte Gamma/Brightness-Schluessel werden beim Speichern entfernt. Display-Tuning bleibt ausschliesslich ueber `DisplayContrast` und `DisplayBrightness` erhalten.
- Pruefung: alte Base-/Detail-Referenzen und unbenutzte Detaildatei entfernt; DDS-Format, Abmessungen, Mip-Anzahl und Randkontinuitaet kontrolliert; Atmosphaeren-, Pointlight-, Tiled-, Forward+-, Composition- und Screen-Space-Lighting-Shader statisch auf Ressourcen-, Register-, Klammer- und Praeprozessor-Konsistenz geprueft; Occlusion-Flags, Rendergraph-Aufrufer und UserSettings-Lese-/Speicherpfade mit Kirides 17.9.7/Nightly abgeglichen; `git diff --check` sauber. Kein lokaler HLSL-Compiler und kein vollstaendiger lokaler C++-Build vorhanden.

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
## Build 095 (DÃ¤mmerung, Screen-Space-Licht und Regennebel)

- Sonne/Mond: Licht, Schatten, Godrays und Backlit Vegetation nutzen getrennte 15-Minuten-Uebergaenge. Mondlicht blendet morgens 04:15-04:30 aus, Sonnenlicht 04:30-04:45 ein; abends blendet Sonnenlicht 17:30-17:45 aus und Mondlicht 17:45-18:00 ein. Schattenquellen ueberlappen nicht.
- Himmel/Wasser: der Mondkoerper bleibt positionsbasiert sichtbar statt ueber das Lichtgewicht ausgeblendet zu werden. Sonnen- und Mond-Glints auf Wasser laufen positionsbasiert unabhaengig von den Licht-/Schatten-Fades weiter.
- Godrays/Backlit: Godrays folgen dem Sonnenlichtgewicht inklusive Regen-Ausblendung; Backlit Vegetation folgt den vorhandenen Sonnen-/Mond-Lichtgewichten inklusive Regen-Ausblendung.
- Contact Shadows/SSGI: SSGI sampelt breiter und verlaesslicher mit weiter gedeckelter Energie, damit mehr Flaechen subtil reagieren ohne die alten Lichtstreifen zurueckzubringen. Contact Shadows werden staerker auf Nahkontakte begrenzt und in der Ferne ausgeblendet, ohne FSR 3 selbst zu veraendern.
- Regennebel: die Geometrie-Nebelrampe bei Regen beginnt weicher ueber eine groessere Distanz, nachts wirkt der Regennebel staerker und tagsueber bei Regen leicht. Himmel-/Wolkenpfad, FSR 3, normale Regen-Transitions und trockener Nachtnebel bleiben unveraendert.
- Pruefung: Shader-Diff, Klammer-/Escape-Pruefungen, Screen-Space-Lighting-Constant-Buffer-Layouts, Zeitfenster-Samples und `git diff --check` statisch kontrolliert. Kein lokaler HLSL-Compiler und kein vollstaendiger lokaler C++-Build ausgefuehrt.

## Build 096 (Stabilitaet, HDR, Beleuchtung und Release-Lizenzen)

- Release-Lizenzen: Attributions- und Lizenztexte in einer einzigen GD3D11/Licences.txt gebuendelt; doppelte MIT-Texte, nicht ausgelieferte Komponenten und der nicht benoetigte Hinweis fuer die einzelne Regentextur entfernt. Die Paketierung entfernt zusaetzlich die separate XeGTAO-Lizenzkopie.
- Sonne/Mond: Abendlicher Wechsel auf die korrekte Zeit 19:15-19:45 verschoben; Sonnenlicht/-schatten blenden 19:15-19:30 aus, Mondlicht/-schatten 19:30-19:45 ein. Godrays und Backlit Vegetation folgen den Lichtgewichten; Wasser-Glints bleiben unabhaengig.
- Regennebel: Tages- und Nachtstaerke auf den Stand vor der letzten Verstaerkung zurueckgefuehrt; der weichere, laengere Distanzuebergang bleibt erhalten.
- Contact Shadows: Nah- und Mittelbereich mit breiteren Rays, zehn Schritten und hoeherer Wirkung deutlich sichtbarer; Fernbereich endet frueher, um Flackern nicht zu verstaerken. FSR 3 selbst bleibt unveraendert.
- Lade-/Exit-Stabilitaet: Welt-, BSP-, VobTree- und MeshManager-Zugriffe abgesichert; leere Geometrie wird ohne polys[0]-Zugriff behandelt. PostFX-/FSR-Ressourcen werden vor dem D3D-Geraet freigegeben und der optionale D3D-Debugzeiger sicher initialisiert.
- Water Effects: sichtbarer Standard und F11-Regler bleiben bei 1,0 beziehungsweise 0,0-2,0; 1,0 entspricht nun der bisherigen Wirkung von 1,4. Wasser- und Regenboden-SSR nutzen dieselbe Normalisierung.
- HDR: Luminanzhistorie startet und resettiert bei Welt-/Spielstandladen neutral auf 18-Prozent-Grau statt maximaler Startbelichtung. Helle und dunkle Adaption bleiben weich getrennt, erreichen das Ziel aber deutlich schneller.
- Pruefung: gezielte Git-Diffs, Projekt-/Shaderregistrierungen, Lade-/Ressourcenlebenszyklen, Feature-Gates, Definitionen, Konfliktmarker und git diff --check statisch kontrolliert. Kein vollstaendiger lokaler C++-/Shader-Build.

## Build 097 (VDB-Feuer und -Wolken, Graslicht und Contact Shadows)

- VDB-Feuer: FIRE_HOT.PFX und FIRE.PFX werden durch kompakte animierte Atlas-Flammen fuer Lagerfeuer beziehungsweise Kamine ersetzt; FIRE_MEDIUM.PFX und TORCH.PFX bleiben unveraendert. FIRE_COMPLETE_A0.TGA wird nur im Nahbereich eines FIRE.PFX-Kaminfeuers unterdrueckt; bei fehlendem Atlas bleibt das originale Partikelfeuer als Fallback erhalten.
- VDB-Wolken: zehn gepackte Volumenwolkenvarianten als zwei animierte Atmosphaerenschichten fuer Tages-, Nacht- und Regenstimmung integriert. Die bestehende Regenwolkendecke bleibt erhalten; es gibt keinen zusaetzlichen Vollbild- oder Schattenpass.
- Backlit Vegetation: Visualnamen mit GRASSGROUP erhalten bei aktivierter Funktion identische Vorder- und Rueckseitenbeleuchtung ohne zusaetzlichen Backlit-Aufschlag. Der F11-Regler steuert weiterhin nur die Backlit-Staerke der uebrigen Vegetation.
- Contact Shadows: NPC-Pixel werden gezielt erkannt und nutzen kuerzere, weichere und schwaechere Contact-Rays gegen harte Gesichts- und Halsartefakte. Weltgeometrie behaelt die bisherige Wirkung.
- FSR 3: nur das Contact-Shadow-Raymuster und dessen temporale Alpha-Historie wurden stabilisiert, um insbesondere fernes Flackern zu reduzieren; Upscaling, Schaerfung, SSGI, Wasser und sonstige FSR-3-Pfade bleiben unveraendert.
- Lizenzen/Paket: erforderlicher JangaFX-CC0-Hinweis kompakt in GD3D11/Licences.txt ergaenzt; ausgeliefert werden nur die beiden komprimierten Laufzeitatlanten.
- Pruefung: Ressourcenpfade und Paketaufnahme, Atlasabmessungen/-inhalt, PFX-Zuordnung und Fallbacks, GBuffer-Marker und alle Beleuchtungsdecoder, Rendergraph-/Shaderregister-/Constant-Buffer-Bindings, Klammer-/Praeprozessorpaare und git diff --check statisch kontrolliert. Kein vollstaendiger lokaler C++-/Shader-Build.

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
## Build 099 (Clouds, VDB-Feuer, Backlit-Menue und Contact-Shadow-Stabilisierung)

- VDB-Wolken: die Atlaswolken wurden von uebergrossen Flaechen auf mehrere kleinere lokale Schichten umgestellt. Die Randmaske ist weich, die Kerne bleiben dichter, Tageshimmel und Sternenhimmel bleiben zwischen den Wolken sichtbar; Nacht- und Regenfaerbung bleiben erhalten.
- VDB-Feuer: FIRE_HOT.PFX fuer Lagerfeuer wird robuster auch bei Pfad-/Quote-/Namenszusatz erkannt. FIRE.PFX und FIRE_HOT.PFX nutzen globale fraktionale Atlas-Animation mit Frame-Interpolation und vertikaler VDB-Orientierungskorrektur gegen Stocken und falsche Ausrichtung; FIRE_MEDIUM und TORCH bleiben unveraendert.
- Backlit Vegetation: der F11-Eintrag steht direkt ueber Enable Rain. Die zweitseitige Beleuchtung matcht die genannten Vegetationsnamen als Wortstamm/Prefix, etwa NW_NATURE_GRASSGROUP_01, OW_NATURE_BUSH_02*, OW_NATURE_BUSH_03*, NW_NATURE_PLANT_03*, NW_KORN* und OW_GRASS_WINTER*.
- Contact Shadows: das pixelgebundene Contact-Jitter wurde im Contact-Shadow-Pfad entfernt, der ferne Weltgeometrie-Bereich frueher ausgeblendet und die Contact-Alpha-Maske normal-/tiefenbewusst weich gefiltert. SSGI-Sampling und FSR-3-Pfad bleiben unveraendert.
- Pruefung: AGENTS-Regeln gelesen; gezielte Status-/Diff-/Shaderstellenpruefungen, BOM-/Zeilenumbruchkontrolle, literal eingefuegte Escape-Sequenzen und git diff --check statisch kontrolliert. Kein vollstaendiger lokaler C++-/Shader-Build.

## Build 100 (Rueckbau VDB, Contact Shadows, Groundfog und Schattenstabilitaet)

- VDB-Rueckbau: VDB-Wolken und VDB-Feuer vollstaendig aus Runtime-Code, Shadern, Paketdateien und Lizenzhinweisen entfernt. FIRE.PFX, FIRE_HOT.PFX und FIRE_COMPLETE_A0.TGA laufen wieder ueber die normalen Gothic-Partikel-/Decalpfade.
- Contact Shadows: der lange richtungsbasierte Screen-Space-Trace wurde durch eine kurze, weiche und deterministische Kontaktabdunklung ersetzt, damit die unterbrochenen Muster, Blickwinkel-Spruenge und FSR-3-Flackerstellen reduziert werden.
- Backlit Vegetation: NW_NATURE_WATERGRASS_56P als weitere Prefix-Ausnahme fuer identische Vorder-/Rueckseitenbeleuchtung ergaenzt.
- Schatten um 12 Uhr: die Shadow-Kamera-Up-Richtung wird nahe Zenith frueher stabilisiert, ohne Sonne-/Mondzeiten oder Lichtuebergaenge zu veraendern.
- Groundfog-PFX: GROUNDFOG-Partikel werden nicht mehr durch die zu harte PFX-BBox-Frustum-Pruefung verworfen; Draw-Radius und showVisual bleiben erhalten.
- F11/Starttext: Startanzeige weist zusaetzlich auf F11 fuer Grafikeinstellungen hin; das F11-Fenster zeigt keine Versionsnummer mehr im Titel.
- Pruefung: AGENTS-Regeln gelesen; gezielte VDB-Symbol-/Asset-/Lizenzpruefungen, Shaderregister-/Escape-Sequenzpruefung, relevante Git-Diffs und git diff --check statisch kontrolliert. Kein vollstaendiger lokaler C++-/Shader-Build.

## Build 101 (MSM-Testfilter, Shadow-/PFX-Fixes und Velocity-Fallback)

- Schattenfilter: MSM als testbarer Shadow-Filter-Modus ergaenzt und im F11-Menue als eigener Enabler eingebunden; Standard bleibt der bisherige einfache Schattenfilter.
- Contact Shadows: sichtbare raybasierte Contact-Shadow-Wirkung aus Build 099 wiederhergestellt und mit deterministischen Mehrfachrays stabilisiert, damit harte Unterbrechungen reduziert werden ohne die Funktion praktisch auszublenden.
- Groundfog-PFX: GROUNDFOG- und nebelartige additive PFX werden breiter erkannt, nicht durch die normale Distanz-/Frustum-Logik verworfen und bleiben ohne unpassende Partikelbeleuchtung sichtbar.
- 12-Uhr-Schatten: Shadow-Kamera-Pullback und Cascade-Update nahe Zenith stabilisiert, damit die Sonnen-Schattenmap um Punkt 12 Uhr nicht kurz ausfaellt.
- TAA/Velocity: der Depth-Motion-Vector-Fallback nutzt nun dieselbe Reprojektionskonvention wie Geometry- und Sky-Velocities (`previousUV - currentUV`). Der normale MRT-Velocity-/FSR3-Pfad bleibt unveraendert.
- Pruefung: AGENTS-Regeln gelesen; Buildnummer aus outputs ermittelt; gezielte Shader-/Konventionspruefung, Diffkontrolle, BOM-/Escape-Sequenzpruefung und git diff --check statisch kontrolliert. Kein vollstaendiger lokaler C++-/Shader-Build.

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
## Build 103 (MSM-, Noon-, Partikel- und Ressourcen-Korrekturen)

- MSM: Moment-Mip-Footprints an den tatsaechlichen Filterdurchmesser gekoppelt, damit das sichtbare Shadowmap-Texelraster bei weichen Schatten reduziert wird.
- 12-Uhr-Weltschatten: Kaskaden wieder aus den echten Kamera-Frustum-Slices mit stabilem Texel-Snapping und dynamischen Tiefengrenzen aufgebaut; Weltgeometrie wird am exakten Zenith zweiseitig in die Shadowmap geschrieben.
- Partikel und Backlit: die normale Partikelbeleuchtung einschliesslich bestehender Tag-/Nachtabdunklung gilt wieder fuer Groundfog; die benannten Vegetationsausnahmen verwenden wieder die funktionierende identische Vorder-/Rueckseitenbeleuchtung.
- Ressourcen: optimierte RainCloud.dds uebernommen, ungenutzte VDB-Atlasdaten aus dem Partikel-Instanzlayout entfernt (60 statt 72 Byte) und den leeren fuenften Sky-SRV-Slot entfernt.
- Offene Ingame-Grenzen: MSM-Bildruhe, 12-Uhr-Weltschatten, Groundfog-Nachtabdunklung und Backlit-Ausnahmen muessen im Spiel bestaetigt werden.
- Pruefung: DDS-Format, Abmessungen, Mip-Anzahl und Quellhash kontrolliert; CPU-/Shader-Partikellayout, Sky-Bindings, betroffene Shaderpfade und git diff --check statisch geprueft. Kein vollstaendiger lokaler C++-/Shader-Build.
## Build 104 (MSM-, Noon-, Backlit- und Preset-Korrekturen)

- MSM: Moment-Sampling auf direkte 4-Moment-Auswertung ohne grobe Moment-Mips/Blocker-Mip-Suche zurueckgefuehrt, damit der Filter weniger pixelig und leichter wird.
- 12-Uhr-Weltschatten: wirkungslosen World-Mesh-Culling-Sonderfall entfernt und die Shadow-Kamera/Kaskadenprojektion am Zenith stabilisiert, ohne sichtbare Sonnen-/Mondbeleuchtung umzubiegen.
- Backlit Vegetation: gemeinsamer Transmissionspfad fuer Sonne/Mond, Forward+/Deferred-/Tiled-Pointlights und dynamisch schattierte Pointlights; benannte Ausnahmen nutzen dieselbe konturbetonte Durchleuchtung statt einfacher Rueckseitenaufhellung.
- F11/Presets: Standard bleibt SSGI aus und Contact Shadows an. Presets setzen Low beide aus, Medium/High Contact Shadows an und SSGI aus, Extreme beide an.
- Pruefung: AGENTS-Regeln gelesen; Defaultwerte, Presetzuordnung, Shader-/Aufruferpfade, betroffene Diffs und git diff --check statisch kontrolliert. Kein vollstaendiger lokaler C++-/Shader-Build.
## Build 105 (MSM-Hard-Softness, Backlit-Gate und Wind-Motion)

- MSM: bei Shadow Softness ganz links wird kein Momentfilter und keine Moment-Erzeugung mehr genutzt; der Pfad faellt auf direkten Shadow-Compare zurueck, damit harte Schatten Simple-naeher und guenstiger bleiben.
- Backlit Vegetation: Sonnen-/Mondlicht, Forward+, Deferred-, Tiled- und dynamisch schattierte Pointlights koppeln die Rueckseiten-/Ausnahme-Durchleuchtung vollstaendig an den F11-Schalter Backlit Vegetation.
- FSR3/Wind: instanzierte Vegetation schreibt Motion Vectors mit der vorherigen Windphase, damit starker Regenwind weniger History-Schlieren erzeugt.
- 12-Uhr-Weltschatten: Weltgeometrie im Sonnen-Shadowpass wird nicht mehr zusaetzlich gegen das Kaskaden-Frustum verworfen; VOB-/NPC-Culling bleibt unveraendert.
- Pruefung: AGENTS-Regeln gelesen; gezielte MSM-, Backlit-, Wind-CBuffer-, Weltmesh-Shadow-Culling- und Aufruferpruefungen sowie git diff --check statisch kontrolliert. Kein vollstaendiger lokaler C++-/Shader-Build.
## Build 106 (MSM, Backlit Vegetation, Contact Shadows und Oillamp-Schatten)

- MSM: der Filter bleibt auch bei Shadow Softness ganz links im MSM-Pfad aktiv und nutzt dort einen sehr kleinen Momenten-Footprint statt eines Hard-/Simple-Fallbacks; hoehere Softness-Werte skalieren den Momentenfilter staerker.
- Backlit Vegetation: Rueckseiten-Transmission ignoriert normale Schattenstreifen nur auf der Rueckseite, Vorderseiten-Schatten bleiben normal. Die allgemeine Vegetationsmaske wurde breiter gefasst, damit Baumlaub wieder sichtbarer auf dem Niveau der aelteren Backlit-Wirkung bleibt, aber mit reduzierter interner Staerke 0,5.
- Contact Shadows und FSR3: Contact-History ist von der Farb-History getrennt und bleibt auch bei FSR3 Native AA stabiler, nicht nur bei Render Scale unter 100 Prozent.
- Indoor-Pointlights: NW_CITY_OILLAMP_01.3DS-Oillamps duerfen analog zu Flammen je das naechste statische und dynamische Licht bis 150 Einheiten als schattenberechtigt verankern; der Schattenursprung wird auf den Oillamp-Mittelpunkt plus 50 Hoeheneinheiten gezogen.
- Offene Ingame-Grenzen: Oillamp-Lichtzuordnung, MSM-Bildruhe bei niedriger Softness, Backlit-Wirkung auf normale Baeume und FSR3-Contact-Stabilitaet sollten im Spiel final gegengeprueft werden.
- Pruefung: Buildnummer aus outputs ermittelt; AGENTS-Regeln gelesen; gezielte Status-, Diff-, Shader-/Binding-, Oillamp-Anker- und git diff --check-Pruefungen statisch kontrolliert. Kein vollstaendiger lokaler C++-/Shader-Build.


## Build 108 (Vegetation-Schatten, Contact Shadows und Versionsanzeige)

- Vegetation: die Schattenbehandlung ist auf Vegetations-Receiver eingegrenzt; die stoerenden gestreiften Artefakte auf vertikaler Alpha-Vegetation werden nur dort abgefangen, nicht auf allgemeinen Oberflaechen.
- Contact Shadows: FSR3 nutzt fest intern `0.35`, ohne FSR3 fest `0.50`; der F11-Staerkeregler ist entfernt, der Schalter bleibt als einfacher Enable-Eintrag.
- F11/Version: die Versionsanzeige steht oben rechts auf Preset-Hoehe, und die oeffentliche Version ist fest auf `Version 18.0` ohne Git-/Datumszusatz gesetzt.
- Build-Pipeline: die Nightly-Build-Konfiguration gibt `18.0` als feste Versionsnummer weiter, damit der veroeffentlichte Stand stabil bleibt.
- Pruefung: nur statische Diff-/Binding-/Aufrufer- und Shaderpfad-Kontrollen; kein lokaler Vollbau.

## Build 109

- Vegetation: die gestreiften Schattenartefakte auf vertikaler Alpha-Vegetation werden nur auf rueckwaertigen Vegetationsflaechen abgefangen; allgemeine Schatten auf anderen Oberflaechen bleiben erhalten.
- Contact Shadows: die feste interne Staerke bleibt bei `0.35` fuer FSR3 und TAA, sonst bei `0.50`; der F11-Regler fuer Contact Shadows bleibt entfernt und die Option ist nur noch ein einfacher Enabler.
- F11: Tooltiptexte wurden kurz und allgemein formuliert; Displaymodus und Versionsanzeige bleiben auf den vereinbarten sichtbaren Positionen.
- EVSM: echter EVSM4-Filterpfad mit mehrstufigen Kaskaden und qualitativer Aufloesungsstaffelung je nach Shadow-Quality weiter integriert.
- Pruefung: nur statische Diff-/Binding-/Aufrufer- und Shaderpfadkontrollen; kein lokaler Vollbau.
