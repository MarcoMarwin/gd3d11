#pragma once
#include "../pch.h"
#include "MyDirect3D7.h"
#include "MyDirectDrawSurface7.h"
#include <comdef.h>
#include <atomic>
#include <new>
#include <limits>
#include "FakeDirectDrawSurface7.h"
#include "MyClipper.h"

class MyDirectDraw final : public IDirectDraw7 {
public:
	MyDirectDraw( IDirectDraw7* directDraw7 )
		: directDraw7( directDraw7 ), RefCount( 1 ) {
		DebugWrite( "MyDirectDraw::MyDirectDraw\n" );
		if ( directDraw7 ) directDraw7->AddRef();

		ZeroMemory( &DisplayMode, sizeof( DDSURFACEDESC2 ) );

		// Gothic calls GetDisplayMode without Setting it first, so do it here
		SetDisplayMode( 800, 600, 32, 60, 0 );
	}

	~MyDirectDraw() {
		if ( directDraw7 ) directDraw7->Release();
	}

	/*** IUnknown methods ***/
	HRESULT __declspec(nothrow) STDMETHODCALLTYPE QueryInterface( REFIID riid, void** ppvObj ) override {
		DebugWrite( "MyDirectDraw::QueryInterface\n" );
		if ( !ppvObj ) return E_POINTER;
		*ppvObj = nullptr;

		if ( IsEqualIID( riid, IID_IUnknown )
			|| IsEqualIID( riid, IID_IDirectDraw7 ) ) {
			*ppvObj = static_cast<IDirectDraw7*>(this);
			AddRef();
			return S_OK;
		}
		if ( IsEqualIID( riid, IID_IDirect3D7 ) ) {
			auto* direct3D = new (std::nothrow) MyDirect3D7( nullptr );
			if ( !direct3D ) return E_OUTOFMEMORY;
			*ppvObj = static_cast<IDirect3D7*>(direct3D);
			return S_OK;
		}
		return E_NOINTERFACE;
	}

	ULONG __declspec(nothrow) STDMETHODCALLTYPE AddRef() override {
		DebugWrite( "MyDirectDraw::AddRef\n" );
		return RefCount.fetch_add( 1, std::memory_order_relaxed ) + 1;
	}

	ULONG __declspec(nothrow) STDMETHODCALLTYPE Release() override {
		DebugWrite( "MyDirectDraw::Release\n" );
		const ULONG references =
			RefCount.fetch_sub( 1, std::memory_order_acq_rel ) - 1;
		if ( references == 0 ) {
			delete this;
		}
		return references;
	}

