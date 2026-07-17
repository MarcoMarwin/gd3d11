#pragma once
#include "d3d.h"
#include "MyDirect3DVertexBuffer7.h"
#include <stdio.h>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include "../Engine.h"
#include "../Logger.h"
#include "MyDirectDrawSurface7.h"
#include "../GothicAPI.h"
#include "../HookExceptionFilter.h"
#include "../ShaderIDs.h"

#define GOTHIC_FVF_XYZ_DIF_T1 (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1)
#define GOTHIC_FVF_XYZ_DIF_T1_SIZE ((3 + 1 + 2) * 4)

#define GOTHIC_FVF_XYZ_NRM_T1 (D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1)
#define GOTHIC_FVF_XYZ_NRM_T1_SIZE ((3 + 3 + 2) * 4)

#define GOTHIC_FVF_XYZ_NRM_DIF_T2 (D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX2)
#define GOTHIC_FVF_XYZ_NRM_DIF_T2_SIZE ((3 + 3 + 1 + 4) * 4)

#define GOTHIC_FVF_XYZ_DIF_T2 (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX2)
#define GOTHIC_FVF_XYZ_DIF_T2_SIZE ((3 + 1 + 4) * 4)

#define GOTHIC_FVF_XYZRHW_DIF_T1 (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)
#define GOTHIC_FVF_XYZRHW_DIF_T1_SIZE ((4 + 1 + 2) * 4)

#define GOTHIC_FVF_XYZRHW_DIF_SPEC_T1 (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX1)
#define GOTHIC_FVF_XYZRHW_DIF_SPEC_T1_SIZE ((4 + 1 + 1 + 2) * 4)

const int DRAW_PRIM_INDEX_BUFFER_SIZE = 4096 * sizeof( VERTEX_INDEX );

