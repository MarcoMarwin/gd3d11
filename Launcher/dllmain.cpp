#include <windows.h>
#include <intrin.h>
#include <shlwapi.h>
#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <string>

#pragma comment(lib, "shlwapi.lib")

enum ExecutableKind {
    GOTHIC1_EXECUTABLE,
    GOTHIC1A_EXECUTABLE,
    GOTHIC2A_EXECUTABLE,
    GOTHIC1_SPACERNET,
    GOTHIC2_SPACERNET,
    INVALID_EXECUTABLE = -1
};

const wchar_t* GetRendererBinaryStem( int executable ) noexcept {
    switch ( executable ) {
    case GOTHIC1_EXECUTABLE: return L"\\GD3D11\\bin\\g1";
    case GOTHIC1A_EXECUTABLE: return L"\\GD3D11\\bin\\g1a";
    case GOTHIC2A_EXECUTABLE: return L"\\GD3D11\\bin\\g2a";
    case GOTHIC1_SPACERNET: return L"\\GD3D11\\bin\\g1_spacer";
    case GOTHIC2_SPACERNET: return L"\\GD3D11\\bin\\g2_spacer";
    default: return nullptr;
    }
}

struct ddraw_dll {
    HMODULE dll = NULL;
    FARPROC	DirectDrawCreateEx;
    FARPROC	AcquireDDThreadLock;
    FARPROC	CheckFullscreen;
    FARPROC	CompleteCreateSysmemSurface;
    FARPROC	D3DParseUnknownCommand;
    FARPROC	DDGetAttachedSurfaceLcl;
    FARPROC	DDInternalLock;
    FARPROC	DDInternalUnlock;
    FARPROC	DSoundHelp;
    FARPROC	DirectDrawCreate;
    FARPROC	DirectDrawCreateClipper;
    FARPROC	DirectDrawEnumerateA;
    FARPROC	DirectDrawEnumerateExA;
    FARPROC	DirectDrawEnumerateExW;
    FARPROC	DirectDrawEnumerateW;
    FARPROC	DllCanUnloadNow;
    FARPROC	DllGetClassObject;
    FARPROC	GetDDSurfaceLocal;
    FARPROC	GetOLEThunkData;
    FARPROC	GetSurfaceFromDC;
    FARPROC	RegisterSpecialCase;
    FARPROC	ReleaseDDThreadLock;
    FARPROC	GDX_AddPointLocator;
    FARPROC	GDX_SetFogColor;
    FARPROC	GDX_SetFogDensity;
    FARPROC	GDX_SetFogHeight;
    FARPROC	GDX_SetFogHeightFalloff;
    FARPROC	GDX_SetSunColor;
    FARPROC	GDX_SetSunStrength;
    FARPROC	GDX_SetShadowStrength;
    FARPROC	GDX_SetShadowAOStrength;
    FARPROC	GDX_SetWorldAOStrength;
    FARPROC	GDX_OpenMessageBox;
    FARPROC	GDX_SetBinkVideoRunning;
    FARPROC	GDX_SetAtmosphericScattering;
    FARPROC	GDX_SetFogRange;
    FARPROC	GDX_SetGlobalWindStrength;
    FARPROC	GDX_SetRainRadiusRange;
    FARPROC	GDX_SetRainHeightRange;
    FARPROC	GDX_SetRainSceneWettness;
    FARPROC	GDX_SetRainFogDensity;
    FARPROC	GDX_SetRainSunLightStrength;
    FARPROC	GDX_SetRainNumParticles;
    FARPROC	GDX_SetRainGlobalVelocity;
    FARPROC	GDX_SetRainFogColor;
    FARPROC	GDX_SetRainMoveParticles;
    FARPROC	GDX_SetRainUseInitialSet;
    FARPROC	GDX_GetDX11RenderingContext;
    FARPROC	GDX_GetVersionString;
    FARPROC	GDX_GetRendererSettings;
    FARPROC	GDX_SaveRendererSettings;    
    FARPROC	UpdateCustomFontMultiplier;
    FARPROC	SetCustomSkyTexture;
    FARPROC	SetCustomSkyTexture_ZenGin;
    FARPROC	SetCustomSkyWavelengths;
    FARPROC	LoadMenuSettings;
    FARPROC	LoadCustomZENResources;
    FARPROC	EnableWindAnimations;
    FARPROC imgui_begin;
    FARPROC imgui_begin_overlay;
    FARPROC imgui_end;
    FARPROC imgui_text;
    FARPROC imgui_text_unformatted;
    FARPROC imgui_button;
    FARPROC imgui_checkbox;
    FARPROC imgui_slider_float;
    FARPROC imgui_input_text;
    FARPROC imgui_same_line;
    FARPROC imgui_new_line;
    FARPROC imgui_separator;
    FARPROC imgui_begin_child;
    FARPROC imgui_end_child;
    FARPROC imgui_collapsing_header;
    FARPROC imgui_begin_main_menu_bar;
    FARPROC imgui_end_main_menu_bar;
    FARPROC imgui_begin_menu;
    FARPROC imgui_end_menu;
    FARPROC imgui_menu_item;
    FARPROC imgui_push_id;
    FARPROC imgui_pop_id;
    FARPROC imgui_is_ready;
    FARPROC imgui_set_next_window_pos;
    FARPROC imgui_set_next_window_size;
    FARPROC imgui_set_item_tooltip;
    FARPROC imgui_set_next_window_bg_alpha;
    FARPROC imgui_set_next_window_collapsed;
    FARPROC imgui_begin_table;
    FARPROC imgui_end_table;
    FARPROC imgui_table_next_column;
    FARPROC imgui_table_next_row;
    FARPROC imgui_table_set_column_index;
    FARPROC imgui_get_content_region_avail_x;
    FARPROC imgui_table_setup_column;
} ddraw;

struct CpuFeatures {
    bool sse2 = false;
    bool avx = false;
    bool avx2 = false;
};

CpuFeatures GetCpuFeatures() noexcept {
    CpuFeatures features;
    int cpuInfo[4]{};
    __cpuidex( cpuInfo, 0, 0 );
    const int maxBasicLeaf = cpuInfo[0];
    if ( maxBasicLeaf < 1 ) {
        return features;
    }

    __cpuidex( cpuInfo, 1, 0 );
    features.sse2 = (cpuInfo[3] & (1 << 26)) != 0;
    const bool osXsave = (cpuInfo[2] & (1 << 27)) != 0;
    const bool hardwareAvx = (cpuInfo[2] & (1 << 28)) != 0;
    if ( osXsave && hardwareAvx && (_xgetbv( 0 ) & 0x6) == 0x6 ) {
        features.avx = true;
        if ( maxBasicLeaf >= 7 ) {
            __cpuidex( cpuInfo, 7, 0 );
            features.avx2 = (cpuInfo[1] & (1 << 5)) != 0;
        }
    }
    return features;
}

