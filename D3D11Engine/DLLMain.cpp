#include "pch.h"

#pragma clang diagnostic ignored "-Wwritable-strings"

#include "ddraw.h"
#include "D3D7/MyDirectDraw.h"
#include "Logger.h"
#include "Detours/detours.h"
#include "DbgHelp.h"
#include "HookedFunctions.h"
#include "VersionCheck.h"
#include "InstructionSet.h"
#include "D3D11GraphicsEngine.h"

#include <shlwapi.h>
#include "GSky.h"

#include <cstdlib>
#include <new>

#pragma comment(lib, "shlwapi.lib")

ZQuantizeHalfFloat QuantizeHalfFloat;
ZQuantizeHalfFloat_X4 QuantizeHalfFloat_X4;
ZUnquantizeHalfFloat UnquantizeHalfFloat;
ZUnquantizeHalfFloat_X4 UnquantizeHalfFloat_X4;
ZUnquantizeHalfFloat_X4 UnquantizeHalfFloat_X8;


#if defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)
bool haveWindAnimations = false;
#endif
bool userHaveAMDGPU = false;

typedef void (WINAPI* DirectDrawSimple)();
typedef HRESULT( WINAPI* DirectDrawCreateEx_type )(GUID FAR*, LPVOID*, REFIID, IUnknown FAR*);

#if defined(BUILD_GOTHIC_2_6_fix)
using WinMainFunc = decltype(&WinMain);
WinMainFunc originalWinMain = reinterpret_cast<WinMainFunc>(GothicMemoryLocations::Functions::WinMain);
#endif

bool GMPModeActive = false;

unsigned short QuantizeHalfFloat_Scalar( float input )
{
    union { float f; unsigned int ui; } u = { input };
    unsigned int ui = u.ui;

    int s = ( ui >> 16 ) & 0x8000;
    int em = ui & 0x7fffffff;

    int h = ( em - ( 112 << 23 ) + ( 1 << 12 ) ) >> 13;
    h = ( em < ( 113 << 23 ) ) ? 0 : h;
    h = ( em >= ( 143 << 23 ) ) ? 0x7c00 : h;
    h = ( em > ( 255 << 23 ) ) ? 0x7e00 : h;
    return static_cast<unsigned short>(s | h);
}

void QuantizeHalfFloats_X4_SSE2( float* input, unsigned short* output )
{
    __m128i v = _mm_castps_si128( _mm_load_ps( input ) );
    __m128i s = _mm_and_si128( _mm_srli_epi32( v, 16 ), _mm_set1_epi32( 0x8000 ) );
    __m128i em = _mm_and_si128( v, _mm_set1_epi32( 0x7FFFFFFF ) );
    __m128i h = _mm_srli_epi32( _mm_sub_epi32( em, _mm_set1_epi32( 0x37FFF000 ) ), 13 );

    __m128i mask = _mm_cmplt_epi32( em, _mm_set1_epi32( 0x38800000 ) );
    h = _mm_or_si128( _mm_and_si128( mask, _mm_setzero_si128() ), _mm_andnot_si128( mask, h ) );

    mask = _mm_cmpgt_epi32( em, _mm_set1_epi32( 0x47800000 - 1 ) );
    h = _mm_or_si128( _mm_and_si128( mask, _mm_set1_epi32( 0x7C00 ) ), _mm_andnot_si128( mask, h ) );

    mask = _mm_cmpgt_epi32( em, _mm_set1_epi32( 0x7F800000 ) );
    h = _mm_or_si128( _mm_and_si128( mask, _mm_set1_epi32( 0x7E00 ) ), _mm_andnot_si128( mask, h ) );

    // We need to stay in int16_t range due to signed saturation
    __m128i halfs = _mm_sub_epi32( _mm_or_si128( s, h ), _mm_set1_epi32( 32768 ) );
    _mm_store_sd( reinterpret_cast<double*>(output), _mm_castsi128_pd( _mm_add_epi16( _mm_packs_epi32( halfs, halfs ), _mm_set1_epi16( 32768u ) ) ) );
}

void QuantizeHalfFloats_X4_SSE41( float* input, unsigned short* output )
{
    QuantizeHalfFloats_X4_SSE2( input, output );
}

#ifdef _XM_AVX2_INTRINSICS_
unsigned short QuantizeHalfFloat_F16C( float input )
{
    return static_cast<unsigned short>(_mm_cvtsi128_si32( _mm_cvtps_ph( _mm_set_ss( input ), _MM_FROUND_CUR_DIRECTION ) ));
}