class MyDirect3DDevice7 final : public IDirect3DDevice7 {
public:
	MyDirect3DDevice7( IDirect3D7* direct3D7, IDirectDrawSurface7* renderTarget )
		: Direct3D7( direct3D7 ), RenderTarget( renderTarget ), RefCount( 1 ) {
		DebugWrite( "MyDirect3DDevice7::MyDirect3DDevice7" );
		if ( Direct3D7 ) Direct3D7->AddRef();
		if ( RenderTarget ) RenderTarget->AddRef();
		BoundTextures.fill( nullptr );

		ZeroMemory(&FakeDeviceDesc, sizeof(D3DDEVICEDESC7));
		ZeroMemory(&CurrentViewport, sizeof(CurrentViewport));
		CurrentViewport.dvMinZ = 0.0f;
		CurrentViewport.dvMaxZ = 1.0f;
		FakeDeviceDesc.dwDevCaps = (D3DDEVCAPS_FLOATTLVERTEX|D3DDEVCAPS_EXECUTESYSTEMMEMORY|D3DDEVCAPS_TLVERTEXSYSTEMMEMORY|D3DDEVCAPS_TEXTUREVIDEOMEMORY|D3DDEVCAPS_DRAWPRIMTLVERTEX
			|D3DDEVCAPS_CANRENDERAFTERFLIP|D3DDEVCAPS_DRAWPRIMITIVES2|D3DDEVCAPS_DRAWPRIMITIVES2EX|D3DDEVCAPS_HWTRANSFORMANDLIGHT|D3DDEVCAPS_HWRASTERIZATION);
		FakeDeviceDesc.dpcLineCaps.dwSize = sizeof(D3DPRIMCAPS);
		FakeDeviceDesc.dpcLineCaps.dwMiscCaps = D3DPMISCCAPS_MASKZ;
		FakeDeviceDesc.dpcLineCaps.dwRasterCaps = (D3DPRASTERCAPS_DITHER|D3DPRASTERCAPS_ZTEST|D3DPRASTERCAPS_SUBPIXEL|D3DPRASTERCAPS_FOGVERTEX|D3DPRASTERCAPS_FOGTABLE
			|D3DPRASTERCAPS_MIPMAPLODBIAS|D3DPRASTERCAPS_ZBIAS|D3DPRASTERCAPS_ANISOTROPY|D3DPRASTERCAPS_WFOG|D3DPRASTERCAPS_ZFOG);
		FakeDeviceDesc.dpcLineCaps.dwZCmpCaps = (D3DPCMPCAPS_NEVER|D3DPCMPCAPS_LESS|D3DPCMPCAPS_EQUAL|D3DPCMPCAPS_LESSEQUAL|D3DPCMPCAPS_GREATER|D3DPCMPCAPS_NOTEQUAL
			|D3DPCMPCAPS_GREATEREQUAL|D3DPCMPCAPS_ALWAYS);
		FakeDeviceDesc.dpcLineCaps.dwSrcBlendCaps = (D3DPBLENDCAPS_ZERO|D3DPBLENDCAPS_ONE|D3DPBLENDCAPS_SRCCOLOR|D3DPBLENDCAPS_INVSRCCOLOR|D3DPBLENDCAPS_SRCALPHA
			|D3DPBLENDCAPS_INVSRCALPHA|D3DPBLENDCAPS_DESTALPHA|D3DPBLENDCAPS_INVDESTALPHA|D3DPBLENDCAPS_DESTCOLOR|D3DPBLENDCAPS_INVDESTCOLOR|D3DPBLENDCAPS_SRCALPHASAT
			|D3DPBLENDCAPS_BOTHSRCALPHA|D3DPBLENDCAPS_BOTHINVSRCALPHA);
		FakeDeviceDesc.dpcLineCaps.dwDestBlendCaps = (D3DPBLENDCAPS_ZERO|D3DPBLENDCAPS_ONE|D3DPBLENDCAPS_SRCCOLOR|D3DPBLENDCAPS_INVSRCCOLOR|D3DPBLENDCAPS_SRCALPHA
			|D3DPBLENDCAPS_INVSRCALPHA|D3DPBLENDCAPS_DESTALPHA|D3DPBLENDCAPS_INVDESTALPHA|D3DPBLENDCAPS_DESTCOLOR|D3DPBLENDCAPS_INVDESTCOLOR|D3DPBLENDCAPS_SRCALPHASAT);
		FakeDeviceDesc.dpcLineCaps.dwAlphaCmpCaps = (D3DPCMPCAPS_NEVER|D3DPCMPCAPS_LESS|D3DPCMPCAPS_EQUAL|D3DPCMPCAPS_LESSEQUAL|D3DPCMPCAPS_GREATER|D3DPCMPCAPS_NOTEQUAL
			|D3DPCMPCAPS_GREATEREQUAL|D3DPCMPCAPS_ALWAYS);
		FakeDeviceDesc.dpcLineCaps.dwShadeCaps = (D3DPSHADECAPS_COLORFLATRGB|D3DPSHADECAPS_COLORGOURAUDRGB|D3DPSHADECAPS_SPECULARFLATRGB|D3DPSHADECAPS_SPECULARGOURAUDRGB
			|D3DPSHADECAPS_ALPHAFLATBLEND|D3DPSHADECAPS_ALPHAGOURAUDBLEND|D3DPSHADECAPS_FOGFLAT|D3DPSHADECAPS_FOGGOURAUD);
		FakeDeviceDesc.dpcLineCaps.dwTextureCaps = (D3DPTEXTURECAPS_PERSPECTIVE|D3DPTEXTURECAPS_ALPHA|D3DPTEXTURECAPS_TRANSPARENCY|D3DPTEXTURECAPS_BORDER
			|D3DPTEXTURECAPS_TEXREPEATNOTSCALEDBYSIZE|D3DPTEXTURECAPS_CUBEMAP|D3DPTEXTURECAPS_COLORKEYBLEND);
		FakeDeviceDesc.dpcLineCaps.dwTextureFilterCaps = (D3DPTFILTERCAPS_NEAREST|D3DPTFILTERCAPS_LINEAR|D3DPTFILTERCAPS_MIPNEAREST|D3DPTFILTERCAPS_MIPLINEAR
			|D3DPTFILTERCAPS_LINEARMIPNEAREST|D3DPTFILTERCAPS_LINEARMIPLINEAR|D3DPTFILTERCAPS_MINFPOINT|D3DPTFILTERCAPS_MINFLINEAR|D3DPTFILTERCAPS_MINFANISOTROPIC
			|D3DPTFILTERCAPS_MIPFPOINT|D3DPTFILTERCAPS_MIPFLINEAR|D3DPTFILTERCAPS_MAGFPOINT|D3DPTFILTERCAPS_MAGFLINEAR|D3DPTFILTERCAPS_MAGFANISOTROPIC);
		FakeDeviceDesc.dpcLineCaps.dwTextureBlendCaps = (D3DPTBLENDCAPS_DECAL|D3DPTBLENDCAPS_MODULATE|D3DPTBLENDCAPS_DECALALPHA|D3DPTBLENDCAPS_MODULATEALPHA|D3DPTBLENDCAPS_DECALMASK
			|D3DPTBLENDCAPS_MODULATEMASK|D3DPTBLENDCAPS_COPY|D3DPTBLENDCAPS_ADD);
		FakeDeviceDesc.dpcLineCaps.dwTextureAddressCaps = (D3DPTADDRESSCAPS_WRAP|D3DPTADDRESSCAPS_MIRROR|D3DPTADDRESSCAPS_CLAMP|D3DPTADDRESSCAPS_BORDER|D3DPTADDRESSCAPS_INDEPENDENTUV);
		FakeDeviceDesc.dpcTriCaps.dwSize = sizeof(D3DPRIMCAPS);
		FakeDeviceDesc.dpcTriCaps.dwMiscCaps = (D3DPMISCCAPS_MASKZ|D3DPMISCCAPS_CULLNONE|D3DPMISCCAPS_CULLCW|D3DPMISCCAPS_CULLCCW);
		FakeDeviceDesc.dpcTriCaps.dwRasterCaps = (D3DPRASTERCAPS_DITHER|D3DPRASTERCAPS_ZTEST|D3DPRASTERCAPS_SUBPIXEL|D3DPRASTERCAPS_FOGVERTEX|D3DPRASTERCAPS_FOGTABLE
			|D3DPRASTERCAPS_MIPMAPLODBIAS|D3DPRASTERCAPS_ZBIAS|D3DPRASTERCAPS_ANISOTROPY|D3DPRASTERCAPS_WFOG|D3DPRASTERCAPS_ZFOG);
		FakeDeviceDesc.dpcTriCaps.dwZCmpCaps = (D3DPCMPCAPS_NEVER|D3DPCMPCAPS_LESS|D3DPCMPCAPS_EQUAL|D3DPCMPCAPS_LESSEQUAL|D3DPCMPCAPS_GREATER|D3DPCMPCAPS_NOTEQUAL
			|D3DPCMPCAPS_GREATEREQUAL|D3DPCMPCAPS_ALWAYS);
		FakeDeviceDesc.dpcTriCaps.dwSrcBlendCaps = (D3DPBLENDCAPS_ZERO|D3DPBLENDCAPS_ONE|D3DPBLENDCAPS_SRCCOLOR|D3DPBLENDCAPS_INVSRCCOLOR|D3DPBLENDCAPS_SRCALPHA
			|D3DPBLENDCAPS_INVSRCALPHA|D3DPBLENDCAPS_DESTALPHA|D3DPBLENDCAPS_INVDESTALPHA|D3DPBLENDCAPS_DESTCOLOR|D3DPBLENDCAPS_INVDESTCOLOR|D3DPBLENDCAPS_SRCALPHASAT
			|D3DPBLENDCAPS_BOTHSRCALPHA|D3DPBLENDCAPS_BOTHINVSRCALPHA);
		FakeDeviceDesc.dpcTriCaps.dwDestBlendCaps = (D3DPBLENDCAPS_ZERO|D3DPBLENDCAPS_ONE|D3DPBLENDCAPS_SRCCOLOR|D3DPBLENDCAPS_INVSRCCOLOR|D3DPBLENDCAPS_SRCALPHA
			|D3DPBLENDCAPS_INVSRCALPHA|D3DPBLENDCAPS_DESTALPHA|D3DPBLENDCAPS_INVDESTALPHA|D3DPBLENDCAPS_DESTCOLOR|D3DPBLENDCAPS_INVDESTCOLOR|D3DPBLENDCAPS_SRCALPHASAT);
		FakeDeviceDesc.dpcTriCaps.dwAlphaCmpCaps = (D3DPCMPCAPS_NEVER|D3DPCMPCAPS_LESS|D3DPCMPCAPS_EQUAL|D3DPCMPCAPS_LESSEQUAL|D3DPCMPCAPS_GREATER|D3DPCMPCAPS_NOTEQUAL
			|D3DPCMPCAPS_GREATEREQUAL|D3DPCMPCAPS_ALWAYS);
		FakeDeviceDesc.dpcTriCaps.dwShadeCaps = (D3DPSHADECAPS_COLORFLATRGB|D3DPSHADECAPS_COLORGOURAUDRGB|D3DPSHADECAPS_SPECULARFLATRGB|D3DPSHADECAPS_SPECULARGOURAUDRGB
			|D3DPSHADECAPS_ALPHAFLATBLEND|D3DPSHADECAPS_ALPHAGOURAUDBLEND|D3DPSHADECAPS_FOGFLAT|D3DPSHADECAPS_FOGGOURAUD);
		FakeDeviceDesc.dpcTriCaps.dwTextureCaps = (D3DPTEXTURECAPS_PERSPECTIVE|D3DPTEXTURECAPS_ALPHA|D3DPTEXTURECAPS_TRANSPARENCY|D3DPTEXTURECAPS_BORDER
			|D3DPTEXTURECAPS_TEXREPEATNOTSCALEDBYSIZE|D3DPTEXTURECAPS_CUBEMAP|D3DPTEXTURECAPS_COLORKEYBLEND);
		FakeDeviceDesc.dpcTriCaps.dwTextureFilterCaps = (D3DPTFILTERCAPS_NEAREST|D3DPTFILTERCAPS_LINEAR|D3DPTFILTERCAPS_MIPNEAREST|D3DPTFILTERCAPS_MIPLINEAR
			|D3DPTFILTERCAPS_LINEARMIPNEAREST|D3DPTFILTERCAPS_LINEARMIPLINEAR|D3DPTFILTERCAPS_MINFPOINT|D3DPTFILTERCAPS_MINFLINEAR|D3DPTFILTERCAPS_MINFANISOTROPIC
			|D3DPTFILTERCAPS_MIPFPOINT|D3DPTFILTERCAPS_MIPFLINEAR|D3DPTFILTERCAPS_MAGFPOINT|D3DPTFILTERCAPS_MAGFLINEAR|D3DPTFILTERCAPS_MAGFANISOTROPIC);
		FakeDeviceDesc.dpcTriCaps.dwTextureBlendCaps = (D3DPTBLENDCAPS_DECAL|D3DPTBLENDCAPS_MODULATE|D3DPTBLENDCAPS_DECALALPHA|D3DPTBLENDCAPS_MODULATEALPHA|D3DPTBLENDCAPS_DECALMASK
			|D3DPTBLENDCAPS_MODULATEMASK|D3DPTBLENDCAPS_COPY|D3DPTBLENDCAPS_ADD);
		FakeDeviceDesc.dpcTriCaps.dwTextureAddressCaps = (D3DPTADDRESSCAPS_WRAP|D3DPTADDRESSCAPS_MIRROR|D3DPTADDRESSCAPS_CLAMP|D3DPTADDRESSCAPS_BORDER|D3DPTADDRESSCAPS_INDEPENDENTUV);
		FakeDeviceDesc.dwDeviceRenderBitDepth = 1280;
		FakeDeviceDesc.dwDeviceZBufferBitDepth = 1536;
		FakeDeviceDesc.dwMinTextureWidth = 1;
		FakeDeviceDesc.dwMinTextureHeight = 1;
		FakeDeviceDesc.dwMaxTextureWidth = 16384;
		FakeDeviceDesc.dwMaxTextureHeight = 16384;
		FakeDeviceDesc.dwMaxTextureRepeat = 32768;
		FakeDeviceDesc.dwMaxTextureAspectRatio = 32768;
		FakeDeviceDesc.dwMaxAnisotropy = 16;
		FakeDeviceDesc.dvGuardBandLeft = -16384.0f;
		FakeDeviceDesc.dvGuardBandTop = -16384.0f;
		FakeDeviceDesc.dvGuardBandRight = 16384.0f;
		FakeDeviceDesc.dvGuardBandBottom = 16384.0f;
		FakeDeviceDesc.dvExtentsAdjust = 0.0f;
		FakeDeviceDesc.dwStencilCaps = (D3DSTENCILCAPS_KEEP|D3DSTENCILCAPS_ZERO|D3DSTENCILCAPS_REPLACE|D3DSTENCILCAPS_INCRSAT|D3DSTENCILCAPS_DECRSAT|D3DSTENCILCAPS_INVERT
			|D3DSTENCILCAPS_INCR|D3DSTENCILCAPS_DECR);
		FakeDeviceDesc.dwFVFCaps = (D3DFVFCAPS_DONOTSTRIPELEMENTS|8);
		FakeDeviceDesc.dwTextureOpCaps = (D3DTEXOPCAPS_DISABLE|D3DTEXOPCAPS_SELECTARG1|D3DTEXOPCAPS_SELECTARG2|D3DTEXOPCAPS_MODULATE|D3DTEXOPCAPS_MODULATE2X|D3DTEXOPCAPS_MODULATE4X
			|D3DTEXOPCAPS_ADD|D3DTEXOPCAPS_ADDSIGNED|D3DTEXOPCAPS_ADDSIGNED2X|D3DTEXOPCAPS_SUBTRACT|D3DTEXOPCAPS_ADDSMOOTH|D3DTEXOPCAPS_BLENDDIFFUSEALPHA|D3DTEXOPCAPS_BLENDTEXTUREALPHA
			|D3DTEXOPCAPS_BLENDFACTORALPHA|D3DTEXOPCAPS_BLENDTEXTUREALPHAPM|D3DTEXOPCAPS_BLENDCURRENTALPHA|D3DTEXOPCAPS_PREMODULATE|D3DTEXOPCAPS_MODULATEALPHA_ADDCOLOR
			|D3DTEXOPCAPS_MODULATECOLOR_ADDALPHA|D3DTEXOPCAPS_MODULATEINVALPHA_ADDCOLOR|D3DTEXOPCAPS_MODULATEINVCOLOR_ADDALPHA|D3DTEXOPCAPS_BUMPENVMAP|D3DTEXOPCAPS_BUMPENVMAPLUMINANCE
			|D3DTEXOPCAPS_DOTPRODUCT3);
		FakeDeviceDesc.wMaxTextureBlendStages = 4;
		FakeDeviceDesc.wMaxSimultaneousTextures = 4;
		FakeDeviceDesc.dwMaxActiveLights = 8;
		FakeDeviceDesc.dvMaxVertexW = 10000000000.0f;
		FakeDeviceDesc.deviceGUID = {0xF5049E78, 0x4861, 0x11D2, {0xA4, 0x07, 0x00, 0xA0, 0xC9, 0x06, 0x29, 0xA8}};
		FakeDeviceDesc.wMaxUserClipPlanes = 6;
		FakeDeviceDesc.wMaxVertexBlendMatrices = 4;
		FakeDeviceDesc.dwVertexProcessingCaps = (D3DVTXPCAPS_TEXGEN|D3DVTXPCAPS_MATERIALSOURCE7|D3DVTXPCAPS_DIRECTIONALLIGHTS|D3DVTXPCAPS_POSITIONALLIGHTS|D3DVTXPCAPS_LOCALVIEWER);
	}