	/*** IDirectDraw7 methods ***/
	HRESULT __declspec(nothrow) STDMETHODCALLTYPE GetAvailableVidMem( LPDDSCAPS2 lpDDSCaps2, LPDWORD lpdwTotal, LPDWORD lpdwFree ) override {
		DebugWrite( "MyDirectDraw::GetAvailableVidMem\n" );
		(void)lpDDSCaps2;
		constexpr DWORD reportedTotal = 1024u * 1024u * 1024u;
		if ( lpdwTotal ) *lpdwTotal = reportedTotal;
		if ( lpdwFree ) *lpdwFree = reportedTotal;
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE GetCaps( LPDDCAPS lpDDDriverCaps, LPDDCAPS lpDDHELCaps ) override {
		DebugWrite( "MyDirectDraw::GetCaps\n" );
		if ( !lpDDDriverCaps && !lpDDHELCaps ) return DDERR_INVALIDPARAMS;
		if ( lpDDDriverCaps ) {
			ZeroMemory( lpDDDriverCaps, sizeof(DDCAPS) );
			lpDDDriverCaps->dwSize = sizeof(DDCAPS);
		}
		if ( lpDDHELCaps ) {
			ZeroMemory( lpDDHELCaps, sizeof(DDCAPS) );
			lpDDHELCaps->dwSize = sizeof(DDCAPS);
		}
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE SetCooperativeLevel( HWND hWnd, DWORD dwFlags ) override {
		DebugWrite( "MyDirectDraw::SetCooperativeLevel\n" );
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE GetDeviceIdentifier( LPDDDEVICEIDENTIFIER2 lpdddi, DWORD dwFlags ) override {
		DebugWrite( "MyDirectDraw::GetDeviceIdentifier\n" );
		(void)dwFlags;
		if ( !lpdddi ) return E_POINTER;

		ZeroMemory( lpdddi, sizeof(DDDEVICEIDENTIFIER2) );
		const char* description = Engine::GraphicsEngine
			? Engine::GraphicsEngine->GetGraphicsDeviceName().c_str()
			: "DirectX11";
		strncpy_s( lpdddi->szDescription, description, _TRUNCATE );
		strncpy_s( lpdddi->szDriver, "DirectX11", _TRUNCATE );
		lpdddi->guidDeviceIdentifier = { 0xF5049E78, 0x4861, 0x11D2, {0xA4, 0x07, 0x00, 0xA0, 0xC9, 0x06, 0x29, 0xA8} };
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE GetDisplayMode( LPDDSURFACEDESC2 lpDDSurfaceDesc2 ) override {
		DebugWrite( "MyDirectDraw::GetDisplayMode\n" );
		if ( !lpDDSurfaceDesc2 ) return E_POINTER;
		*lpDDSurfaceDesc2 = DisplayMode;
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE SetDisplayMode( DWORD dwWidth, DWORD dwHeight, DWORD dwBPP, DWORD dwRefreshRate, DWORD dwFlags ) override {
		DebugWrite( "MyDirectDraw::SetDisplayMode\n" );

		DisplayMode.dwWidth = dwWidth;
		DisplayMode.dwHeight = dwHeight;
		DisplayMode.dwRefreshRate = dwRefreshRate;
		DisplayMode.dwFlags = dwFlags;

		DisplayMode.ddpfPixelFormat.dwRGBBitCount = dwBPP;
		DisplayMode.ddpfPixelFormat.dwPrivateFormatBitCount = dwBPP;
		DisplayMode.dwFlags |= DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT;
		DisplayMode.dwSize = sizeof( DisplayMode );
		DisplayMode.ddpfPixelFormat.dwSize = sizeof( DisplayMode.ddpfPixelFormat );

		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE GetFourCCCodes( LPDWORD lpNumCodes, LPDWORD lpCodes ) override {
		DebugWrite( "MyDirectDraw::GetFourCCCodes\n" );
		(void)lpCodes;
		if ( !lpNumCodes ) return E_POINTER;
		*lpNumCodes = 0;
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE GetGDISurface( LPDIRECTDRAWSURFACE7 FAR* lplpGDIDDSSurface ) override {
		DebugWrite( "MyDirectDraw::GetGDISurface\n" );
		if ( !lplpGDIDDSSurface ) return E_POINTER;
		*lplpGDIDDSSurface = nullptr;
		return DDERR_NOTFOUND;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE GetMonitorFrequency( LPDWORD lpdwFrequency ) override {
		DebugWrite( "MyDirectDraw::GetMonitorFrequency\n" );
		if ( !lpdwFrequency ) return E_POINTER;
		*lpdwFrequency = 60;
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE GetScanLine( LPDWORD lpdwScanLine ) override {
		DebugWrite( "MyDirectDraw::GetScanLine\n" );
		if ( !lpdwScanLine ) return E_POINTER;
		*lpdwScanLine = 0;
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE GetSurfaceFromDC( HDC hdc, LPDIRECTDRAWSURFACE7* lpDDS ) override {
		DebugWrite( "MyDirectDraw::GetSurfaceFromDC\n" );
		(void)hdc;
		if ( !lpDDS ) return E_POINTER;
		*lpDDS = nullptr;
		return DDERR_NOTFOUND;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE GetVerticalBlankStatus( LPBOOL lpbIsInVB ) override {
		DebugWrite( "MyDirectDraw::GetVerticalBlankStatus\n" );
		if ( !lpbIsInVB ) return E_POINTER;
		*lpbIsInVB = FALSE;
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE Compact() override {
		DebugWrite( "MyDirectDraw::Compact\n" );
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE CreateClipper( DWORD dwFlags, LPDIRECTDRAWCLIPPER FAR* lplpDDClipper, IUnknown FAR* pUnkOuter ) override {
		DebugWrite( "MyDirectDraw::CreateClipper\n" );
		(void)dwFlags;
		if ( !lplpDDClipper ) return E_POINTER;
		*lplpDDClipper = nullptr;
		if ( pUnkOuter ) return CLASS_E_NOAGGREGATION;

		auto* clipper = new (std::nothrow) MyClipper;
		if ( !clipper ) return E_OUTOFMEMORY;
		*lplpDDClipper = clipper;
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE CreatePalette( DWORD dwFlags, LPPALETTEENTRY lpDDColorArray, LPDIRECTDRAWPALETTE FAR* lplpDDPalette, IUnknown FAR* pUnkOuter ) override {
		DebugWrite( "MyDirectDraw::CreatePalette\n" );
		(void)dwFlags;
		(void)lpDDColorArray;
		if ( !lplpDDPalette ) return E_POINTER;
		*lplpDDPalette = nullptr;
		if ( pUnkOuter ) return CLASS_E_NOAGGREGATION;
		return DDERR_UNSUPPORTED;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE CreateSurface( LPDDSURFACEDESC2 lpDDSurfaceDesc2, LPDIRECTDRAWSURFACE7 FAR* lplpDDSurface, IUnknown FAR* pUnkOuter ) override {
		DebugWrite( "MyDirectDraw::CreateSurface\n" );
		if ( !lplpDDSurface ) return E_POINTER;
		*lplpDDSurface = nullptr;
		if ( !lpDDSurfaceDesc2 ) return DDERR_INVALIDPARAMS;
		if ( pUnkOuter ) return CLASS_E_NOAGGREGATION;
		if ( lpDDSurfaceDesc2->dwWidth
			> static_cast<DWORD>((std::numeric_limits<int>::max)())
			|| lpDDSurfaceDesc2->dwHeight
				> static_cast<DWORD>((std::numeric_limits<int>::max)())
			|| ((lpDDSurfaceDesc2->ddsCaps.dwCaps & DDSCAPS_MIPMAP)
				&& lpDDSurfaceDesc2->dwMipMapCount > 32) ) {
			return DDERR_INVALIDPARAMS;
		}

		if ( lpDDSurfaceDesc2->ddsCaps.dwCaps & DDSCAPS_OFFSCREENPLAIN ) {
			LogInfo() << "Forcing DDSCAPS_OFFSCREENPLAIN-Surface to 24-Bit";
			// Set up the pixel format for 24-bit RGB (8-8-8).
			lpDDSurfaceDesc2->ddpfPixelFormat.dwSize = sizeof( DDPIXELFORMAT );
			lpDDSurfaceDesc2->ddpfPixelFormat.dwFlags = DDPF_RGB;
			lpDDSurfaceDesc2->ddpfPixelFormat.dwRGBBitCount = 24;
			lpDDSurfaceDesc2->ddpfPixelFormat.dwRBitMask = 0x00FF0000;
			lpDDSurfaceDesc2->ddpfPixelFormat.dwGBitMask = 0x0000FF00;
			lpDDSurfaceDesc2->ddpfPixelFormat.dwBBitMask = 0x000000FF;
		}

        // Check potential texture conversions
        if ( lpDDSurfaceDesc2->ddpfPixelFormat.dwRGBBitCount == 16 ) {
            if ( lpDDSurfaceDesc2->ddpfPixelFormat.dwRBitMask == 0x7C00
                && lpDDSurfaceDesc2->ddpfPixelFormat.dwGBitMask == 0x3E0
                && lpDDSurfaceDesc2->ddpfPixelFormat.dwBBitMask == 0x1F
                && lpDDSurfaceDesc2->ddpfPixelFormat.dwRGBAlphaBitMask == 0x8000 )
                lpDDSurfaceDesc2->ddpfPixelFormat.dwFourCC = 1;
            else if ( lpDDSurfaceDesc2->ddpfPixelFormat.dwRBitMask == 0xF00
                && lpDDSurfaceDesc2->ddpfPixelFormat.dwGBitMask == 0xF0
                && lpDDSurfaceDesc2->ddpfPixelFormat.dwBBitMask == 0x0F
                && lpDDSurfaceDesc2->ddpfPixelFormat.dwRGBAlphaBitMask == 0xF000 )
                lpDDSurfaceDesc2->ddpfPixelFormat.dwFourCC = 2;
            else
                lpDDSurfaceDesc2->ddpfPixelFormat.dwFourCC = 0;
        }

		// Create surface
		auto* mySurface = new (std::nothrow) MyDirectDrawSurface7();
		if ( !mySurface ) return E_OUTOFMEMORY;

		// Create a fake mipmap chain if needed
		if ( lpDDSurfaceDesc2->ddsCaps.dwCaps & DDSCAPS_MIPMAP ) {
			DDSURFACEDESC2 desc = *lpDDSurfaceDesc2;
			FakeDirectDrawSurface7* lastMip = nullptr;
			int level = 1;
			while ( desc.dwMipMapCount > 1 ) {
				auto* mip = new (std::nothrow) FakeDirectDrawSurface7;
				if ( !mip ) {
					mySurface->Release();
					return E_OUTOFMEMORY;
				}
				--desc.dwMipMapCount;
				desc.ddsCaps.dwCaps2 |= DDSCAPS2_MIPMAPSUBLEVEL;
				mip->InitFakeSurface( &desc, mySurface, level );

				const HRESULT attachResult = lastMip
					? lastMip->AddAttachedSurface( mip )
					: mySurface->AddAttachedSurface( mip );
				if ( FAILED( attachResult ) ) {
					mip->Release();
					mySurface->Release();
					return attachResult;
				}
				lastMip = mip;
				mip->Release();
				++level;
			}
		}

		const HRESULT surfaceResult =
			mySurface->SetSurfaceDesc( lpDDSurfaceDesc2, 0 );
		if ( FAILED( surfaceResult ) ) {
			mySurface->Release();
			return surfaceResult;
		}

		*lplpDDSurface = mySurface;
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE DuplicateSurface( LPDIRECTDRAWSURFACE7 lpDDSurface, LPDIRECTDRAWSURFACE7 FAR* lplpDupDDSurface ) override {
		DebugWrite( "MyDirectDraw::DuplicateSurface\n" );
		(void)lpDDSurface;
		if ( !lplpDupDDSurface ) return E_POINTER;
		*lplpDupDDSurface = nullptr;
		return DDERR_UNSUPPORTED;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE EnumDisplayModes( DWORD dwFlags, LPDDSURFACEDESC2 lpDDSurfaceDesc2, LPVOID lpContext, LPDDENUMMODESCALLBACK2 lpEnumModesCallback ) override {
		DebugWrite( "MyDirectDraw::EnumDisplayModes\n" );
		(void)lpDDSurfaceDesc2;
		if ( !lpEnumModesCallback ) return DDERR_INVALIDPARAMS;
		if ( !Engine::GraphicsEngine ) return DDERR_GENERIC;

		try {
		std::vector<DisplayModeInfo> modes;
		Engine::GraphicsEngine->GetDisplayModeList( &modes );

        // Gothic expects 640x480 and 800x600 resolutions to be available
        // otherwise it results in D3DXERR_CAPSNOTSUPPORTED error
        // if this device don't have those resolutions report them anyway

        INT2 currentResolution = Engine::GraphicsEngine->GetResolution( );
        bool have640x480 = false, have800x600 = false, haveCurrentResolution = false;
        for ( DisplayModeInfo& mode : modes ) {
            if ( mode.Width == 640 && mode.Height == 480 )
                have640x480 = true;
            if ( mode.Width == 800 && mode.Height == 600 )
                have800x600 = true;
            if ( mode.Width == static_cast<DWORD>(currentResolution.x) && mode.Height == static_cast<DWORD>(currentResolution.y) )
                haveCurrentResolution = true;
        }

        if ( !haveCurrentResolution ) {
            DisplayModeInfo info{};
            info.Width = static_cast<DWORD>(currentResolution.x);
            info.Height = static_cast<DWORD>(currentResolution.y);
            modes.insert( modes.begin(), info );
        }
        if ( !have800x600 ) {
            DisplayModeInfo info{};
            info.Width = 800;
            info.Height = 600;
            modes.insert( modes.begin(), info );
        }
        if ( !have640x480 ) {
            DisplayModeInfo info{};
            info.Width = 640;
            info.Height = 480;
            modes.insert( modes.begin(), info );
        }

		for ( DisplayModeInfo& mode : modes ) {
			DDSURFACEDESC2 desc;
			ZeroMemory( &desc, sizeof( desc ) );

            desc.dwSize = sizeof( DDSURFACEDESC2 );
            desc.dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
            desc.dwWidth = mode.Width;
			desc.dwHeight = mode.Height;
            desc.ddpfPixelFormat.dwSize = sizeof( DDPIXELFORMAT );
            desc.ddpfPixelFormat.dwFlags = DDPF_RGB;
			desc.ddpfPixelFormat.dwRGBBitCount = 32;
            if ( dwFlags & DDEDM_REFRESHRATES ) {
                desc.dwFlags |= DDSD_REFRESHRATE;
                desc.dwRefreshRate = 60;
            }

			if ( (*lpEnumModesCallback)(&desc, lpContext)
				== DDENUMRET_CANCEL ) {
				break;
			}
		}

		return S_OK;
		} catch ( const std::bad_alloc& ) {
			return E_OUTOFMEMORY;
		} catch ( ... ) {
			return DDERR_GENERIC;
		}
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE EnumSurfaces( DWORD dwFlags, LPDDSURFACEDESC2 lpDDSD2, LPVOID lpContext, LPDDENUMSURFACESCALLBACK7 lpEnumSurfacesCallback ) override {
		DebugWrite( "MyDirectDraw::EnumSurfaces\n" );
		(void)dwFlags;
		(void)lpDDSD2;
		(void)lpContext;
		if ( !lpEnumSurfacesCallback ) return DDERR_INVALIDPARAMS;
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE EvaluateMode( DWORD dwFlags, DWORD* pSecondsUntilTimeout ) override {
		DebugWrite( "MyDirectDraw::EvaluateMode\n" );
		(void)dwFlags;
		if ( pSecondsUntilTimeout ) *pSecondsUntilTimeout = 0;
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE FlipToGDISurface() override {
		DebugWrite( "MyDirectDraw::FlipToGDISurface\n" );
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE Initialize( GUID FAR* lpGUID ) override {
		DebugWrite( "MyDirectDraw::Initialize\n" );
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE RestoreDisplayMode() override {
		DebugWrite( "MyDirectDraw::RestoreDisplayMode\n" );
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE WaitForVerticalBlank( DWORD dwFlags, HANDLE hEvent ) override {
		DebugWrite( "MyDirectDraw::WaitForVerticalBlank\n" );
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE RestoreAllSurfaces() override {
		DebugWrite( "MyDirectDraw::RestoreAllSurfaces\n" );
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE StartModeTest( LPSIZE lpModesToTest, DWORD dwNumEntries, DWORD dwFlags ) override {
		DebugWrite( "MyDirectDraw::StartModeTest\n" );
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE TestCooperativeLevel() override {
		DebugWrite( "MyDirectDraw::TestCooperativeLevel\n" );
		return S_OK;
	}

private:
	IDirectDraw7* directDraw7;
	std::atomic<ULONG> RefCount;
	DDSURFACEDESC2 DisplayMode;
};