void QuantizeHalfFloats_X4_F16C( float* input, unsigned short* output )
{
    _mm_store_sd( reinterpret_cast<double*>(output), _mm_castsi128_pd( _mm_cvtps_ph( _mm_load_ps( input ), _MM_FROUND_CUR_DIRECTION ) ) );
}
#endif

float UnquantizeHalfFloat_Scalar( unsigned short input )
{
    unsigned int s = input & 0x8000;
    unsigned int m = input & 0x03FF;
    unsigned int e = input & 0x7C00;
    e += 0x0001C000;

    float out;
    unsigned int r = (s << 16) | (m << 13) | (e << 13);
    memcpy( &out, &r, sizeof( float ) );
    return out;
}

void UnquantizeHalfFloat_X4_SSE2( unsigned short* input, float* output )
{
    const __m128i mask_zero = _mm_setzero_si128();
    const __m128i mask_s = _mm_set1_epi16( 0x8000u );
    const __m128i mask_m = _mm_set1_epi16( 0x03FF );
    const __m128i mask_e = _mm_set1_epi16( 0x7C00 );
    const __m128i bias_e = _mm_set1_epi32( 0x0001C000 );

    __m128i halfs = _mm_loadl_epi64( reinterpret_cast<const __m128i*>(input) );

    __m128i s = _mm_and_si128( halfs, mask_s );
    __m128i m = _mm_and_si128( halfs, mask_m );
    __m128i e = _mm_and_si128( halfs, mask_e );

    __m128i s4 = _mm_unpacklo_epi16( s, mask_zero );
    s4 = _mm_slli_epi32( s4, 16 );

    __m128i m4 = _mm_unpacklo_epi16( m, mask_zero );
    m4 = _mm_slli_epi32( m4, 13 );

    __m128i e4 = _mm_unpacklo_epi16( e, mask_zero );
    e4 = _mm_add_epi32( e4, bias_e );
    e4 = _mm_slli_epi32( e4, 13 );

    _mm_store_si128( reinterpret_cast<__m128i*>(output), _mm_or_si128( s4, _mm_or_si128( e4, m4 ) ) );
}

void UnquantizeHalfFloat_X8_SSE2( unsigned short* input, float* output )
{
    const __m128i mask_zero = _mm_setzero_si128();
    const __m128i mask_s = _mm_set1_epi16( 0x8000u );
    const __m128i mask_m = _mm_set1_epi16( 0x03FF );
    const __m128i mask_e = _mm_set1_epi16( 0x7C00 );
    const __m128i bias_e = _mm_set1_epi32( 0x0001C000 );

    __m128i halfs = _mm_load_si128( reinterpret_cast<const __m128i*>(input) );

    __m128i s = _mm_and_si128( halfs, mask_s );
    __m128i m = _mm_and_si128( halfs, mask_m );
    __m128i e = _mm_and_si128( halfs, mask_e );

    __m128i s4 = _mm_unpacklo_epi16( s, mask_zero );
    s4 = _mm_slli_epi32( s4, 16 );

    __m128i m4 = _mm_unpacklo_epi16( m, mask_zero );
    m4 = _mm_slli_epi32( m4, 13 );

    __m128i e4 = _mm_unpacklo_epi16( e, mask_zero );
    e4 = _mm_add_epi32( e4, bias_e );
    e4 = _mm_slli_epi32( e4, 13 );

    _mm_store_si128( reinterpret_cast<__m128i*>(output + 0), _mm_or_si128( s4, _mm_or_si128( e4, m4 ) ) );

    s4 = _mm_unpackhi_epi16( s, mask_zero );
    s4 = _mm_slli_epi32( s4, 16 );

    m4 = _mm_unpackhi_epi16( m, mask_zero );
    m4 = _mm_slli_epi32( m4, 13 );

    e4 = _mm_unpackhi_epi16( e, mask_zero );
    e4 = _mm_add_epi32( e4, bias_e );
    e4 = _mm_slli_epi32( e4, 13 );

    _mm_store_si128( reinterpret_cast<__m128i*>(output + 4), _mm_or_si128( s4, _mm_or_si128( e4, m4 ) ) );
}

#ifdef _XM_AVX2_INTRINSICS_
float UnquantizeHalfFloat_F16C( unsigned short input )
{
    return _mm_cvtss_f32( _mm_cvtph_ps( _mm_cvtsi32_si128( input ) ) );
}