	~MyDirect3DDevice7() {
		for ( IDirectDrawSurface7* texture : BoundTextures ) {
			if ( texture ) texture->Release();
		}
		if ( RenderTarget ) RenderTarget->Release();
		if ( Direct3D7 ) Direct3D7->Release();
	}


	/*** IUnknown methods ***/
	HRESULT __declspec(nothrow) STDMETHODCALLTYPE QueryInterface( REFIID riid, void** ppvObj ) override {
		DebugWrite( "MyDirect3DDevice7::QueryInterface" );
		if ( !ppvObj ) return E_POINTER;
		*ppvObj = nullptr;
		if ( IsEqualIID( riid, IID_IUnknown )
			|| IsEqualIID( riid, IID_IDirect3DDevice7 ) ) {
			*ppvObj = static_cast<IDirect3DDevice7*>(this);
			AddRef();
			return S_OK;
		}
		return E_NOINTERFACE;
	}

	ULONG __declspec(nothrow) STDMETHODCALLTYPE AddRef() override {
		DebugWrite( "MyDirect3DDevice7::AddRef" );
		return RefCount.fetch_add( 1, std::memory_order_relaxed ) + 1;
	}

	ULONG __declspec(nothrow) STDMETHODCALLTYPE Release() override {
		DebugWrite( "MyDirect3DDevice7::Release" );
		const ULONG references =
			RefCount.fetch_sub( 1, std::memory_order_acq_rel ) - 1;
		if ( references == 0 ) {
			delete this;
		}
		return references;
	}