bool GetExecutableImage( uintptr_t& baseAddress, size_t& imageSize ) noexcept {
    const HMODULE module = GetModuleHandleW( nullptr );
    if ( !module ) {
        return false;
    }

    const auto* base = reinterpret_cast<const BYTE*>(module);
    const auto* dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if ( dosHeader->e_magic != IMAGE_DOS_SIGNATURE
        || dosHeader->e_lfanew <= 0 || dosHeader->e_lfanew > 1024 * 1024 ) {
        return false;
    }

    const auto* ntHeader = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dosHeader->e_lfanew);
    if ( ntHeader->Signature != IMAGE_NT_SIGNATURE
        || ntHeader->OptionalHeader.SizeOfImage < sizeof( IMAGE_DOS_HEADER ) ) {
        return false;
    }

    baseAddress = reinterpret_cast<uintptr_t>(base);
    imageSize = ntHeader->OptionalHeader.SizeOfImage;
    return true;
}

bool MatchesExecutableDword( uintptr_t baseAddress, size_t imageSize, size_t offset, DWORD expected ) noexcept {
    if ( offset > imageSize || imageSize - offset < sizeof( DWORD ) ) {
        return false;
    }
    return *reinterpret_cast<const DWORD*>(baseAddress + offset) == expected;
}

bool ValidateResolvedExports( bool rendererLoaded, const char*& missingExport ) noexcept {
#define REQUIRE_EXPORT(member) do { if ( !ddraw.member ) { missingExport = #member; return false; } } while ( false )
    REQUIRE_EXPORT( AcquireDDThreadLock );
    REQUIRE_EXPORT( CheckFullscreen );
    REQUIRE_EXPORT( CompleteCreateSysmemSurface );
    REQUIRE_EXPORT( D3DParseUnknownCommand );
    REQUIRE_EXPORT( DDGetAttachedSurfaceLcl );
    REQUIRE_EXPORT( DDInternalLock );
    REQUIRE_EXPORT( DDInternalUnlock );
    REQUIRE_EXPORT( DSoundHelp );
    REQUIRE_EXPORT( DirectDrawCreate );
    REQUIRE_EXPORT( DirectDrawCreateClipper );
    REQUIRE_EXPORT( DirectDrawCreateEx );
    REQUIRE_EXPORT( DirectDrawEnumerateA );
    REQUIRE_EXPORT( DirectDrawEnumerateExA );
    REQUIRE_EXPORT( DirectDrawEnumerateExW );
    REQUIRE_EXPORT( DirectDrawEnumerateW );
    REQUIRE_EXPORT( DllCanUnloadNow );
    REQUIRE_EXPORT( DllGetClassObject );
    REQUIRE_EXPORT( GetDDSurfaceLocal );
    REQUIRE_EXPORT( GetOLEThunkData );
    REQUIRE_EXPORT( GetSurfaceFromDC );
    REQUIRE_EXPORT( RegisterSpecialCase );
    REQUIRE_EXPORT( ReleaseDDThreadLock );

    if ( !rendererLoaded ) {
        return true;
    }

    REQUIRE_EXPORT( GDX_AddPointLocator );
    REQUIRE_EXPORT( GDX_SetFogColor );
    REQUIRE_EXPORT( GDX_SetFogDensity );
    REQUIRE_EXPORT( GDX_SetFogHeight );
    REQUIRE_EXPORT( GDX_SetFogHeightFalloff );
    REQUIRE_EXPORT( GDX_SetSunColor );
    REQUIRE_EXPORT( GDX_SetSunStrength );
    REQUIRE_EXPORT( GDX_SetShadowStrength );
    REQUIRE_EXPORT( GDX_SetShadowAOStrength );
    REQUIRE_EXPORT( GDX_SetWorldAOStrength );
    REQUIRE_EXPORT( GDX_OpenMessageBox );
    REQUIRE_EXPORT( GDX_SetBinkVideoRunning );
    REQUIRE_EXPORT( GDX_SetAtmosphericScattering );
    REQUIRE_EXPORT( GDX_SetFogRange );
    REQUIRE_EXPORT( GDX_SetGlobalWindStrength );
    REQUIRE_EXPORT( GDX_SetRainRadiusRange );
    REQUIRE_EXPORT( GDX_SetRainHeightRange );
    REQUIRE_EXPORT( GDX_SetRainSceneWettness );
    REQUIRE_EXPORT( GDX_SetRainFogDensity );
    REQUIRE_EXPORT( GDX_SetRainSunLightStrength );
    REQUIRE_EXPORT( GDX_SetRainNumParticles );
    REQUIRE_EXPORT( GDX_SetRainGlobalVelocity );
    REQUIRE_EXPORT( GDX_SetRainFogColor );
    REQUIRE_EXPORT( GDX_SetRainMoveParticles );
    REQUIRE_EXPORT( GDX_SetRainUseInitialSet );
    REQUIRE_EXPORT( GDX_GetDX11RenderingContext );
    REQUIRE_EXPORT( GDX_GetVersionString );
    REQUIRE_EXPORT( GDX_GetRendererSettings );
    REQUIRE_EXPORT( GDX_SaveRendererSettings );
    REQUIRE_EXPORT( UpdateCustomFontMultiplier );
    REQUIRE_EXPORT( SetCustomSkyTexture );
    REQUIRE_EXPORT( SetCustomSkyTexture_ZenGin );
    REQUIRE_EXPORT( SetCustomSkyWavelengths );
    REQUIRE_EXPORT( LoadMenuSettings );
    REQUIRE_EXPORT( LoadCustomZENResources );
    REQUIRE_EXPORT( EnableWindAnimations );
    REQUIRE_EXPORT( imgui_begin );
    REQUIRE_EXPORT( imgui_begin_overlay );
    REQUIRE_EXPORT( imgui_end );
    REQUIRE_EXPORT( imgui_text );
    REQUIRE_EXPORT( imgui_text_unformatted );
    REQUIRE_EXPORT( imgui_button );
    REQUIRE_EXPORT( imgui_checkbox );
    REQUIRE_EXPORT( imgui_slider_float );
    REQUIRE_EXPORT( imgui_input_text );
    REQUIRE_EXPORT( imgui_same_line );
    REQUIRE_EXPORT( imgui_new_line );
    REQUIRE_EXPORT( imgui_separator );
    REQUIRE_EXPORT( imgui_begin_child );
    REQUIRE_EXPORT( imgui_end_child );
    REQUIRE_EXPORT( imgui_collapsing_header );
    REQUIRE_EXPORT( imgui_begin_main_menu_bar );
    REQUIRE_EXPORT( imgui_end_main_menu_bar );
    REQUIRE_EXPORT( imgui_begin_menu );
    REQUIRE_EXPORT( imgui_end_menu );
    REQUIRE_EXPORT( imgui_menu_item );
    REQUIRE_EXPORT( imgui_push_id );
    REQUIRE_EXPORT( imgui_pop_id );
    REQUIRE_EXPORT( imgui_is_ready );
    REQUIRE_EXPORT( imgui_set_next_window_pos );
    REQUIRE_EXPORT( imgui_set_next_window_size );
    REQUIRE_EXPORT( imgui_set_item_tooltip );
    REQUIRE_EXPORT( imgui_set_next_window_bg_alpha );
    REQUIRE_EXPORT( imgui_set_next_window_collapsed );
    REQUIRE_EXPORT( imgui_begin_table );
    REQUIRE_EXPORT( imgui_end_table );
    REQUIRE_EXPORT( imgui_table_next_column );
    REQUIRE_EXPORT( imgui_table_next_row );
    REQUIRE_EXPORT( imgui_table_set_column_index );
    REQUIRE_EXPORT( imgui_get_content_region_avail_x );
    REQUIRE_EXPORT( imgui_table_setup_column );
#undef REQUIRE_EXPORT
    return true;
}

