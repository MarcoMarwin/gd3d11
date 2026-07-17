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

static HINSTANCE hLThis = 0;
static bool comInitialized = false;
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

HRESULT DoHookedDirectDrawCreateEx( GUID FAR* lpGuid, LPVOID* lplpDD, REFIID iid, IUnknown FAR* pUnkOuter ) {
    (void)lpGuid;
    (void)iid;
    (void)pUnkOuter;

    *lplpDD = new MyDirectDraw( nullptr );

    if ( !Engine::GraphicsEngine ) {
        Engine::GAPI->OnGameStart();
        Engine::CreateGraphicsEngine();
    }

    return S_OK;
}

extern "C" HRESULT WINAPI HookedDirectDrawCreateEx( GUID FAR * lpGuid, LPVOID * lplpDD, REFIID  iid, IUnknown FAR * pUnkOuter ) {
    if ( Engine::PassThrough ) {
        return reinterpret_cast<DirectDrawCreateEx_type>(ddraw.DirectDrawCreateEx)( lpGuid, lplpDD, iid, pUnkOuter );
    }

    hook_infunc

        return DoHookedDirectDrawCreateEx( lpGuid, lplpDD, iid, pUnkOuter );

    hook_outfunc

    return S_OK;
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

    if ( reason == DLL_PROCESS_ATTACH ) {
        DetourRestoreAfterWith();
        DetourTransactionBegin();

        std::set_new_handler( badAllocationHandler );
        hLThis = hInst;

        Engine::PassThrough = false;

#if defined(BUILD_GOTHIC_2_6_fix)
        DetourAttachTyped( &originalWinMain, hooked_WinMain );
#endif

        SetupWorkingDirectory();
        if ( !Engine::PassThrough ) {
            Log::Clear();
            LogInfo() << "Starting DDRAW Proxy DLL.";

            HRESULT hr = CoInitializeEx( NULL, COINIT_APARTMENTTHREADED );
            if ( hr == RPC_E_CHANGED_MODE ) {
                hr = CoInitializeEx( NULL, COINIT_MULTITHREADED );
            }

            if ( hr == S_FALSE || hr == S_OK ) {
                comInitialized = true;
                LogInfo() << "COM initialized";
            }

            ZoneScoped;

            VersionCheck::CheckExecutable();
            if ( !CheckPlatformSupport() ) {
                DetourTransactionAbort();
                return FALSE;
            }

            Engine::GAPI = nullptr;
            Engine::GraphicsEngine = nullptr;
            Engine::ImGuiHandle = nullptr;
            Engine::RenderingThreadPool = nullptr;
            Engine::WorkerThreadPool = nullptr;

            Engine::CreateGothicAPI();
            HookedFunctions::OriginalFunctions.InitHooks();

            EnableCrashingOnCrashes();
        }
        DetourTransactionCommit();

        char dllBuf[MAX_PATH];
        GetSystemDirectoryA( dllBuf, MAX_PATH );
        strcat_s( dllBuf, MAX_PATH, "\\ddraw.dll" );

        ddraw.dll = LoadLibraryA( dllBuf );
        if ( !ddraw.dll ) return FALSE;

        ddraw.AcquireDDThreadLock = GetProcAddress( ddraw.dll, "AcquireDDThreadLock" );
        ddraw.CheckFullscreen = GetProcAddress( ddraw.dll, "CheckFullscreen" );
        if ( !ddraw.CheckFullscreen ) {
            ddraw.CheckFullscreen = reinterpret_cast<FARPROC>(&FallbackCheckFullscreen);
        }
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
    } else if ( reason == DLL_PROCESS_DETACH ) {
        Engine::OnShutDown();

        if ( comInitialized ) {
            comInitialized = false;
            CoUninitialize();
        }
        if ( ddraw.dll ) {
            FreeLibrary( ddraw.dll );
        }

        LogInfo() << "DDRAW Proxy DLL signing off.\n";
    }
    return TRUE;
}
