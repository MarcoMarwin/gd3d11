# GD3D11 (Gothic Direct3D 11) Renderer

GD3D11 replaces the original DirectDraw-based rendering path of Gothic and Gothic II with a Direct3D 11 renderer. The project is developed by multiple authors and contributors from the Gothic community.

## Main features

- Direct3D 11 rendering for Gothic and Gothic II
- Dynamic lighting and cascaded shadows
- Extended object and world draw distances
- SMAA and FidelityFX Super Resolution 3 anti-aliasing with resolution scaling
- XeGTAO ambient occlusion and screen-space water reflections
- Depth of field, HDR tone mapping, and god rays
- Improved water, rain, fog, dynamic clouds, and atmospheric rendering
- Normal maps and parallax surface detail
- In-game renderer configuration through the F11 menu

## Supported games

- Gothic 1.08k
- Gothic 1.12f
- Gothic II: Night of the Raven 2.6

The required 32-bit Microsoft Visual C++ runtime must be installed.

## Installation

1. Download the current GD3D11 release archive from the Releases page of this repository.
2. Extract the archive into the system directory of Gothic or Gothic II.
3. Start the game and press F11 to open the renderer menu.
4. Select Save Settings to store the global configuration in system\GD3D11\UserSettings.ini.

Renderer settings apply globally. Resolution, display mode, renderer language, presets, and other F11 options are not stored per world.

## Per-world environment overrides

ZENResources is an optional runtime configuration directory, not a separate
renderer module or a bundled resource package. It is not created by the release
archive; create it manually when a mod needs world-specific environment values.
The renderer reads only the following sections from these files: Fog,
Atmosphere, and Rain.

For the original game:

    system\GD3D11\ZENResources\<WORLD>.INI

For a mod started with -game:MODNAME.INI, where MODNAME is the filename
without its .INI extension:

    system\GD3D11\ZENResources\<MODNAME>\<WORLD>.INI

The world filename is used without its path and .ZEN extension. These
overrides are loaded after the global system\GD3D11\UserSettings.ini
settings, so they affect only the matching world.

Example:

    [Fog]
    Height=800
    HeightFalloff=0.0005
    GlobalDensity=0.00004

    [Atmosphere]
    SunLightColor=255,240,220
    FogColorMod=180,180,255
    ReplaceSunDirection=0
    LightDirection=1,-1,0

    [Rain]
    RadiusRange=5000
    HeightRange=1000
    NumParticles=45000
    GlobalVelocity=250,-1000,0
    SceneWettness=0
    SunLightStrength=0.5
    FogColor=71,71,71
    FogDensity=0.00078

## Building

The repository contains Visual Studio project files and CMake presets. The
packaged Windows builds are produced by the GitHub workflow with MSBuild. The
current Windows toolchain is:

- Visual Studio 2026 toolset v145
- Windows 10 SDK
- C++20
- Win32 (32-bit/x86) target architecture

CMake 3.24 or newer can be used for local builds. Configure and build the
default Gothic II release with:

    cmake --preset Release
    cmake --build --preset Release

Additional presets are available for Gothic 1, Gothic 1 1.12f, AVX/AVX2,
Release_NoOpt, Spacer.NET, and Clang. The supported game, SIMD, compiler, and
profile options are defined in CMakeLists.txt and CMakePresets.json.

## Main dependencies

- Microsoft Detours
- DirectXMath and DirectXTK
- Dear ImGui
- AMD FidelityFX
- Intel XeGTAO
- Assimp
- meshoptimizer
- RapidJSON
- SMAA

## Authors

GD3D11 was created and developed across multiple versions by:

- Degenerated/Ataulien
- Bonne6
- Kirides
- SaiyansKing
- Marco Marwin

Additional code, testing, documentation, bug reports, and feedback have been contributed by the wider Gothic community.

## License

GD3D11 is distributed under the GNU General Public License version 3. See [LICENSE](LICENSE).

Third-party copyright and license notices are collected in [blobs/Licences.txt](blobs/Licences.txt).