__declspec(naked) void FakeAcquireDDThreadLock() { _asm { jmp[ddraw.AcquireDDThreadLock] } }
__declspec(naked) void FakeCheckFullscreen() { _asm { jmp[ddraw.CheckFullscreen] } }
__declspec(naked) void FakeCompleteCreateSysmemSurface() { _asm { jmp[ddraw.CompleteCreateSysmemSurface] } }
__declspec(naked) void FakeD3DParseUnknownCommand() { _asm { jmp[ddraw.D3DParseUnknownCommand] } }
__declspec(naked) void FakeDDGetAttachedSurfaceLcl() { _asm { jmp[ddraw.DDGetAttachedSurfaceLcl] } }
__declspec(naked) void FakeDDInternalLock() { _asm { jmp[ddraw.DDInternalLock] } }
__declspec(naked) void FakeDDInternalUnlock() { _asm { jmp[ddraw.DDInternalUnlock] } }
__declspec(naked) void FakeDSoundHelp() { _asm { jmp[ddraw.DSoundHelp] } }
__declspec(naked) void FakeDirectDrawCreate() { _asm { jmp[ddraw.DirectDrawCreate] } }
__declspec(naked) void FakeDirectDrawCreateClipper() { _asm { jmp[ddraw.DirectDrawCreateClipper] } }
__declspec(naked) void FakeDirectDrawCreateEx() { _asm { jmp[ddraw.DirectDrawCreateEx] } }
__declspec(naked) void FakeDirectDrawEnumerateA() { _asm { jmp[ddraw.DirectDrawEnumerateA] } }
__declspec(naked) void FakeDirectDrawEnumerateExA() { _asm { jmp[ddraw.DirectDrawEnumerateExA] } }
__declspec(naked) void FakeDirectDrawEnumerateExW() { _asm { jmp[ddraw.DirectDrawEnumerateExW] } }
__declspec(naked) void FakeDirectDrawEnumerateW() { _asm { jmp[ddraw.DirectDrawEnumerateW] } }
__declspec(naked) void FakeDllCanUnloadNow() { _asm { jmp[ddraw.DllCanUnloadNow] } }
__declspec(naked) void FakeDllGetClassObject() { _asm { jmp[ddraw.DllGetClassObject] } }
__declspec(naked) void FakeGetDDSurfaceLocal() { _asm { jmp[ddraw.GetDDSurfaceLocal] } }
__declspec(naked) void FakeGetOLEThunkData() { _asm { jmp[ddraw.GetOLEThunkData] } }
__declspec(naked) void FakeGetSurfaceFromDC() { _asm { jmp[ddraw.GetSurfaceFromDC] } }
__declspec(naked) void FakeRegisterSpecialCase() { _asm { jmp[ddraw.RegisterSpecialCase] } }
__declspec(naked) void FakeReleaseDDThreadLock() { _asm { jmp[ddraw.ReleaseDDThreadLock] } }
__declspec(naked) void FakeGDX_AddPointLocator() { _asm { jmp[ddraw.GDX_AddPointLocator] } }
__declspec(naked) void FakeGDX_SetFogColor() { _asm { jmp[ddraw.GDX_SetFogColor] } }
__declspec(naked) void FakeGDX_SetFogDensity() { _asm { jmp[ddraw.GDX_SetFogDensity] } }
__declspec(naked) void FakeGDX_SetFogHeight() { _asm { jmp[ddraw.GDX_SetFogHeight] } }
__declspec(naked) void FakeGDX_SetFogHeightFalloff() { _asm { jmp[ddraw.GDX_SetFogHeightFalloff] } }
__declspec(naked) void FakeGDX_SetSunColor() { _asm { jmp[ddraw.GDX_SetSunColor] } }
__declspec(naked) void FakeGDX_SetSunStrength() { _asm { jmp[ddraw.GDX_SetSunStrength] } }
__declspec(naked) void FakeGDX_SetShadowStrength() { _asm { jmp[ddraw.GDX_SetShadowStrength] } }
__declspec(naked) void FakeGDX_SetShadowAOStrength() { _asm { jmp[ddraw.GDX_SetShadowAOStrength] } }
__declspec(naked) void FakeGDX_SetWorldAOStrength() { _asm { jmp[ddraw.GDX_SetWorldAOStrength] } }
__declspec(naked) void FakeGDX_OpenMessageBox() { _asm { jmp[ddraw.GDX_OpenMessageBox] } }
__declspec(naked) void FakeGDX_SetBinkVideoRunning() { _asm { jmp[ddraw.GDX_SetBinkVideoRunning] } }
__declspec(naked) void FakeGDX_SetAtmosphericScattering() { _asm { jmp[ddraw.GDX_SetAtmosphericScattering] } }
__declspec(naked) void FakeGDX_SetFogRange() { _asm { jmp[ddraw.GDX_SetFogRange] } }
__declspec(naked) void FakeGDX_SetGlobalWindStrength() { _asm { jmp[ddraw.GDX_SetGlobalWindStrength] } }
__declspec(naked) void FakeGDX_SetRainRadiusRange() { _asm { jmp[ddraw.GDX_SetRainRadiusRange] } }
__declspec(naked) void FakeGDX_SetRainHeightRange() { _asm { jmp[ddraw.GDX_SetRainHeightRange] } }
__declspec(naked) void FakeGDX_SetRainSceneWettness() { _asm { jmp[ddraw.GDX_SetRainSceneWettness] } }
__declspec(naked) void FakeGDX_SetRainFogDensity() { _asm { jmp[ddraw.GDX_SetRainFogDensity] } }
__declspec(naked) void FakeGDX_SetRainSunLightStrength() { _asm { jmp[ddraw.GDX_SetRainSunLightStrength] } }
__declspec(naked) void FakeGDX_SetRainNumParticles() { _asm { jmp[ddraw.GDX_SetRainNumParticles] } }
__declspec(naked) void FakeGDX_SetRainGlobalVelocity() { _asm { jmp[ddraw.GDX_SetRainGlobalVelocity] } }
__declspec(naked) void FakeGDX_SetRainFogColor() { _asm { jmp[ddraw.GDX_SetRainFogColor] } }
__declspec(naked) void FakeGDX_SetRainMoveParticles() { _asm { jmp[ddraw.GDX_SetRainMoveParticles] } }
__declspec(naked) void FakeGDX_SetRainUseInitialSet() { _asm { jmp[ddraw.GDX_SetRainUseInitialSet] } }
__declspec(naked) void FakeGDX_GetDX11RenderingContext() { _asm { jmp[ddraw.GDX_GetDX11RenderingContext] } }
__declspec(naked) void FakeGDX_GetVersionString() { _asm { jmp[ddraw.GDX_GetVersionString] } }
__declspec(naked) void FakeGDX_GetRendererSettings() { _asm { jmp[ddraw.GDX_GetRendererSettings] } }
__declspec(naked) void FakeGDX_SaveRendererSettings() { _asm { jmp[ddraw.GDX_SaveRendererSettings] } }
__declspec(naked) void FakeUpdateCustomFontMultiplier() { _asm { jmp[ddraw.UpdateCustomFontMultiplier] } }
__declspec(naked) void FakeSetCustomSkyTexture() { _asm { jmp[ddraw.SetCustomSkyTexture] } }
__declspec(naked) void FakeSetCustomSkyTexture_ZenGin() { _asm { jmp[ddraw.SetCustomSkyTexture_ZenGin] } }
__declspec(naked) void FakeSetCustomSkyWavelengths() { _asm { jmp[ddraw.SetCustomSkyWavelengths] } }
__declspec(naked) void FakeLoadMenuSettings() { _asm { jmp[ddraw.LoadMenuSettings] } }
__declspec(naked) void FakeLoadCustomZENResources() { _asm { jmp[ddraw.LoadCustomZENResources] } }
__declspec(naked) void FakeEnableWindAnimations() { _asm { jmp[ddraw.EnableWindAnimations] } }
__declspec(naked) void Fakeimgui_begin() { _asm { jmp[ddraw.imgui_begin] } }
__declspec(naked) void Fakeimgui_begin_overlay() { _asm { jmp[ddraw.imgui_begin_overlay] } }
__declspec(naked) void Fakeimgui_end() { _asm { jmp[ddraw.imgui_end] } }
__declspec(naked) void Fakeimgui_text() { _asm { jmp[ddraw.imgui_text] } }
__declspec(naked) void Fakeimgui_text_unformatted() { _asm { jmp[ddraw.imgui_text_unformatted] } }
__declspec(naked) void Fakeimgui_button() { _asm { jmp[ddraw.imgui_button] } }
__declspec(naked) void Fakeimgui_checkbox() { _asm { jmp[ddraw.imgui_checkbox] } }
__declspec(naked) void Fakeimgui_slider_float() { _asm { jmp[ddraw.imgui_slider_float] } }
__declspec(naked) void Fakeimgui_input_text() { _asm { jmp[ddraw.imgui_input_text] } }
__declspec(naked) void Fakeimgui_same_line() { _asm { jmp[ddraw.imgui_same_line] } }
__declspec(naked) void Fakeimgui_new_line() { _asm { jmp[ddraw.imgui_new_line] } }
__declspec(naked) void Fakeimgui_separator() { _asm { jmp[ddraw.imgui_separator] } }
__declspec(naked) void Fakeimgui_begin_child() { _asm { jmp[ddraw.imgui_begin_child] } }
__declspec(naked) void Fakeimgui_end_child() { _asm { jmp[ddraw.imgui_end_child] } }
__declspec(naked) void Fakeimgui_collapsing_header() { _asm { jmp[ddraw.imgui_collapsing_header] } }
__declspec(naked) void Fakeimgui_begin_main_menu_bar() { _asm { jmp[ddraw.imgui_begin_main_menu_bar] } }
__declspec(naked) void Fakeimgui_end_main_menu_bar() { _asm { jmp[ddraw.imgui_end_main_menu_bar] } }
__declspec(naked) void Fakeimgui_begin_menu() { _asm { jmp[ddraw.imgui_begin_menu] } }
__declspec(naked) void Fakeimgui_end_menu() { _asm { jmp[ddraw.imgui_end_menu] } }
__declspec(naked) void Fakeimgui_menu_item() { _asm { jmp[ddraw.imgui_menu_item] } }
__declspec(naked) void Fakeimgui_push_id() { _asm { jmp[ddraw.imgui_push_id] } }
__declspec(naked) void Fakeimgui_pop_id() { _asm { jmp[ddraw.imgui_pop_id] } }
__declspec(naked) void Fakeimgui_is_ready() { _asm { jmp[ddraw.imgui_is_ready] } }
__declspec(naked) void Fakeimgui_set_next_window_pos() { _asm { jmp[ddraw.imgui_set_next_window_pos] } }
__declspec(naked) void Fakeimgui_set_next_window_size() { _asm { jmp[ddraw.imgui_set_next_window_size] } }
__declspec(naked) void Fakeimgui_set_next_window_bg_alpha() { _asm { jmp[ddraw.imgui_set_next_window_bg_alpha] } }
__declspec(naked) void Fakeimgui_set_next_window_collapsed() { _asm { jmp[ddraw.imgui_set_next_window_collapsed] } }
__declspec(naked) void Fakeimgui_set_item_tooltip() { _asm { jmp[ddraw.imgui_set_item_tooltip] } }
__declspec(naked) void Fakeimgui_get_content_region_avail_x() { _asm { jmp[ddraw.imgui_get_content_region_avail_x] } }
__declspec(naked) void Fakeimgui_begin_table() { _asm { jmp[ddraw.imgui_begin_table] } }
__declspec(naked) void Fakeimgui_end_table() { _asm { jmp[ddraw.imgui_end_table] } }
__declspec(naked) void Fakeimgui_table_next_column() { _asm { jmp[ddraw.imgui_table_next_column] } }
__declspec(naked) void Fakeimgui_table_next_row() { _asm { jmp[ddraw.imgui_table_next_row] } }
__declspec(naked) void Fakeimgui_table_set_column_index() { _asm { jmp[ddraw.imgui_table_set_column_index] } }
__declspec(naked) void Fakeimgui_table_setup_column() { _asm { jmp[ddraw.imgui_table_setup_column] } }