void UnquantizeHalfFloat_X4_F16C( unsigned short* input, float* output )
{
    _mm_store_ps( output, _mm_cvtph_ps( _mm_loadl_epi64( reinterpret_cast<const __m128i*>(input) ) ) );
}

void UnquantizeHalfFloat_X8_F16C( unsigned short* input, float* output )
{
    _mm256_store_ps( output, _mm256_cvtph_ps( _mm_load_si128( reinterpret_cast<const __m128i*>(input) ) ) );
}
#endif

struct ddraw_dll {
    HMODULE dll = NULL;
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
    FARPROC	DirectDrawCreateEx;
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
    FARPROC	UpdateCustomFontMultiplier;
    FARPROC	SetCustomSkyTexture;
    FARPROC	LoadMenuSettings;
} ddraw;

void WINAPI FallbackCheckFullscreen() noexcept {}

bool LoadSystemDirectDraw() noexcept {
    wchar_t systemPath[MAX_PATH]{};
    const UINT pathLength = GetSystemDirectoryW( systemPath, MAX_PATH );
    if ( pathLength == 0 || pathLength >= MAX_PATH
        || wcscat_s( systemPath, L"\\ddraw.dll" ) != 0 ) {
        return false;
    }

    ddraw_dll resolved{};
    resolved.dll = LoadLibraryW( systemPath );
    if ( !resolved.dll ) {
        return false;
    }

#define RESOLVE_SYSTEM_EXPORT(member) \
    resolved.member = GetProcAddress( resolved.dll, #member )
    RESOLVE_SYSTEM_EXPORT( AcquireDDThreadLock );
    RESOLVE_SYSTEM_EXPORT( CheckFullscreen );
    RESOLVE_SYSTEM_EXPORT( CompleteCreateSysmemSurface );
    RESOLVE_SYSTEM_EXPORT( D3DParseUnknownCommand );
    RESOLVE_SYSTEM_EXPORT( DDGetAttachedSurfaceLcl );
    RESOLVE_SYSTEM_EXPORT( DDInternalLock );
    RESOLVE_SYSTEM_EXPORT( DDInternalUnlock );
    RESOLVE_SYSTEM_EXPORT( DSoundHelp );
    RESOLVE_SYSTEM_EXPORT( DirectDrawCreate );
    RESOLVE_SYSTEM_EXPORT( DirectDrawCreateClipper );
    RESOLVE_SYSTEM_EXPORT( DirectDrawCreateEx );
    RESOLVE_SYSTEM_EXPORT( DirectDrawEnumerateA );
    RESOLVE_SYSTEM_EXPORT( DirectDrawEnumerateExA );
    RESOLVE_SYSTEM_EXPORT( DirectDrawEnumerateExW );
    RESOLVE_SYSTEM_EXPORT( DirectDrawEnumerateW );
    RESOLVE_SYSTEM_EXPORT( DllCanUnloadNow );
    RESOLVE_SYSTEM_EXPORT( DllGetClassObject );
    RESOLVE_SYSTEM_EXPORT( GetDDSurfaceLocal );
    RESOLVE_SYSTEM_EXPORT( GetOLEThunkData );
    RESOLVE_SYSTEM_EXPORT( GetSurfaceFromDC );
    RESOLVE_SYSTEM_EXPORT( RegisterSpecialCase );
    RESOLVE_SYSTEM_EXPORT( ReleaseDDThreadLock );
#undef RESOLVE_SYSTEM_EXPORT

    if ( !resolved.CheckFullscreen ) {
        resolved.CheckFullscreen = reinterpret_cast<FARPROC>(&FallbackCheckFullscreen);
    }

    if ( !resolved.DirectDrawCreate || !resolved.DirectDrawCreateClipper
        || !resolved.DirectDrawCreateEx || !resolved.DirectDrawEnumerateA
        || !resolved.DirectDrawEnumerateExA || !resolved.DirectDrawEnumerateExW
        || !resolved.DirectDrawEnumerateW ) {
        FreeLibrary( resolved.dll );
        return false;
    }

    ddraw = resolved;
    return true;
}

void InitializeComForCurrentThread() noexcept {
    static thread_local bool attempted = false;
    if ( attempted ) {
        return;
    }
    attempted = true;

    const HRESULT result = CoInitializeEx( nullptr, COINIT_APARTMENTTHREADED );
    if ( result == S_OK || result == S_FALSE ) {
        LogInfo() << "COM initialized for the render thread.";
    } else if ( result != RPC_E_CHANGED_MODE ) {
        LogWarn() << "COM initialization failed with code 0x"
            << std::hex << static_cast<unsigned long>(result) << ".";
    }
}

HRESULT DoHookedDirectDrawCreateEx( GUID FAR* lpGuid, LPVOID* lplpDD, REFIID iid, IUnknown FAR* pUnkOuter ) {
    (void)lpGuid;
    if ( !lplpDD ) {
        return E_POINTER;
    }
    *lplpDD = nullptr;
    if ( pUnkOuter ) {
        return CLASS_E_NOAGGREGATION;
    }
    if ( !IsEqualIID( iid, IID_IDirectDraw7 )
        && !IsEqualIID( iid, IID_IUnknown ) ) {
        return E_NOINTERFACE;
    }
    if ( !Engine::GAPI ) {
        return E_UNEXPECTED;
    }

    try {
        InitializeComForCurrentThread();
        if ( !Engine::GraphicsEngine ) {
            Engine::GAPI->OnGameStart();
            if ( Engine::CreateGraphicsEngine() != XR_SUCCESS ) {
                LogError() << "DirectDraw initialization aborted because the graphics engine failed.";
                return E_FAIL;
            }
        }

        auto* directDraw = new (std::nothrow) MyDirectDraw( nullptr );
        if ( !directDraw ) {
            return E_OUTOFMEMORY;
        }
        *lplpDD = directDraw;
        return S_OK;
    } catch ( const std::exception& error ) {
        LogError() << "DirectDraw initialization failed: " << error.what();
        return E_FAIL;
    } catch ( ... ) {
        LogError() << "DirectDraw initialization failed unexpectedly.";
        return E_FAIL;
    }
}

extern "C" HRESULT WINAPI HookedDirectDrawCreateEx( GUID FAR * lpGuid, LPVOID * lplpDD, REFIID  iid, IUnknown FAR * pUnkOuter ) {
    if ( Engine::PassThrough ) {
        return reinterpret_cast<DirectDrawCreateEx_type>(ddraw.DirectDrawCreateEx)( lpGuid, lplpDD, iid, pUnkOuter );
    }

    hook_infunc

        return DoHookedDirectDrawCreateEx( lpGuid, lplpDD, iid, pUnkOuter );

    hook_outfunc

    return E_FAIL;
}

extern "C" void WINAPI HookedAcquireDDThreadLock() {
    if ( Engine::PassThrough ) {
        reinterpret_cast<DirectDrawSimple>(ddraw.AcquireDDThreadLock)();
        return;
    }
    // The renderer does not use the legacy DirectDraw lock.
}

extern "C" void WINAPI HookedReleaseDDThreadLock() {
    if ( Engine::PassThrough ) {
        reinterpret_cast<DirectDrawSimple>(ddraw.ReleaseDDThreadLock)();
        return;
    }
    // The renderer does not use the legacy DirectDraw lock.
}

extern "C" float WINAPI UpdateCustomFontMultiplierFontRendering( float multiplier ) {
    // Using this function is unrecommended if you respect your players
    // there are a lot of players that don't play with GD3D11 mod
    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    return engine ? engine->UpdateCustomFontMultiplierFontRendering( multiplier ) : 1.0f;
}

extern "C" void WINAPI SetCustomCloudAndNightTexture( int idxTexture, bool isNightTexture ) {
    if ( !Engine::GAPI ) {
        return;
    }
    GSky* sky = Engine::GAPI->GetSky();
    WorldInfo* currentWorld = Engine::GAPI->GetLoadedWorldInfo();
    if ( sky && currentWorld ) {
        sky->SetCustomCloudAndNightTexture( idxTexture, isNightTexture, currentWorld->WorldName == "OLDWORLD" || currentWorld->WorldName == "WORLD" );
    }
}

extern "C" void WINAPI SetCustomSkyTexture_ZenGin( bool isNightTexture, zCTexture* texture ) {
    if ( !Engine::GAPI ) {
        return;
    }
    GSky* sky = Engine::GAPI->GetSky();
    WorldInfo* currentWorld = Engine::GAPI->GetLoadedWorldInfo();
    if ( sky && currentWorld ) {
        sky->SetCustomSkyTexture_ZenGin( isNightTexture, texture, currentWorld->WorldName == "OLDWORLD" || currentWorld->WorldName == "WORLD" );
    }
}

extern "C" void WINAPI SetCustomSkyWavelengths( float X, float Y, float Z ) {
    if ( !Engine::GAPI || !std::isfinite( X ) || !std::isfinite( Y ) || !std::isfinite( Z ) ) {
        return;
    }
    GSky* sky = Engine::GAPI->GetSky();
    if ( sky ) {
        sky->SetCustomSkyWavelengths( X, Y, Z );
    }
}

extern "C" void WINAPI LoadMenuSettings(char* menuSettingsFile) {
    if ( Engine::GAPI ) {
        Engine::GAPI->LoadMenuSettings( !menuSettingsFile ? MENU_SETTINGS_FILE : menuSettingsFile );
    }
}

extern "C" void WINAPI LoadCustomZENResources() {
    if ( Engine::GAPI ) {
        Engine::GAPI->LoadCustomZENResources();
    }
}

extern "C" void WINAPI EnableWindAnimations( void ) {
#if defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)
    haveWindAnimations = true;
#endif
}

__declspec(naked) void FakeAcquireDDThreadLock() { _asm { jmp[ddraw.AcquireDDThreadLock] } }
__declspec(naked) void FakeCheckFullscreen() { _asm { jmp[ddraw.CheckFullscreen] } }
__declspec(naked) void FakeCompleteCreateSysmemSurface() { _asm { jmp[ddraw.CompleteCreateSysmemSurface] } }
__declspec(naked) void FakeD3DParseUnknownCommand() { _asm { jmp[ddraw.D3DParseUnknownCommand] } }
__declspec(naked) void FakeDDGetAttachedSurfaceLcl() { _asm { jmp[ddraw.DDGetAttachedSurfaceLcl] } }
__declspec(naked) void FakeDDInternalLock() { _asm { jmp[ddraw.DDInternalLock] } }
__declspec(naked) void FakeDDInternalUnlock() { _asm { jmp[ddraw.DDInternalUnlock] } }
__declspec(naked) void FakeDSoundHelp() { _asm { jmp[ddraw.DSoundHelp] } }
// HRESULT WINAPI DirectDrawCreate(GUID FAR *lpGUID, LPDIRECTDRAW FAR *lplpDD, IUnknown FAR *pUnkOuter);
__declspec(naked) void FakeDirectDrawCreate() { _asm { jmp[ddraw.DirectDrawCreate] } }
// HRESULT WINAPI DirectDrawCreateClipper(DWORD dwFlags, LPDIRECTDRAWCLIPPER FAR *lplpDDClipper, IUnknown FAR *pUnkOuter);
__declspec(naked) void FakeDirectDrawCreateClipper() { _asm { jmp[ddraw.DirectDrawCreateClipper] } }
// HRESULT WINAPI DirectDrawCreateEx(GUID FAR * lpGuid, LPVOID *lplpDD, REFIID iid,IUnknown FAR *pUnkOuter);
__declspec(naked) void FakeDirectDrawCreateEx() { _asm { jmp[ddraw.DirectDrawCreateEx] } }
// HRESULT WINAPI DirectDrawEnumerateA(LPDDENUMCALLBACKA lpCallback, LPVOID lpContext);

static char FakeDirectDrawEnumerateA_deviceName[] = "DirectX11";

HRESULT WINAPI FakeDirectDrawEnumerateA( LPDDENUMCALLBACKA lpCallback, LPVOID lpContext )
{
    if ( !lpCallback ) {
        return DDERR_INVALIDPARAMS;
    }
    GUID deviceGUID = { 0xF5049E78, 0x4861, 0x11D2, {0xA4, 0x07, 0x00, 0xA0, 0xC9, 0x06, 0x29, 0xA8} };
    lpCallback( &deviceGUID, FakeDirectDrawEnumerateA_deviceName, FakeDirectDrawEnumerateA_deviceName, lpContext );
    return S_OK;
}
// HRESULT WINAPI DirectDrawEnumerateExA(LPDDENUMCALLBACKEXA lpCallback, LPVOID lpContext, DWORD dwFlags);
HRESULT WINAPI FakeDirectDrawEnumerateExA( LPDDENUMCALLBACKEXA lpCallback, LPVOID lpContext, DWORD dwFlags )
{
    (void)dwFlags;
    if ( !lpCallback ) {
        return DDERR_INVALIDPARAMS;
    }
    GUID deviceGUID = { 0xF5049E78, 0x4861, 0x11D2, {0xA4, 0x07, 0x00, 0xA0, 0xC9, 0x06, 0x29, 0xA8} };
    lpCallback( &deviceGUID, FakeDirectDrawEnumerateA_deviceName, FakeDirectDrawEnumerateA_deviceName, lpContext, nullptr );
    return S_OK;
}
// HRESULT WINAPI DirectDrawEnumerateExW(LPDDENUMCALLBACKEXW lpCallback, LPVOID lpContext, DWORD dwFlags);
__declspec(naked) void FakeDirectDrawEnumerateExW() { _asm { jmp[ddraw.DirectDrawEnumerateExW] } }
// HRESULT WINAPI DirectDrawEnumerateW(LPDDENUMCALLBACKW lpCallback, LPVOID lpContext);
__declspec(naked) void FakeDirectDrawEnumerateW() { _asm { jmp[ddraw.DirectDrawEnumerateW] } }
__declspec(naked) void FakeDllCanUnloadNow() { _asm { jmp[ddraw.DllCanUnloadNow] } }
__declspec(naked) void FakeDllGetClassObject() { _asm { jmp[ddraw.DllGetClassObject] } }
__declspec(naked) void FakeGetDDSurfaceLocal() { _asm { jmp[ddraw.GetDDSurfaceLocal] } }
__declspec(naked) void FakeGetOLEThunkData() { _asm { jmp[ddraw.GetOLEThunkData] } }
__declspec(naked) void FakeGetSurfaceFromDC() { _asm { jmp[ddraw.GetSurfaceFromDC] } }
__declspec(naked) void FakeRegisterSpecialCase() { _asm { jmp[ddraw.RegisterSpecialCase] } }
__declspec(naked) void FakeReleaseDDThreadLock() { _asm { jmp[ddraw.ReleaseDDThreadLock] } }

bool SetupWorkingDirectory() noexcept {
    wchar_t executablePath[MAX_PATH]{};
    const DWORD pathLength = GetModuleFileNameW( nullptr, executablePath, MAX_PATH );
    return pathLength > 0 && pathLength < MAX_PATH
        && PathRemoveFileSpecW( executablePath )
        && SetCurrentDirectoryW( executablePath );
}

void EnableCrashingOnCrashes() {
    typedef BOOL( WINAPI* tGetPolicy )(LPDWORD lpFlags);
    typedef BOOL( WINAPI* tSetPolicy )(DWORD dwFlags);
    const DWORD EXCEPTION_SWALLOWING = 0x1;

    HMODULE kernel32 = GetModuleHandleW( L"kernel32.dll" );
    if ( kernel32 ) {
        tGetPolicy pGetPolicy = (tGetPolicy)GetProcAddress( kernel32,
            "GetProcessUserModeExceptionPolicy" );
        tSetPolicy pSetPolicy = (tSetPolicy)GetProcAddress( kernel32,
            "SetProcessUserModeExceptionPolicy" );
        if ( pGetPolicy && pSetPolicy ) {
            DWORD dwFlags;
            if ( pGetPolicy( &dwFlags ) ) {
                // Turn off the filter
                pSetPolicy( dwFlags & ~EXCEPTION_SWALLOWING );
            }
        }
    }
}

bool CheckPlatformSupport() {
    LogInstructionSet();
    auto requireFeature = []( const char* feature, bool supported ) {
        if ( supported ) {
            return true;
        }
        ErrorBox( (std::string( "Incompatible system or wrong renderer DLL.\n\n" )
            + feature + " is required but unavailable on:\n" + InstructionSet::Brand()).c_str() );
        return false;
    };

#if __AVX2__
    if ( !requireFeature( "AVX2", InstructionSet::AVX2() ) ) return false;
#elif __AVX__
    if ( !requireFeature( "AVX", InstructionSet::AVX() ) ) return false;
#elif __SSE2__
    if ( !requireFeature( "SSE2", InstructionSet::SSE2() ) ) return false;
#elif __SSE__
    if ( !requireFeature( "SSE", InstructionSet::SSE() ) ) return false;
#endif

#ifdef _XM_AVX2_INTRINSICS_
    if ( InstructionSet::F16C() ) {
        QuantizeHalfFloat = QuantizeHalfFloat_F16C;
        QuantizeHalfFloat_X4 = QuantizeHalfFloats_X4_F16C;
        UnquantizeHalfFloat = UnquantizeHalfFloat_F16C;
        UnquantizeHalfFloat_X4 = UnquantizeHalfFloat_X4_F16C;
        UnquantizeHalfFloat_X8 = UnquantizeHalfFloat_X8_F16C;
    } else
#endif
    if ( InstructionSet::SSE41() ) {
        QuantizeHalfFloat = QuantizeHalfFloat_Scalar;
        QuantizeHalfFloat_X4 = QuantizeHalfFloats_X4_SSE41;
        UnquantizeHalfFloat = UnquantizeHalfFloat_Scalar;
        UnquantizeHalfFloat_X4 = UnquantizeHalfFloat_X4_SSE2;
        UnquantizeHalfFloat_X8 = UnquantizeHalfFloat_X8_SSE2;
    } else {
        QuantizeHalfFloat = QuantizeHalfFloat_Scalar;
        QuantizeHalfFloat_X4 = QuantizeHalfFloats_X4_SSE2;
        UnquantizeHalfFloat = UnquantizeHalfFloat_Scalar;
        UnquantizeHalfFloat_X4 = UnquantizeHalfFloat_X4_SSE2;
        UnquantizeHalfFloat_X8 = UnquantizeHalfFloat_X8_SSE2;
    }
    return true;
}

#if defined(BUILD_GOTHIC_2_6_fix)
int WINAPI hooked_WinMain( HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd ) {
    if ( GetModuleHandleA( "gmp.dll" ) ) {
        GMPModeActive = true;
        LogInfo() << "GMP Mode Enabled";
    }
    // Remove automatic volume change of sounds regarding whether the camera is indoor or outdoor
    // TODO: Implement!
    if ( !GMPModeActive ) {
        const LONG beginResult = DetourTransactionBegin();
        if ( beginResult == ERROR_SUCCESS ) {
            const LONG attachResult = DetourAttachTyped(
                &HookedFunctions::OriginalFunctions.original_zCActiveSndAutoCalcObstruction,
                HookedFunctionInfo::hooked_zCActiveSndAutoCalcObstruction );
            if ( attachResult == ERROR_SUCCESS ) {
                const LONG commitResult = DetourTransactionCommit();
                if ( commitResult != ERROR_SUCCESS ) {
                    LogError() << "Failed to commit sound obstruction hook. Error " << commitResult << ".";
                }
            } else {
                DetourTransactionAbort();
                LogError() << "Failed to attach sound obstruction hook. Error " << attachResult << ".";
            }
        } else {
            LogError() << "Failed to begin sound obstruction hook transaction. Error " << beginResult << ".";
        }
    }
    return originalWinMain( hInstance, hPrevInstance, lpCmdLine, nShowCmd );
}
#endif

[[noreturn]] void badAllocationHandler() noexcept
{
    std::set_new_handler( nullptr );

    bool largeAddressAware = true;
    const auto* codeBase = reinterpret_cast<const BYTE*>(GetModuleHandleW( nullptr ));
    if ( codeBase ) {
        const auto* dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(codeBase);
        if ( dosHeader->e_magic == IMAGE_DOS_SIGNATURE
            && dosHeader->e_lfanew > 0 && dosHeader->e_lfanew <= 1024 * 1024 ) {
            const auto* ntHeader = reinterpret_cast<const IMAGE_NT_HEADERS*>(codeBase + dosHeader->e_lfanew);
            if ( ntHeader->Signature == IMAGE_NT_SIGNATURE ) {
                largeAddressAware =
                    (ntHeader->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0;
            }
        }
    }

    const char* message = "Allocation failed due to running out of memory or virtual address space.";
    if ( !largeAddressAware ) {
        message =
            "Allocation failed due to running out of memory or virtual address space.\n\n"
            "The executable is not Large Address Aware. Apply a 4 GB patch before running GD3D11.";
    } else if ( userHaveAMDGPU ) {
        message =
            "Allocation failed due to running out of memory or virtual address space.\n\n"
            "AMD drivers can add substantial 32-bit address-space overhead. "
            "Using 32-bit DXVK may reduce that overhead.";
    }

    while ( ShowCursor( TRUE ) < 0 ) {}
    MessageBoxA( nullptr, message, "Gothic GD3D11", MB_OK | MB_ICONERROR | MB_TOPMOST );
    TerminateProcess( GetCurrentProcess(), ERROR_NOT_ENOUGH_MEMORY );
    std::abort();
}

BOOL WINAPI DllMain( HINSTANCE hInst, DWORD reason, LPVOID ) {
    if ( DetourIsHelperProcess() ) {
        return TRUE;
    }
    if ( reason != DLL_PROCESS_ATTACH ) {
        return TRUE;
    }

    DisableThreadLibraryCalls( hInst );
    bool detourTransactionOpen = false;
    try {
        if ( !SetupWorkingDirectory() ) {
            MessageBoxA( nullptr, "GD3D11 could not set the game working directory.",
                "Gothic GD3D11", MB_ICONERROR );
            return FALSE;
        }

        std::set_new_handler( badAllocationHandler );
        Log::Clear();
        LogInfo() << "Starting DDRAW renderer DLL.";

        if ( !CheckPlatformSupport() ) {
            return FALSE;
        }
        if ( !LoadSystemDirectDraw() ) {
            LogErrorBox() << "Failed to load or validate the system DirectDraw DLL.";
            return FALSE;
        }

        Engine::GAPI = nullptr;
        Engine::GraphicsEngine = nullptr;
        Engine::ImGuiHandle = nullptr;
        Engine::RenderingThreadPool = nullptr;
        Engine::WorkerThreadPool = nullptr;
        Engine::PassThrough = !VersionCheck::CheckExecutable();

        if ( !Engine::PassThrough ) {
            DetourRestoreAfterWith();
            if ( DetourTransactionBegin() != ERROR_SUCCESS ) {
                LogError() << "Failed to begin hook transaction.";
                return FALSE;
            }
            detourTransactionOpen = true;

#if defined(BUILD_GOTHIC_2_6_fix)
            if ( DetourAttachTyped( &originalWinMain, hooked_WinMain ) != ERROR_SUCCESS ) {
                DetourTransactionAbort();
                detourTransactionOpen = false;
                LogError() << "Failed to attach WinMain hook.";
                return FALSE;
            }
#endif

            if ( !GothicPatching::BeginPatchTransaction() ) {
                DetourTransactionAbort();
                detourTransactionOpen = false;
                LogError() << "Failed to begin Gothic memory patch transaction. Error "
                    << GothicPatching::GetPatchStatus() << ".";
                return FALSE;
            }

            if ( Engine::CreateGothicAPI() != XR_SUCCESS ) {
                GothicPatching::AbortPatchTransaction();
                DetourTransactionAbort();
                detourTransactionOpen = false;
                return FALSE;
            }

            const LONG hookResult = HookedFunctions::OriginalFunctions.InitHooks();
            if ( hookResult != ERROR_SUCCESS ) {
                GothicPatching::AbortPatchTransaction();
                DetourTransactionAbort();
                detourTransactionOpen = false;
                LogErrorBox() << "Failed to prepare renderer hooks. Error "
                    << hookResult << ".";
                return FALSE;
            }

            if ( !GothicPatching::CommitPatchTransaction() ) {
                const LONG patchStatus = GothicPatching::GetPatchStatus();
                const uintptr_t failedAddress =
                    GothicPatching::GetFirstFailureAddress();
                GothicPatching::AbortPatchTransaction();
                DetourTransactionAbort();
                detourTransactionOpen = false;
                LogErrorBox() << "Failed to apply Gothic memory patch at 0x"
                    << std::hex << failedAddress << ". Error "
                    << std::dec << patchStatus << ".";
                return FALSE;
            }

            const LONG commitResult = DetourTransactionCommit();
            detourTransactionOpen = false;
            if ( commitResult != ERROR_SUCCESS ) {
                GothicPatching::RollbackPatchTransaction();
                LogErrorBox() << "Failed to commit renderer hooks. Error "
                    << commitResult << ".";
                return FALSE;
            }
            GothicPatching::FinalizePatchTransaction();

            EnableCrashingOnCrashes();
        }

        return TRUE;
    } catch ( const std::exception& error ) {
        GothicPatching::AbortPatchTransaction();
        if ( detourTransactionOpen ) {
            DetourTransactionAbort();
        }
        LogError() << "Renderer DLL initialization failed: " << error.what();
        return FALSE;
    } catch ( ... ) {
        GothicPatching::AbortPatchTransaction();
        if ( detourTransactionOpen ) {
            DetourTransactionAbort();
        }
        LogError() << "Renderer DLL initialization failed unexpectedly.";
        return FALSE;
    }
}
