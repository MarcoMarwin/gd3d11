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