uintptr_t __cdecl FallbackCdeclExport() noexcept {
    return 0;
}

const char* __cdecl FallbackVersionString() noexcept {
    return "GD3D11 unavailable";
}

void* __cdecl FallbackRendererSettings( unsigned int& structSize ) noexcept {
    structSize = 0;
    return nullptr;
}

float WINAPI FallbackFontMultiplier( float ) noexcept {
    return 1.0f;
}

void WINAPI FallbackSetCustomSkyTexture( int, bool ) noexcept {}
void WINAPI FallbackSetCustomSkyTextureZenGin( bool, void* ) noexcept {}
void WINAPI FallbackSetCustomSkyWavelengths( float, float, float ) noexcept {}
void WINAPI FallbackLoadMenuSettings( char* ) noexcept {}
void WINAPI FallbackVoidExport() noexcept {}

void InstallFallbackExports() noexcept {
    FARPROC generic = reinterpret_cast<FARPROC>(&FallbackCdeclExport);

#define SET_GENERIC_FALLBACK(member) ddraw.member = generic
    SET_GENERIC_FALLBACK( GDX_AddPointLocator );
    SET_GENERIC_FALLBACK( GDX_SetFogColor );
    SET_GENERIC_FALLBACK( GDX_SetFogDensity );
    SET_GENERIC_FALLBACK( GDX_SetFogHeight );
    SET_GENERIC_FALLBACK( GDX_SetFogHeightFalloff );
    SET_GENERIC_FALLBACK( GDX_SetSunColor );
    SET_GENERIC_FALLBACK( GDX_SetSunStrength );
    SET_GENERIC_FALLBACK( GDX_SetShadowStrength );
    SET_GENERIC_FALLBACK( GDX_SetShadowAOStrength );
    SET_GENERIC_FALLBACK( GDX_SetWorldAOStrength );
    SET_GENERIC_FALLBACK( GDX_OpenMessageBox );
    SET_GENERIC_FALLBACK( GDX_SetBinkVideoRunning );
    SET_GENERIC_FALLBACK( GDX_SetAtmosphericScattering );
    SET_GENERIC_FALLBACK( GDX_SetFogRange );
    SET_GENERIC_FALLBACK( GDX_SetGlobalWindStrength );
    SET_GENERIC_FALLBACK( GDX_SetRainRadiusRange );
    SET_GENERIC_FALLBACK( GDX_SetRainHeightRange );
    SET_GENERIC_FALLBACK( GDX_SetRainSceneWettness );
    SET_GENERIC_FALLBACK( GDX_SetRainFogDensity );
    SET_GENERIC_FALLBACK( GDX_SetRainSunLightStrength );
    SET_GENERIC_FALLBACK( GDX_SetRainNumParticles );
    SET_GENERIC_FALLBACK( GDX_SetRainGlobalVelocity );
    SET_GENERIC_FALLBACK( GDX_SetRainFogColor );
    SET_GENERIC_FALLBACK( GDX_SetRainMoveParticles );
    SET_GENERIC_FALLBACK( GDX_SetRainUseInitialSet );
    SET_GENERIC_FALLBACK( GDX_GetDX11RenderingContext );
    SET_GENERIC_FALLBACK( GDX_SaveRendererSettings );
    SET_GENERIC_FALLBACK( imgui_begin );
    SET_GENERIC_FALLBACK( imgui_begin_overlay );
    SET_GENERIC_FALLBACK( imgui_end );
    SET_GENERIC_FALLBACK( imgui_text );
    SET_GENERIC_FALLBACK( imgui_text_unformatted );
    SET_GENERIC_FALLBACK( imgui_button );
    SET_GENERIC_FALLBACK( imgui_checkbox );
    SET_GENERIC_FALLBACK( imgui_slider_float );
    SET_GENERIC_FALLBACK( imgui_input_text );
    SET_GENERIC_FALLBACK( imgui_same_line );
    SET_GENERIC_FALLBACK( imgui_new_line );
    SET_GENERIC_FALLBACK( imgui_separator );
    SET_GENERIC_FALLBACK( imgui_begin_child );
    SET_GENERIC_FALLBACK( imgui_end_child );
    SET_GENERIC_FALLBACK( imgui_collapsing_header );
    SET_GENERIC_FALLBACK( imgui_begin_main_menu_bar );
    SET_GENERIC_FALLBACK( imgui_end_main_menu_bar );
    SET_GENERIC_FALLBACK( imgui_begin_menu );
    SET_GENERIC_FALLBACK( imgui_end_menu );
    SET_GENERIC_FALLBACK( imgui_menu_item );
    SET_GENERIC_FALLBACK( imgui_push_id );
    SET_GENERIC_FALLBACK( imgui_pop_id );
    SET_GENERIC_FALLBACK( imgui_is_ready );
    SET_GENERIC_FALLBACK( imgui_set_next_window_pos );
    SET_GENERIC_FALLBACK( imgui_set_next_window_size );
    SET_GENERIC_FALLBACK( imgui_set_item_tooltip );
    SET_GENERIC_FALLBACK( imgui_set_next_window_bg_alpha );
    SET_GENERIC_FALLBACK( imgui_set_next_window_collapsed );
    SET_GENERIC_FALLBACK( imgui_begin_table );
    SET_GENERIC_FALLBACK( imgui_end_table );
    SET_GENERIC_FALLBACK( imgui_table_next_column );
    SET_GENERIC_FALLBACK( imgui_table_next_row );
    SET_GENERIC_FALLBACK( imgui_table_set_column_index );
    SET_GENERIC_FALLBACK( imgui_get_content_region_avail_x );
    SET_GENERIC_FALLBACK( imgui_table_setup_column );
#undef SET_GENERIC_FALLBACK

    ddraw.GDX_GetVersionString = reinterpret_cast<FARPROC>(&FallbackVersionString);
    ddraw.GDX_GetRendererSettings = reinterpret_cast<FARPROC>(&FallbackRendererSettings);
    ddraw.UpdateCustomFontMultiplier = reinterpret_cast<FARPROC>(&FallbackFontMultiplier);
    ddraw.SetCustomSkyTexture = reinterpret_cast<FARPROC>(&FallbackSetCustomSkyTexture);
    ddraw.SetCustomSkyTexture_ZenGin = reinterpret_cast<FARPROC>(&FallbackSetCustomSkyTextureZenGin);
    ddraw.SetCustomSkyWavelengths = reinterpret_cast<FARPROC>(&FallbackSetCustomSkyWavelengths);
    ddraw.LoadMenuSettings = reinterpret_cast<FARPROC>(&FallbackLoadMenuSettings);
    ddraw.LoadCustomZENResources = reinterpret_cast<FARPROC>(&FallbackVoidExport);
    ddraw.EnableWindAnimations = reinterpret_cast<FARPROC>(&FallbackVoidExport);
}