	/*** IDirect3DDevice7 methods ***/
	HRESULT __declspec(nothrow) STDMETHODCALLTYPE GetCaps( LPD3DDEVICEDESC7 lpD3DDevDesc ) override {
		DebugWrite( "MyDirect3DDevice7::GetCaps" );
		if ( !lpD3DDevDesc ) return E_POINTER;
		*lpD3DDevDesc = FakeDeviceDesc;
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE GetClipPlane( DWORD Index, float* pPlane ) override {
		DebugWrite( "MyDirect3DDevice7::GetClipPlane" );
		if ( !pPlane ) return E_POINTER;
		if ( Index >= 6 ) return E_INVALIDARG;
		std::fill_n( pPlane, 4, 0.0f );
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE SetClipPlane( DWORD dwIndex, D3DVALUE* pPlaneEquation ) override {
		DebugWrite( "MyDirect3DDevice7::SetClipPlane" );
		if ( !pPlaneEquation ) return E_POINTER;
		if ( dwIndex >= 6 ) return E_INVALIDARG;
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE GetClipStatus( LPD3DCLIPSTATUS lpD3DClipStatus ) override {
		DebugWrite( "MyDirect3DDevice7::GetClipStatus" );
		if ( !lpD3DClipStatus ) return E_POINTER;
		ZeroMemory( lpD3DClipStatus, sizeof( *lpD3DClipStatus ) );
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE SetClipStatus( LPD3DCLIPSTATUS lpD3DClipStatus ) override {
		DebugWrite( "MyDirect3DDevice7::SetClipStatus" );
		return lpD3DClipStatus ? S_OK : E_POINTER;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE GetDirect3D( IDirect3D7** ppD3D ) override {
		DebugWrite( "MyDirect3DDevice7::GetDirect3D" );
		if ( !ppD3D ) return E_POINTER;
		*ppD3D = Direct3D7;
		if ( !Direct3D7 ) return E_UNEXPECTED;
		Direct3D7->AddRef();
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE GetInfo( DWORD dwDevInfoID, LPVOID pDevInfoStruct, DWORD dwSize ) override {
		DebugWrite( "MyDirect3DDevice7::GetInfo" );
		(void)dwDevInfoID;
		if ( dwSize != 0 && !pDevInfoStruct ) return E_POINTER;
		return E_NOTIMPL;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE GetLight( DWORD dwLightIndex, LPD3DLIGHT7 lpLight ) override {
		DebugWrite( "MyDirect3DDevice7::GetLight" );
		(void)dwLightIndex;
		if ( !lpLight ) return E_POINTER;
		ZeroMemory( lpLight, sizeof( *lpLight ) );
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE GetLightEnable( DWORD Index, BOOL* pEnable ) override {
		DebugWrite( "MyDirect3DDevice7::GetLightEnable" );
		(void)Index;
		if ( !pEnable ) return E_POINTER;
		*pEnable = FALSE;
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE GetMaterial( LPD3DMATERIAL7 lpMaterial ) override {
		DebugWrite( "MyDirect3DDevice7::GetMaterial" );
		if ( !lpMaterial ) return E_POINTER;
		ZeroMemory( lpMaterial, sizeof( *lpMaterial ) );
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE SetMaterial( LPD3DMATERIAL7 lpMaterial ) override {
		DebugWrite( "MyDirect3DDevice7::SetMaterial" );
		return lpMaterial ? S_OK : E_POINTER;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE GetRenderState( D3DRENDERSTATETYPE State, DWORD* pValue ) override {
		DebugWrite( "MyDirect3DDevice7::GetRenderState" );
		if ( !pValue ) return E_POINTER;
		*pValue = 0;
		if ( !Engine::GAPI ) return E_UNEXPECTED;

		const GothicRendererState& state = Engine::GAPI->GetRendererState();
		switch ( State ) {
		case D3DRENDERSTATE_FOGENABLE:
			*pValue = state.GraphicsState.FF_FogWeight != 0.0f;
			break;
		case D3DRENDERSTATE_FOGSTART:
			std::memcpy( pValue, &state.GraphicsState.FF_FogNear, sizeof( *pValue ) );
			break;
		case D3DRENDERSTATE_FOGEND:
			std::memcpy( pValue, &state.GraphicsState.FF_FogFar, sizeof( *pValue ) );
			break;
		case D3DRENDERSTATE_FOGCOLOR:
			*pValue = float4( state.GraphicsState.FF_FogColor ).ToDWORD();
			break;
		case D3DRENDERSTATE_AMBIENT:
			*pValue = float4( state.GraphicsState.FF_AmbientLighting ).ToDWORD();
			break;
		case D3DRENDERSTATE_ZENABLE:
			*pValue = state.DepthState.DepthBufferEnabled;
			break;
		case D3DRENDERSTATE_ZWRITEENABLE:
			*pValue = state.DepthState.DepthWriteEnabled;
			break;
		case D3DRENDERSTATE_ALPHATESTENABLE:
			*pValue = (state.GraphicsState.FF_GSwitches & GSWITCH_ALPHAREF) != 0;
			break;
		case D3DRENDERSTATE_SRCBLEND:
			*pValue = static_cast<DWORD>(state.BlendState.SrcBlend);
			break;
		case D3DRENDERSTATE_DESTBLEND:
			*pValue = static_cast<DWORD>(state.BlendState.DestBlend);
			break;
		case D3DRENDERSTATE_ZFUNC:
			*pValue = static_cast<DWORD>(state.DepthState.DepthBufferCompareFunc);
			break;
		case D3DRENDERSTATE_ALPHAREF:
			*pValue = static_cast<DWORD>(std::clamp(
				state.GraphicsState.FF_AlphaRef, 0.0f, 1.0f ) * 255.0f + 0.5f);
			break;
		case D3DRENDERSTATE_ALPHABLENDENABLE:
			*pValue = state.BlendState.BlendEnabled;
			break;
		case D3DRENDERSTATE_ZBIAS:
			*pValue = static_cast<DWORD>(state.RasterizerState.ZBias);
			break;
		case D3DRENDERSTATE_CULLMODE:
			*pValue = static_cast<DWORD>(state.RasterizerState.CullMode);
			break;
		case D3DRENDERSTATE_TEXTUREFACTOR:
			*pValue = state.GraphicsState.FF_TextureFactor.ToDWORD();
			break;
		case D3DRENDERSTATE_LIGHTING:
			*pValue = (state.GraphicsState.FF_GSwitches & GSWITCH_LIGHING) != 0;
			break;
		default:
			break;
		}
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE SetRenderState( D3DRENDERSTATETYPE State, DWORD Value ) override {
		DebugWrite( "MyDirect3DDevice7::SetRenderState" );
		if ( !Engine::GAPI ) return E_UNEXPECTED;

		GothicRendererState& state = Engine::GAPI->GetRendererState();

		// Extract the needed renderstates
		switch ( State ) {
		case D3DRENDERSTATETYPE::D3DRENDERSTATE_FOGENABLE:
			state.GraphicsState.FF_FogWeight = Value != 0 ? 1.0f : 0.0f;
			break;

		case D3DRENDERSTATETYPE::D3DRENDERSTATE_FOGSTART:
			std::memcpy( &state.GraphicsState.FF_FogNear, &Value, sizeof( Value ) );
			break;

		case D3DRENDERSTATETYPE::D3DRENDERSTATE_FOGEND:
			std::memcpy( &state.GraphicsState.FF_FogFar, &Value, sizeof( Value ) );
			break;

		case D3DRENDERSTATETYPE::D3DRENDERSTATE_FOGCOLOR:
		{
			BYTE r = (Value >> 16) & 0xFF;
			BYTE g = (Value >> 8) & 0xFF;
			BYTE b = Value & 0xFF;
			state.GraphicsState.FF_FogColor = float3( r / 255.0f, g / 255.0f, b / 255.0f );
		}
		break;

		case D3DRENDERSTATETYPE::D3DRENDERSTATE_AMBIENT:
		{
			BYTE r = (Value >> 16) & 0xFF;
			BYTE g = (Value >> 8) & 0xFF;
			BYTE b = Value & 0xFF;
			state.GraphicsState.FF_AmbientLighting = float3( r / 255.0f, g / 255.0f, b / 255.0f );

			// Does this enable the ambientlighting?
			//data->lightEnabled = 1.0f;
		}
		break;

        case D3DRENDERSTATE_ZENABLE: {
            if ( state.RendererInfo.RenderStage == STAGE_DRAW_SKY ) {
                // we do custom Sky rendering behavior
                break;
            }
            state.DepthState.DepthBufferEnabled = Value != 0;
            state.DepthState.SetDirty();
            break;
        }
		case D3DRENDERSTATE_ZWRITEENABLE:
			state.DepthState.DepthWriteEnabled = Value != 0;
			state.DepthState.SetDirty();
			break;
		case D3DRENDERSTATE_ALPHATESTENABLE: state.GraphicsState.SetGraphicsSwitch( GSWITCH_ALPHAREF, Value != 0 );	break;
		case D3DRENDERSTATE_SRCBLEND: state.BlendState.SrcBlend = static_cast<GothicBlendStateInfo::EBlendFunc>(Value); state.BlendState.SetDirty(); break;
		case D3DRENDERSTATE_DESTBLEND: state.BlendState.DestBlend = static_cast<GothicBlendStateInfo::EBlendFunc>(Value); state.BlendState.SetDirty(); break;
		//case D3DRENDERSTATE_CULLMODE: state.RasterizerState.CullMode = static_cast<GothicRasterizerStateInfo::ECullMode>(Value); state.RasterizerState.SetDirty(); break;
		case D3DRENDERSTATE_ZFUNC: state.DepthState.DepthBufferCompareFunc = static_cast<GothicDepthBufferStateInfo::ECompareFunc>(Value); state.DepthState.SetDirty(); break;
		case D3DRENDERSTATE_ALPHAREF: state.GraphicsState.FF_AlphaRef = static_cast<float>(Value) / 255.0f; break; // Ref for masked
		case D3DRENDERSTATE_ALPHABLENDENABLE: state.BlendState.BlendEnabled = Value != 0; state.BlendState.SetDirty(); break;
		case D3DRENDERSTATE_ZBIAS: state.RasterizerState.ZBias = static_cast<int>(Value); state.RasterizerState.SetDirty(); break;
		case D3DRENDERSTATE_TEXTUREFACTOR: state.GraphicsState.FF_TextureFactor = float4( Value ); break;
		case D3DRENDERSTATE_LIGHTING: state.GraphicsState.SetGraphicsSwitch( GSWITCH_LIGHING, Value != 0 ); break;
		}

		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE GetRenderTarget( LPDIRECTDRAWSURFACE7* lplpRenderTarget ) override {
		DebugWrite( "MyDirect3DDevice7::GetRenderTarget" );
		if ( !lplpRenderTarget ) return E_POINTER;
		*lplpRenderTarget = RenderTarget;
		if ( !RenderTarget ) return DDERR_NOTFOUND;
		RenderTarget->AddRef();
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE SetRenderTarget( LPDIRECTDRAWSURFACE7 lpNewRenderTarget, DWORD dwFlags ) override {
		DebugWrite( "MyDirect3DDevice7::SetRenderTarget" );
		(void)dwFlags;
		if ( !lpNewRenderTarget ) return E_POINTER;
		lpNewRenderTarget->AddRef();
		if ( RenderTarget ) RenderTarget->Release();
		RenderTarget = lpNewRenderTarget;
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE GetTexture( DWORD dwStage, LPDIRECTDRAWSURFACE7* lplpTexture ) override {
		DebugWrite( "MyDirect3DDevice7::GetTexture" );
		if ( !lplpTexture ) return E_POINTER;
		*lplpTexture = nullptr;
		if ( dwStage >= BoundTextures.size() ) return E_INVALIDARG;
		*lplpTexture = BoundTextures[dwStage];
		if ( *lplpTexture ) (*lplpTexture)->AddRef();
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE SetTexture( DWORD dwStage, LPDIRECTDRAWSURFACE7 lplpTexture ) override {
		DebugWrite( "MyDirect3DDevice7::SetTexture" );
		if ( dwStage >= 8 ) return E_INVALIDARG;
		if ( !Engine::GraphicsEngine ) return E_UNEXPECTED;

		if ( lplpTexture ) {
			static_cast<MyDirectDrawSurface7*>(lplpTexture)->BindToSlot(
				static_cast<int>(dwStage) );
			lplpTexture->AddRef();
		} else {
			Engine::GraphicsEngine->UnbindTexture( dwStage );
		}
		if ( BoundTextures[dwStage] ) BoundTextures[dwStage]->Release();
		BoundTextures[dwStage] = lplpTexture;
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE GetTextureStageState( DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD* pValue ) override {
		DebugWrite( "MyDirect3DDevice7::GetTextureStageState" );
		if ( !pValue ) return E_POINTER;
		*pValue = 0;
		if ( Stage >= 8 ) return E_INVALIDARG;
		if ( !Engine::GAPI ) return E_UNEXPECTED;

		const GothicRendererState& state = Engine::GAPI->GetRendererState();
		if ( Stage < 2 ) {
			const FixedFunctionStage& stage = state.GraphicsState.FF_Stages[Stage];
			switch ( Type ) {
			case D3DTSS_COLOROP: *pValue = static_cast<DWORD>(stage.ColorOp); break;
			case D3DTSS_COLORARG1: *pValue = static_cast<DWORD>(stage.ColorArg1); break;
			case D3DTSS_COLORARG2: *pValue = static_cast<DWORD>(stage.ColorArg2); break;
			case D3DTSS_ALPHAOP: *pValue = static_cast<DWORD>(stage.AlphaOp); break;
			case D3DTSS_ALPHAARG1: *pValue = static_cast<DWORD>(stage.AlphaArg1); break;
			case D3DTSS_ALPHAARG2: *pValue = static_cast<DWORD>(stage.AlphaArg2); break;
			case D3DTSS_ADDRESSU: *pValue = static_cast<DWORD>(state.SamplerState.AddressU); break;
			case D3DTSS_ADDRESSV: *pValue = static_cast<DWORD>(state.SamplerState.AddressV); break;
			case D3DTSS_ADDRESS:
				*pValue = state.SamplerState.AddressU == state.SamplerState.AddressV
					? static_cast<DWORD>(state.SamplerState.AddressU) : 0;
				break;
			default: break;
			}
		}
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE SetTextureStageState( DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value ) override {
		DebugWrite( "MyDirect3DDevice7::SetTextureStageState" );
		if ( Stage >= 8 ) return E_INVALIDARG;
		if ( !Engine::GAPI ) return E_UNEXPECTED;

		GothicRendererState& state = Engine::GAPI->GetRendererState();
		switch ( Type ) {
		case D3DTSS_COLOROP:
			if ( Stage < 2 )
				state.GraphicsState.FF_Stages[Stage].ColorOp = static_cast<FixedFunctionStage::EColorOp>(Value);
			else
				LogWarn() << "Gothic uses more than 2 TextureStages!";
			break;

		case D3DTSS_COLORARG1:
			if ( Stage < 2 )
				state.GraphicsState.FF_Stages[Stage].ColorArg1 = static_cast<FixedFunctionStage::ETextureArg>(Value);
			break;

		case D3DTSS_COLORARG2:
			if ( Stage < 2 )
				state.GraphicsState.FF_Stages[Stage].ColorArg2 = static_cast<FixedFunctionStage::ETextureArg>(Value);
			break;

		case D3DTSS_ALPHAOP:
			if ( Stage < 2 )
				state.GraphicsState.FF_Stages[Stage].AlphaOp = static_cast<FixedFunctionStage::EColorOp>(Value);
			break;

		case D3DTSS_ALPHAARG1:
			if ( Stage < 2 )
				state.GraphicsState.FF_Stages[Stage].AlphaArg1 = static_cast<FixedFunctionStage::ETextureArg>(Value);
			break;

		case D3DTSS_ALPHAARG2:
			if ( Stage < 2 )
				state.GraphicsState.FF_Stages[Stage].AlphaArg2 = static_cast<FixedFunctionStage::ETextureArg>(Value);
			break;

		case D3DTSS_BUMPENVMAT00: break;
		case D3DTSS_BUMPENVMAT01: break;
		case D3DTSS_BUMPENVMAT10: break;
		case D3DTSS_BUMPENVMAT11: break;
		case D3DTSS_TEXCOORDINDEX:
			if ( Value > 7 ) // This means that some other flag was set, and the only case that happens is for reflections
			{
				state.GraphicsState.SetGraphicsSwitch( GSWITCH_REFLECTIONS, true );
			} else {
				state.GraphicsState.SetGraphicsSwitch( GSWITCH_REFLECTIONS, false );
			}
			break;

		case D3DTSS_ADDRESS: state.SamplerState.AddressU = static_cast<GothicSamplerStateInfo::ETextureAddress>(Value);
			state.SamplerState.AddressV = static_cast<GothicSamplerStateInfo::ETextureAddress>(Value);
			state.SamplerState.SetDirty();
			break;

		case D3DTSS_ADDRESSU: state.SamplerState.AddressU = static_cast<GothicSamplerStateInfo::ETextureAddress>(Value);
			state.SamplerState.SetDirty();
			break;

		case D3DTSS_ADDRESSV: state.SamplerState.AddressV = static_cast<GothicSamplerStateInfo::ETextureAddress>(Value);
			state.SamplerState.SetDirty();
			break;

		case D3DTSS_BORDERCOLOR: break;
		case D3DTSS_MAGFILTER: break;
		case D3DTSS_MINFILTER: break;
		case D3DTSS_MIPFILTER: break;
		case D3DTSS_MIPMAPLODBIAS: break;
		case D3DTSS_MAXMIPLEVEL: break;
		case D3DTSS_MAXANISOTROPY: break;
		case D3DTSS_BUMPENVLSCALE: break;
		case D3DTSS_BUMPENVLOFFSET: break;
		case D3DTSS_TEXTURETRANSFORMFLAGS:
			break;
		}
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE GetTransform( D3DTRANSFORMSTATETYPE State, D3DMATRIX* pMatrix ) override {
		DebugWrite( "MyDirect3DDevice7::GetTransform" );
		if ( !pMatrix ) return E_POINTER;
		if ( !Engine::GAPI ) return E_UNEXPECTED;

		const GothicTransformInfo& transforms =
			Engine::GAPI->GetRendererState().TransformState;
		const XMFLOAT4X4* source = nullptr;
		switch ( State ) {
		case D3DTRANSFORMSTATE_WORLD:
			source = &transforms.TransformWorld;
			break;
		case D3DTRANSFORMSTATE_VIEW:
			source = &transforms.TransformView;
			break;
		case D3DTRANSFORMSTATE_PROJECTION:
			source = &transforms.TransformProjUnjittered;
			break;
		default:
			return E_INVALIDARG;
		}
		XMStoreFloat4x4(
			reinterpret_cast<XMFLOAT4X4*>(pMatrix),
			XMMatrixTranspose( XMLoadFloat4x4( source ) ) );
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE SetTransform( D3DTRANSFORMSTATETYPE dtstTransformStateType, LPD3DMATRIX lpD3DMatrix ) override {
		DebugWrite( "MyDirect3DDevice7::SetTransform" );
		if ( !lpD3DMatrix ) return E_POINTER;
		if ( !Engine::GAPI ) return E_UNEXPECTED;

		GothicRendererState& state = Engine::GAPI->GetRendererState();
		switch ( dtstTransformStateType ) {
		case D3DTRANSFORMSTATE_WORLD: {
			XMMATRIX matrixWorld = XMLoadFloat4x4( reinterpret_cast<XMFLOAT4X4*>(lpD3DMatrix) );
			XMStoreFloat4x4( &state.TransformState.TransformWorld, XMMatrixTranspose( matrixWorld ) );
			break;
		}

		case D3DTRANSFORMSTATE_VIEW: {
			XMMATRIX matrixView = XMLoadFloat4x4( reinterpret_cast<XMFLOAT4X4*>(lpD3DMatrix) );
			XMStoreFloat4x4( &state.TransformState.TransformView, XMMatrixTranspose( matrixView ) );
			break;
		}

		case D3DTRANSFORMSTATE_PROJECTION: {
            if ( state.RendererInfo.RenderStage == STAGE_DRAW_WORLD ) {
                // stop the game from constantly resetting the projection matrix
                // as temporal jitter may have modified it. Only allow this once at the start of a frame.
                return S_OK;
            }

            XMMATRIX matrixProj = XMLoadFloat4x4( reinterpret_cast<XMFLOAT4X4*>(lpD3DMatrix) );
			XMStoreFloat4x4( &state.TransformState.TransformProj, XMMatrixTranspose( matrixProj ) );
            state.TransformState.TransformProjUnjittered = state.TransformState.TransformProj;
			break;
		}
		}

		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE GetViewport( LPD3DVIEWPORT7 lpViewport ) override {
		DebugWrite( "MyDirect3DDevice7::GetViewport" );
		if ( !lpViewport ) return E_POINTER;
		*lpViewport = CurrentViewport;
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE SetViewport( LPD3DVIEWPORT7 lpViewport ) override {
		DebugWrite( "MyDirect3DDevice7::SetViewport" );
		if ( !lpViewport ) return E_POINTER;
		if ( !Engine::GAPI || !Engine::GraphicsEngine ) return E_UNEXPECTED;

		const float scale = std::max(
			0.1f,
			Engine::GAPI->GetRendererState().RendererSettings.GothicUIScale );
		if ( !std::isfinite( scale )
			|| !std::isfinite( lpViewport->dvMinZ )
			|| !std::isfinite( lpViewport->dvMaxZ )
			|| lpViewport->dvMinZ > lpViewport->dvMaxZ ) {
			return E_INVALIDARG;
		}

		const double scaledX = static_cast<double>(lpViewport->dwX) * scale;
		const double scaledY = static_cast<double>(lpViewport->dwY) * scale;
		const double scaledWidth = static_cast<double>(lpViewport->dwWidth) * scale;
		const double scaledHeight = static_cast<double>(lpViewport->dwHeight) * scale;
		const double maxViewportValue =
			static_cast<double>((std::numeric_limits<UINT>::max)());
		if ( scaledX > maxViewportValue || scaledY > maxViewportValue
			|| scaledWidth > maxViewportValue || scaledHeight > maxViewportValue ) {
			return E_INVALIDARG;
		}

		ViewportInfo vp;
		vp.TopLeftX = static_cast<UINT>(scaledX);
		vp.TopLeftY = static_cast<UINT>(scaledY);
		vp.Height = static_cast<UINT>(scaledHeight);
		vp.Width = static_cast<UINT>(scaledWidth);
		vp.MinZ = lpViewport->dvMinZ;
		vp.MaxZ = lpViewport->dvMaxZ;

		CurrentViewport = *lpViewport;
		Engine::GraphicsEngine->SetViewport( vp );

		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE ApplyStateBlock( DWORD dwBlockHandle ) override {
		DebugWrite( "MyDirect3DDevice7::ApplyStateBlock" );
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE BeginScene() override {
		DebugWrite( "MyDirect3DDevice7::BeginScene" );
		if ( !Engine::GraphicsEngine ) return E_UNEXPECTED;
		Engine::GraphicsEngine->OnBeginFrame();
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE BeginStateBlock() override {
		DebugWrite( "MyDirect3DDevice7::BeginStateBlock" );
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE CaptureStateBlock( DWORD dwBlockHandle ) override {
		DebugWrite( "MyDirect3DDevice7::CaptureStateBlock" );
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE Clear( DWORD dwCount, LPD3DRECT lpRects, DWORD dwFlags, D3DCOLOR dwColor, D3DVALUE dvZ, DWORD dwStencil ) override {
		DebugWrite( "MyDirect3DDevice7::Clear" );
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE ComputeSphereVisibility( LPD3DVECTOR lpCenters, LPD3DVALUE lpRadii, DWORD dwNumSpheres, DWORD dwFlags, LPDWORD lpdwReturnValues ) override {
		DebugWrite( "MyDirect3DDevice7::ComputeSphereVisibility" );
		(void)dwFlags;
		if ( dwNumSpheres == 0 ) return S_OK;
		if ( !lpCenters || !lpRadii || !lpdwReturnValues ) return E_POINTER;
		if ( dwNumSpheres > 10000000u ) return E_INVALIDARG;
		std::fill_n( lpdwReturnValues, dwNumSpheres, 0u );
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE CreateStateBlock( D3DSTATEBLOCKTYPE d3dsbType, LPDWORD lpdwBlockHandle ) override {
		DebugWrite( "MyDirect3DDevice7::CreateStateBlock" );
		(void)d3dsbType;
		if ( !lpdwBlockHandle ) return E_POINTER;
		*lpdwBlockHandle = 1;
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE DeleteStateBlock( DWORD dwBlockHandle ) override {
		DebugWrite( "MyDirect3DDevice7::DeleteStateBlock" );
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE DrawIndexedPrimitive( D3DPRIMITIVETYPE dptPrimitiveType, DWORD dwVertexTypeDesc, LPVOID lpvVertices, DWORD dwVertexCount, LPWORD lpwIndices, DWORD dwIndexCount, DWORD dwFlags ) override {
		DebugWrite( "MyDirect3DDevice7::DrawIndexedPrimitive" );
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE DrawIndexedPrimitiveStrided( D3DPRIMITIVETYPE dptPrimitiveType, DWORD dwVertexTypeDesc, LPD3DDRAWPRIMITIVESTRIDEDDATA lpVertexArray, DWORD dwVertexCount, LPWORD lpwIndices, DWORD dwIndexCount, DWORD dwFlags ) override {
		DebugWrite( "MyDirect3DDevice7::DrawIndexedPrimitiveStrided" );
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE DrawIndexedPrimitiveVB( D3DPRIMITIVETYPE d3dptPrimitiveType, LPDIRECT3DVERTEXBUFFER7 lpd3dVertexBuffer, DWORD dwStartVertex, DWORD dwNumVertices, LPWORD lpwIndices, DWORD dwIndexCount, DWORD dwFlags ) override {
		DebugWrite( "MyDirect3DDevice7::DrawIndexedPrimitiveVB" );
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE DrawPrimitive( D3DPRIMITIVETYPE dptPrimitiveType, DWORD dwVertexTypeDesc, LPVOID lpvVertices, DWORD dwVertexCount, DWORD dwFlags ) override {
		DebugWrite( "MyDirect3DDevice7::DrawPrimitive" );
		(void)dwFlags;
		if ( dwVertexCount == 0 ) return S_OK;
		if ( !lpvVertices ) return E_POINTER;
		if ( !Engine::GAPI || !Engine::GraphicsEngine ) return E_UNEXPECTED;
		if ( dwVertexCount > 10000000u ) return E_INVALIDARG;

		// Convert them into ExVertices
		static std::vector<ExVertexStruct> exv;
		try {
			exv.resize( dwVertexCount );
		} catch ( const std::bad_alloc& ) {
			return E_OUTOFMEMORY;
		}

		switch ( dwVertexTypeDesc ) {
        case GOTHIC_FVF_XYZRHW_DIF_T1: {
			//return S_OK; 
			for ( unsigned int i = 0; i < dwVertexCount; i++ ) {
				Gothic_XYZRHW_DIF_T1_Vertex* rhw = reinterpret_cast<Gothic_XYZRHW_DIF_T1_Vertex*>(lpvVertices);

				exv[i].Position = rhw[i].xyz;
				exv[i].Normal.x = rhw[i].rhw;
				exv[i].TexCoord = rhw[i].texCoord;
				exv[i].Color = rhw[i].color;
			}

			// Gothic wants that for the sky
            auto& state = Engine::GAPI->GetRendererState();
            state.RasterizerState.FrontCounterClockwise = true;
            state.RasterizerState.SetDirty();

            const auto vs = state.RendererInfo.RenderStage == STAGE_DRAW_SKY
                ? VShaderID::VS_TransformedEx_MAX_Z
                : VShaderID::VS_TransformedEx;

			Engine::GraphicsEngine->SetActiveVertexShader( vs );
			Engine::GraphicsEngine->BindViewportInformation( vs, 0 );
			break;
        }

        case GOTHIC_FVF_XYZRHW_DIF_SPEC_T1: {
			for ( unsigned int i = 0; i < dwVertexCount; i++ ) {
				Gothic_XYZRHW_DIF_SPEC_T1_Vertex* rhw = reinterpret_cast<Gothic_XYZRHW_DIF_SPEC_T1_Vertex*>(lpvVertices);

				exv[i].Position = rhw[i].xyz;
				exv[i].Normal.x = rhw[i].rhw;
				exv[i].TexCoord = rhw[i].texCoord;
				exv[i].Color = rhw[i].color;
			}
            const auto vs = Engine::GAPI->GetRendererState().RendererInfo.RenderStage == STAGE_DRAW_SKY
                ? VShaderID::VS_TransformedEx_MAX_Z
                : VShaderID::VS_TransformedEx;

			Engine::GraphicsEngine->SetActiveVertexShader( vs );
			Engine::GraphicsEngine->BindViewportInformation( vs, 0 );
			break;
        }

		default:
			return S_OK;
		}

		Engine::GraphicsEngine->SetActivePixelShader( PShaderID::PS_FixedFunctionPipe );
		if ( dptPrimitiveType == D3DPT_TRIANGLEFAN ) {
			static std::vector<ExVertexStruct> vertexList;
			vertexList.clear();
			try {
				WorldConverter::TriangleFanToList(
					exv.data(), dwVertexCount, &vertexList );
			} catch ( const std::bad_alloc& ) {
				exv.clear();
				return E_OUTOFMEMORY;
			}
			if ( !vertexList.empty() ) {
				Engine::GraphicsEngine->DrawVertexArray(
					vertexList.data(), vertexList.size() );
			}
		} else {
			if ( dptPrimitiveType == D3DPT_TRIANGLELIST )
				Engine::GraphicsEngine->DrawVertexArray( exv.data(), dwVertexCount );
		}

		exv.clear(); // static, keep the memory allocated

		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE DrawPrimitiveStrided( D3DPRIMITIVETYPE dptPrimitiveType, DWORD dwVertexTypeDesc, LPD3DDRAWPRIMITIVESTRIDEDDATA lpVertexArray, DWORD dwVertexCount, DWORD dwFlags ) override {
		DebugWrite( "MyDirect3DDevice7::DrawPrimitiveStrided" );
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE DrawPrimitiveVB( D3DPRIMITIVETYPE d3dptPrimitiveType, LPDIRECT3DVERTEXBUFFER7 lpd3dVertexBuffer, DWORD dwStartVertex, DWORD dwNumVertices, DWORD dwFlags ) override {
		DebugWrite( "MyDirect3DDevice7::DrawPrimitiveVB" );
		(void)dwFlags;
		if ( !lpd3dVertexBuffer ) return E_POINTER;
		if ( !Engine::GAPI || !Engine::GraphicsEngine ) return E_UNEXPECTED;
		if ( dwNumVertices == 0 ) return S_OK;
		if ( d3dptPrimitiveType < 4 )
		{
			return S_OK;
		}

		D3DVERTEXBUFFERDESC desc{};
		const HRESULT descResult =
			lpd3dVertexBuffer->GetVertexBufferDesc( &desc );
		if ( FAILED( descResult ) ) return descResult;
		if ( dwStartVertex > desc.dwNumVertices
			|| dwNumVertices > desc.dwNumVertices - dwStartVertex ) {
			return E_INVALIDARG;
		}

		switch ( desc.dwFVF ) {
        case GOTHIC_FVF_XYZRHW_DIF_T1: {
            const auto& state = Engine::GAPI->GetRendererState();
            auto vshader =
                state.RendererInfo.RenderStage == STAGE_DRAW_SKY
                ? VShaderID::VS_XYZRHW_DIF_T1_MAX_Z
                : VShaderID::VS_XYZRHW_DIF_T1;

			Engine::GraphicsEngine->SetActiveVertexShader( vshader );
            Engine::GraphicsEngine->SetActivePixelShader( PShaderID::PS_FixedFunctionPipe );

            Engine::GraphicsEngine->BindViewportInformation( vshader, 0 );

			// Gothic wants that for the sky
			Engine::GAPI->GetRendererState().RasterizerState.FrontCounterClockwise = true;
			Engine::GAPI->GetRendererState().RasterizerState.SetDirty();
			Engine::GraphicsEngine->DrawVertexBufferFF( static_cast<MyDirect3DVertexBuffer7*>(lpd3dVertexBuffer)->GetVertexBuffer(), dwNumVertices, dwStartVertex, sizeof( Gothic_XYZRHW_DIF_T1_Vertex ) );
			break;
        }

		default:
			return S_OK;
		}

		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE EndScene() override {
		DebugWrite( "MyDirect3DDevice7::EndScene" );
		if ( !Engine::GraphicsEngine ) return E_UNEXPECTED;

		hook_infunc

			Engine::GraphicsEngine->OnEndFrame();

		hook_outfunc

		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE EndStateBlock( LPDWORD lpdwBlockHandle ) override {
		DebugWrite( "MyDirect3DDevice7::EndStateBlock" );
		if ( !lpdwBlockHandle ) return E_POINTER;
		*lpdwBlockHandle = 1;
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE EnumTextureFormats( LPD3DENUMPIXELFORMATSCALLBACK lpd3dEnumPixelProc, LPVOID lpArg ) override {
		DebugWrite( "MyDirect3DDevice7::EnumTextureFormats" );

        static std::array<DDPIXELFORMAT, 19> tformats = { {
            {32, DDPF_ALPHA, 0, 8, 0x00, 0x00, 0x00, 0x00},
            {32, DDPF_LUMINANCE, 0, 8, 0xFF, 0x00, 0x00, 0x00},
            {32, DDPF_LUMINANCE | DDPF_ALPHAPIXELS, 0, 8, 0x0F, 0x00, 0x00, 0xF0},
            {32, DDPF_RGB, 0, 16, 0xF800, 0x7E0, 0x1F, 0x00},
            {32, DDPF_RGB | DDPF_ALPHAPIXELS, 0, 16, 0x7C00, 0x3E0, 0x1F, 0x8000},
            {32, DDPF_RGB | DDPF_ALPHAPIXELS, 0, 16, 0xF00, 0xF0, 0x0F, 0xF000},
            {32, DDPF_LUMINANCE | DDPF_ALPHAPIXELS, 0, 16, 0xFF, 0x00, 0x00, 0xFF00},
            {32, DDPF_BUMPDUDV, 0, 16, 0xFF, 0xFF00, 0x00, 0x00},
            {32, DDPF_BUMPDUDV | DDPF_BUMPLUMINANCE, 0, 16, 0x1F, 0x3E0, 0xFC00, 0x00},
            {32, DDPF_RGB, 0, 32, 0xFF0000, 0xFF00, 0xFF, 0x00},
            {32, DDPF_RGB | DDPF_ALPHAPIXELS, 0, 32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000},
            {32, DDPF_FOURCC, MAKEFOURCC( 'Y','U','Y','2' ), 0, 0x00, 0x00, 0x00, 0x00},
            {32, DDPF_FOURCC, MAKEFOURCC( 'U','Y','V','Y' ), 0, 0x00, 0x00, 0x00, 0x00},
            {32, DDPF_FOURCC, MAKEFOURCC( 'A','Y','U','V' ), 0, 0x00, 0x00, 0x00, 0x00},
            {32, DDPF_FOURCC, FOURCC_DXT1, 0, 0x00, 0x00, 0x00, 0x00},
            {32, DDPF_FOURCC, FOURCC_DXT2, 0, 0x00, 0x00, 0x00, 0x00},
            {32, DDPF_FOURCC, FOURCC_DXT3, 0, 0x00, 0x00, 0x00, 0x00},
            {32, DDPF_FOURCC, FOURCC_DXT4, 0, 0x00, 0x00, 0x00, 0x00},
            {32, DDPF_FOURCC, FOURCC_DXT5, 0, 0x00, 0x00, 0x00, 0x00},
        } };

        if ( !lpd3dEnumPixelProc ) return E_POINTER;
        for ( DDPIXELFORMAT& ppf : tformats ) {
            if ( (*lpd3dEnumPixelProc)(&ppf, lpArg) == D3DENUMRET_CANCEL )
                break;
        }

		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE Load( LPDIRECTDRAWSURFACE7 lpDestTex, LPPOINT lpDestPoint, LPDIRECTDRAWSURFACE7 lpSrcTex, LPRECT lprcSrcRect, DWORD dwFlags ) override {
		DebugWrite( "MyDirect3DDevice7::Load" );
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE MultiplyTransform( D3DTRANSFORMSTATETYPE dtstTransformStateType, LPD3DMATRIX lpD3DMatrix ) override {
		DebugWrite( "MyDirect3DDevice7::MultiplyTransform" );
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE PreLoad( LPDIRECTDRAWSURFACE7 lpddsTexture ) override {
		DebugWrite( "MyDirect3DDevice7::PreLoad" );
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE ValidateDevice( DWORD* pNumPasses ) override {
		DebugWrite( "MyDirect3DDevice7::ValidateDevice" );
		if ( !pNumPasses ) return E_POINTER;
		*pNumPasses = 1;
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE LightEnable( DWORD Index, BOOL Enable ) override {
		DebugWrite( "MyDirect3DDevice7::LightEnable" );
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE SetLight( DWORD dwLightIndex, LPD3DLIGHT7 lpLight ) override {
		DebugWrite( "MyDirect3DDevice7::SetLight" );
		(void)dwLightIndex;
		return lpLight ? S_OK : E_POINTER;
	}

private:
	D3DDEVICEDESC7 FakeDeviceDesc;
	IDirect3D7* Direct3D7;
	IDirectDrawSurface7* RenderTarget;
	std::array<IDirectDrawSurface7*, 8> BoundTextures;
	D3DVIEWPORT7 CurrentViewport;
	std::atomic<ULONG> RefCount;
};