bool FakeIsUsingBGRATextures() { return true; }

bool CheckFileExists( const wchar_t* fileName ) noexcept {
    if ( !fileName || !fileName[0] ) {
        return false;
    }
    const DWORD attributes = GetFileAttributesW( fileName );
    return attributes != INVALID_FILE_ATTRIBUTES
        && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

void CheckLibraryExists( const wchar_t* filePath, const wchar_t* fileName ) noexcept {
    wchar_t libraryPath[MAX_PATH]{};
    if ( !filePath || !fileName || !PathCombineW( libraryPath, filePath, fileName )
        || !CheckFileExists( libraryPath ) ) {
        MessageBoxW( nullptr,
            L"GD3D11 Renderer could not find assimp-vc143-mt.dll in the game directory.",
            L"Gothic GD3D11", MB_ICONERROR );
    }
}

BOOL APIENTRY DllMain( HINSTANCE hInst, DWORD reason, LPVOID ) {
    if ( reason == DLL_PROCESS_ATTACH ) {
        DisableThreadLibraryCalls( hInst );
        try {
        int foundExecutable = INVALID_EXECUTABLE;
        const CpuFeatures cpu = GetCpuFeatures();
        bool haveAVX = cpu.avx;
        bool haveAVX2 = cpu.avx2;

        if ( !cpu.sse2 ) {
            MessageBoxW( nullptr, L"GD3D11 Renderer requires SSE2 instructions.", L"Gothic GD3D11", MB_ICONERROR );
            return FALSE;
        }
        uintptr_t baseAddr = 0;
        size_t imageSize = 0;
        const bool validImage = GetExecutableImage( baseAddr, imageSize );
        if ( validImage && baseAddr == 0x400000 ) {
            if ( MatchesExecutableDword( baseAddr, imageSize, 0x168, 0x3D4318 )
                && MatchesExecutableDword( baseAddr, imageSize, 0x3D43A0, 0x82E108 )
                && MatchesExecutableDword( baseAddr, imageSize, 0x3D43CB, 0x82E10C ) ) {
                foundExecutable = GOTHIC2A_EXECUTABLE;
            } else if ( MatchesExecutableDword( baseAddr, imageSize, 0x160, 0x37A8D8 )
                && MatchesExecutableDword( baseAddr, imageSize, 0x37A960, 0x7D01E4 )
                && MatchesExecutableDword( baseAddr, imageSize, 0x37A98B, 0x7D01E8 ) ) {
                foundExecutable = GOTHIC1_EXECUTABLE;
            } else if ( MatchesExecutableDword( baseAddr, imageSize, 0x140, 0x3BE698 )
                && MatchesExecutableDword( baseAddr, imageSize, 0x3BE720, 0x8131E4 )
                && MatchesExecutableDword( baseAddr, imageSize, 0x3BE74B, 0x8131E8 ) ) {
                foundExecutable = GOTHIC1A_EXECUTABLE;
            }
        }

        if ( foundExecutable != INVALID_EXECUTABLE ) {
            std::wstring commandLine = GetCommandLineW();
            std::transform( commandLine.begin(), commandLine.end(), commandLine.begin(),
                []( wchar_t character ) { return static_cast<wchar_t>(towlower( character )); } );
            if ( commandLine.find( L"-game:spacer_net.ini" ) != std::wstring::npos ) {
                // Don't search for avx versions
                haveAVX2 = false;
                haveAVX = false;
                if ( foundExecutable == GOTHIC1_EXECUTABLE ) {
                    foundExecutable = GOTHIC1_SPACERNET;
                } else if ( foundExecutable == GOTHIC2A_EXECUTABLE ) {
                    foundExecutable = GOTHIC2_SPACERNET;
                }
            }
        }

        wchar_t executablePath[MAX_PATH]{};
        const DWORD executablePathLength = GetModuleFileNameW( nullptr, executablePath, MAX_PATH );
        if ( executablePathLength == 0 || executablePathLength >= MAX_PATH
            || !PathRemoveFileSpecW( executablePath ) ) {
            MessageBoxW( nullptr, L"GD3D11 Renderer could not resolve the game directory.",
                L"Gothic GD3D11", MB_ICONERROR );
            return FALSE;
        }

        CheckLibraryExists( executablePath, L"assimp-vc143-mt.dll" );
        ddraw.dll = nullptr;

        std::wstring dllPath;
        DWORD rendererLoadError = ERROR_SUCCESS;
        bool showLoadingInfo = true;
        const wchar_t* rendererStem = GetRendererBinaryStem( foundExecutable );
        if ( rendererStem ) {
            auto tryRenderer = [&]( const wchar_t* suffix ) {
                if ( ddraw.dll ) {
                    return;
                }
                dllPath = std::wstring( executablePath ) + rendererStem + suffix;
                SetLastError( ERROR_SUCCESS );
                ddraw.dll = LoadLibraryW( dllPath.c_str() );
                if ( !ddraw.dll ) {
                    rendererLoadError = GetLastError();
                }
            };

            if ( haveAVX2 ) {
                tryRenderer( L"_avx2.dll" );
            }
            if ( haveAVX ) {
                tryRenderer( L"_avx.dll" );
            }
            tryRenderer( L".dll" );
        } else {
            if ( baseAddr != 0x400000 ) {
                MessageBoxA( nullptr, "GD3D11 Renderer couldn't be loaded.\nDetected enabled ASLR. Disable both Mandatory and Bottom-up ASLR in Exploit Protection for Gothic executable before continuing.", "Gothic GD3D11", MB_ICONERROR );
            } else {
                MessageBoxA( nullptr, "GD3D11 Renderer doesn't work with your game version.\nIt requires report version of the game. Same as System Pack or Union.", "Gothic GD3D11", MB_ICONERROR );
            }
            showLoadingInfo = false;
        }

        const bool rendererLoaded = ddraw.dll != nullptr;
        if ( !ddraw.dll ) {
            if ( showLoadingInfo ) {
                if ( !CheckFileExists( dllPath.c_str() ) ) {
                    std::wstring errorMessage =
                        L"GD3D11 Renderer could not load the selected DLL:\n" + dllPath
                        + L"\nThe file does not exist.";
                    MessageBoxW( nullptr, errorMessage.c_str(), L"Gothic GD3D11", MB_ICONERROR );
                } else {
                    const DWORD errorCode = rendererLoadError != ERROR_SUCCESS
                        ? rendererLoadError : ERROR_DLL_INIT_FAILED;
                    std::wstring errorMessage( L"GD3D11 Renderer could not load the selected DLL.\n" );
                    LPWSTR messageBuffer = nullptr;
                    const DWORD size = FormatMessageW(
                        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                        nullptr, errorCode, MAKELANGID( LANG_NEUTRAL, SUBLANG_DEFAULT ),
                        reinterpret_cast<LPWSTR>(&messageBuffer), 0, nullptr );
                    if ( size > 0 && messageBuffer ) {
                        errorMessage.append( messageBuffer, size );
                    } else {
                        wchar_t buffer[32]{};
                        swprintf_s( buffer, L"Error 0x%08X.", errorCode );
                        errorMessage.append( buffer );
                    }
                    if ( messageBuffer ) {
                        LocalFree( messageBuffer );
                    }
                    MessageBoxW( nullptr, errorMessage.c_str(), L"Gothic GD3D11", MB_ICONERROR );
                }
            }

            wchar_t systemDdrawPath[MAX_PATH]{};
            const UINT systemPathLength = GetSystemDirectoryW( systemDdrawPath, MAX_PATH );
            if ( systemPathLength == 0 || systemPathLength >= MAX_PATH
                || wcscat_s( systemDdrawPath, L"\\ddraw.dll" ) != 0 ) {
                MessageBoxW( nullptr, L"GD3D11 could not resolve the system DirectDraw DLL.",
                    L"Gothic GD3D11", MB_ICONERROR );
                return FALSE;
            }
            ddraw.dll = LoadLibraryW( systemDdrawPath );
            if ( !ddraw.dll ) {
                MessageBoxW( nullptr, L"GD3D11 could not load the system DirectDraw DLL.",
                    L"Gothic GD3D11", MB_ICONERROR );
                return FALSE;
            }
        }

        ddraw.AcquireDDThreadLock = GetProcAddress( ddraw.dll, "AcquireDDThreadLock" );
        ddraw.CheckFullscreen = GetProcAddress( ddraw.dll, "CheckFullscreen" );
        ddraw.CompleteCreateSysmemSurface = GetProcAddress( ddraw.dll, "CompleteCreateSysmemSurface" );
        ddraw.D3DParseUnknownCommand = GetProcAddress( ddraw.dll, "D3DParseUnknownCommand" );
        ddraw.DDGetAttachedSurfaceLcl = GetProcAddress( ddraw.dll, "DDGetAttachedSurfaceLcl" );
        ddraw.DDInternalLock = GetProcAddress( ddraw.dll, "DDInternalLock" );
        ddraw.DDInternalUnlock = GetProcAddress( ddraw.dll, "DDInternalUnlock" );
        ddraw.DSoundHelp = GetProcAddress( ddraw.dll, "DSoundHelp" );
        ddraw.DirectDrawCreate = GetProcAddress( ddraw.dll, "DirectDrawCreate" );
        ddraw.DirectDrawCreateClipper = GetProcAddress( ddraw.dll, "DirectDrawCreateClipper" );
        ddraw.DirectDrawCreateEx = GetProcAddress( ddraw.dll, "DirectDrawCreateEx" );
        ddraw.DirectDrawEnumerateA = GetProcAddress( ddraw.dll, "DirectDrawEnumerateA" );
        ddraw.DirectDrawEnumerateExA = GetProcAddress( ddraw.dll, "DirectDrawEnumerateExA" );
        ddraw.DirectDrawEnumerateExW = GetProcAddress( ddraw.dll, "DirectDrawEnumerateExW" );
        ddraw.DirectDrawEnumerateW = GetProcAddress( ddraw.dll, "DirectDrawEnumerateW" );
        ddraw.DllCanUnloadNow = GetProcAddress( ddraw.dll, "DllCanUnloadNow" );
        ddraw.DllGetClassObject = GetProcAddress( ddraw.dll, "DllGetClassObject" );
        ddraw.GetDDSurfaceLocal = GetProcAddress( ddraw.dll, "GetDDSurfaceLocal" );
        ddraw.GetOLEThunkData = GetProcAddress( ddraw.dll, "GetOLEThunkData" );
        ddraw.GetSurfaceFromDC = GetProcAddress( ddraw.dll, "GetSurfaceFromDC" );
        ddraw.RegisterSpecialCase = GetProcAddress( ddraw.dll, "RegisterSpecialCase" );
        ddraw.ReleaseDDThreadLock = GetProcAddress( ddraw.dll, "ReleaseDDThreadLock" );

        ddraw.GDX_AddPointLocator = GetProcAddress( ddraw.dll, "GDX_AddPointLocator" );
        ddraw.GDX_SetFogColor = GetProcAddress( ddraw.dll, "GDX_SetFogColor" );
        ddraw.GDX_SetFogDensity = GetProcAddress( ddraw.dll, "GDX_SetFogDensity" );
        ddraw.GDX_SetFogHeight = GetProcAddress( ddraw.dll, "GDX_SetFogHeight" );
        ddraw.GDX_SetFogHeightFalloff = GetProcAddress( ddraw.dll, "GDX_SetFogHeightFalloff" );
        ddraw.GDX_SetSunColor = GetProcAddress( ddraw.dll, "GDX_SetSunColor" );
        ddraw.GDX_SetSunStrength = GetProcAddress( ddraw.dll, "GDX_SetSunStrength" );
        ddraw.GDX_SetShadowStrength = GetProcAddress( ddraw.dll, "GDX_SetShadowStrength" );
        ddraw.GDX_SetShadowAOStrength = GetProcAddress( ddraw.dll, "GDX_SetShadowAOStrength" );
        ddraw.GDX_SetWorldAOStrength = GetProcAddress( ddraw.dll, "GDX_SetWorldAOStrength" );
        ddraw.GDX_OpenMessageBox = GetProcAddress( ddraw.dll, "GDX_OpenMessageBox" );
        ddraw.GDX_SetBinkVideoRunning = GetProcAddress( ddraw.dll, "GDX_SetBinkVideoRunning" );
        ddraw.GDX_SetAtmosphericScattering = GetProcAddress( ddraw.dll, "GDX_SetAtmosphericScattering" );
        ddraw.GDX_SetFogRange = GetProcAddress( ddraw.dll, "GDX_SetFogRange" );
        ddraw.GDX_SetGlobalWindStrength = GetProcAddress( ddraw.dll, "GDX_SetGlobalWindStrength" );
        ddraw.GDX_SetRainRadiusRange = GetProcAddress( ddraw.dll, "GDX_SetRainRadiusRange" );
        ddraw.GDX_SetRainHeightRange = GetProcAddress( ddraw.dll, "GDX_SetRainHeightRange" );
        ddraw.GDX_SetRainSceneWettness = GetProcAddress( ddraw.dll, "GDX_SetRainSceneWettness" );
        ddraw.GDX_SetRainFogDensity = GetProcAddress( ddraw.dll, "GDX_SetRainFogDensity" );
        ddraw.GDX_SetRainSunLightStrength = GetProcAddress( ddraw.dll, "GDX_SetRainSunLightStrength" );
        ddraw.GDX_SetRainNumParticles = GetProcAddress( ddraw.dll, "GDX_SetRainNumParticles" );
        ddraw.GDX_SetRainGlobalVelocity = GetProcAddress( ddraw.dll, "GDX_SetRainGlobalVelocity" );
        ddraw.GDX_SetRainFogColor = GetProcAddress( ddraw.dll, "GDX_SetRainFogColor" );
        ddraw.GDX_SetRainMoveParticles = GetProcAddress( ddraw.dll, "GDX_SetRainMoveParticles" );
        ddraw.GDX_SetRainUseInitialSet = GetProcAddress( ddraw.dll, "GDX_SetRainUseInitialSet" );
        ddraw.GDX_GetDX11RenderingContext = GetProcAddress( ddraw.dll, "GDX_GetDX11RenderingContext" );
        ddraw.GDX_GetVersionString = GetProcAddress( ddraw.dll, "GDX_GetVersionString" );
        ddraw.GDX_GetRendererSettings = GetProcAddress( ddraw.dll, "GDX_GetRendererSettings" );
        ddraw.GDX_SaveRendererSettings = GetProcAddress( ddraw.dll, "GDX_SaveRendererSettings" );

        ddraw.UpdateCustomFontMultiplier = GetProcAddress( ddraw.dll, "UpdateCustomFontMultiplier" );
        ddraw.SetCustomSkyTexture = GetProcAddress( ddraw.dll, "SetCustomSkyTexture" );
        ddraw.SetCustomSkyTexture_ZenGin = GetProcAddress( ddraw.dll, "SetCustomSkyTexture_ZenGin" );
        ddraw.SetCustomSkyWavelengths = GetProcAddress( ddraw.dll, "SetCustomSkyWavelengths" );
        ddraw.LoadMenuSettings = GetProcAddress( ddraw.dll, "LoadMenuSettings" );
        ddraw.LoadCustomZENResources = GetProcAddress( ddraw.dll, "LoadCustomZENResources" );
        ddraw.EnableWindAnimations = GetProcAddress(
            ddraw.dll, MAKEINTRESOURCEA( 2137 ) );
        
        ddraw.imgui_begin = GetProcAddress( ddraw.dll, "imgui_begin" );
        ddraw.imgui_begin_overlay = GetProcAddress( ddraw.dll, "imgui_begin_overlay" );
        ddraw.imgui_end = GetProcAddress( ddraw.dll, "imgui_end" );
        ddraw.imgui_text = GetProcAddress( ddraw.dll, "imgui_text" );
        ddraw.imgui_text_unformatted = GetProcAddress( ddraw.dll, "imgui_text_unformatted" );
        ddraw.imgui_button = GetProcAddress( ddraw.dll, "imgui_button" );
        ddraw.imgui_checkbox = GetProcAddress( ddraw.dll, "imgui_checkbox" );
        ddraw.imgui_slider_float = GetProcAddress( ddraw.dll, "imgui_slider_float" );
        ddraw.imgui_input_text = GetProcAddress( ddraw.dll, "imgui_input_text" );
        ddraw.imgui_same_line = GetProcAddress( ddraw.dll, "imgui_same_line" );
        ddraw.imgui_new_line = GetProcAddress( ddraw.dll, "imgui_new_line" );
        ddraw.imgui_separator = GetProcAddress( ddraw.dll, "imgui_separator" );
        ddraw.imgui_begin_child = GetProcAddress( ddraw.dll, "imgui_begin_child" );
        ddraw.imgui_end_child = GetProcAddress( ddraw.dll, "imgui_end_child" );
        ddraw.imgui_collapsing_header = GetProcAddress( ddraw.dll, "imgui_collapsing_header" );
        ddraw.imgui_begin_main_menu_bar = GetProcAddress( ddraw.dll, "imgui_begin_main_menu_bar" );
        ddraw.imgui_end_main_menu_bar = GetProcAddress( ddraw.dll, "imgui_end_main_menu_bar" );
        ddraw.imgui_begin_menu = GetProcAddress( ddraw.dll, "imgui_begin_menu" );
        ddraw.imgui_end_menu = GetProcAddress( ddraw.dll, "imgui_end_menu" );
        ddraw.imgui_menu_item = GetProcAddress( ddraw.dll, "imgui_menu_item" );
        ddraw.imgui_push_id = GetProcAddress( ddraw.dll, "imgui_push_id" );
        ddraw.imgui_pop_id = GetProcAddress( ddraw.dll, "imgui_pop_id" );
        ddraw.imgui_is_ready = GetProcAddress( ddraw.dll, "imgui_is_ready" );
        ddraw.imgui_set_next_window_pos = GetProcAddress( ddraw.dll, "imgui_set_next_window_pos" );
        ddraw.imgui_set_next_window_size = GetProcAddress( ddraw.dll, "imgui_set_next_window_size" );
        ddraw.imgui_set_item_tooltip = GetProcAddress( ddraw.dll, "imgui_set_item_tooltip" );
        ddraw.imgui_set_next_window_bg_alpha = GetProcAddress( ddraw.dll, "imgui_set_next_window_bg_alpha" );
        ddraw.imgui_set_next_window_collapsed = GetProcAddress( ddraw.dll, "imgui_set_next_window_collapsed" );
        ddraw.imgui_begin_table = GetProcAddress( ddraw.dll, "imgui_begin_table" );
        ddraw.imgui_end_table = GetProcAddress( ddraw.dll, "imgui_end_table" );
        ddraw.imgui_table_next_column = GetProcAddress( ddraw.dll, "imgui_table_next_column" );
        ddraw.imgui_table_next_row = GetProcAddress( ddraw.dll, "imgui_table_next_row" );
        ddraw.imgui_table_set_column_index = GetProcAddress( ddraw.dll, "imgui_table_set_column_index" );
        ddraw.imgui_get_content_region_avail_x = GetProcAddress( ddraw.dll, "imgui_get_content_region_avail_x" );
        ddraw.imgui_table_setup_column = GetProcAddress( ddraw.dll, "imgui_table_setup_column" );

        if ( !rendererLoaded ) {
            InstallFallbackExports();
        }

        const char* missingExport = nullptr;
        if ( !ValidateResolvedExports( rendererLoaded, missingExport ) ) {
            char errorMessage[256]{};
            sprintf_s( errorMessage, "Loaded DLL is missing required export '%s'.",
                missingExport ? missingExport : "<unknown>" );
            MessageBoxA( nullptr, errorMessage, "Gothic GD3D11", MB_ICONERROR );
            FreeLibrary( ddraw.dll );
            ddraw.dll = nullptr;
            return FALSE;
        }
        } catch ( ... ) {
            if ( ddraw.dll ) {
                FreeLibrary( ddraw.dll );
                ddraw.dll = nullptr;
            }
            MessageBoxW( nullptr, L"GD3D11 Launcher initialization failed unexpectedly.",
                L"Gothic GD3D11", MB_ICONERROR );
            return FALSE;
        }
    }
    return TRUE;
}
