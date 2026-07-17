#include "FakeDirectDrawSurface7.h"
#include "MyDirectDrawSurface7.h"
#include "../D3D11Texture.h"
#include "../Engine.h"
#include "../GothicAPI.h"
#include <algorithm>
#include <limits>
#include <new>

FakeDirectDrawSurface7::FakeDirectDrawSurface7()
    : RefCount( 1 ),
    MipLevel( 0 ),
    Data( nullptr ),
    IsLocked( false ),
    OriginalDesc{},
    Resource( nullptr ),
    Priority( 0 ),
    Lod( 0 ),
    Uniqueness( 0 ) {
}


FakeDirectDrawSurface7::~FakeDirectDrawSurface7() {
    // Release mip-map chain first
    for (size_t i = 0; i < AttachedSurfaces.size(); ++i ) {
        if ( AttachedSurfaces[i] ) AttachedSurfaces[i]->Release();
    }
    AttachedSurfaces.clear();

    delete[] Data;
}

void FakeDirectDrawSurface7::InitFakeSurface( const DDSURFACEDESC2* desc, MyDirectDrawSurface7* resource, int mipLevel ) {
    if ( !desc || !resource || mipLevel < 0 || mipLevel >= 32 ) {
        Resource = nullptr;
        MipLevel = 0;
        ZeroMemory( &OriginalDesc, sizeof( OriginalDesc ) );
        return;
    }

    OriginalDesc = *desc;
    Resource = resource;
    MipLevel = mipLevel;
    OriginalDesc.dwWidth = (std::max<DWORD>)(
        static_cast<DWORD>(1), OriginalDesc.dwWidth >> static_cast<unsigned int>(MipLevel) );
    OriginalDesc.dwHeight = (std::max<DWORD>)(
        static_cast<DWORD>(1), OriginalDesc.dwHeight >> static_cast<unsigned int>(MipLevel) );
}

HRESULT FakeDirectDrawSurface7::QueryInterface( REFIID riid, LPVOID* ppvObj ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::QueryInterface(%s)" );
    if ( !ppvObj ) return E_POINTER;
    *ppvObj = nullptr;
    if ( IsEqualIID( riid, IID_IUnknown )
        || IsEqualIID( riid, IID_IDirectDrawSurface7 ) ) {
        *ppvObj = static_cast<IDirectDrawSurface7*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

ULONG FakeDirectDrawSurface7::AddRef() {
    DebugWrite( "FakeDirectDrawSurface7(%p)::AddRef(%i)" );
    const ULONG previous =
        RefCount.fetch_add( 1, std::memory_order_acq_rel );
    if ( previous == 1 && Resource ) Resource->AddRef();
    return previous + 1;
}

ULONG FakeDirectDrawSurface7::Release() {
    DebugWrite( "FakeDirectDrawSurface7(%p)::Release(%i)" );
    const ULONG previous =
        RefCount.fetch_sub( 1, std::memory_order_acq_rel );
    const ULONG references = previous - 1;
    if ( previous == 2 && Resource ) {
        MyDirectDrawSurface7* root = Resource;
        root->Release();
    } else if ( references == 0 ) {
        delete this;
    }
    return references;
}

HRESULT FakeDirectDrawSurface7::AddAttachedSurface( LPDIRECTDRAWSURFACE7 lpDDSAttachedSurface ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::AddAttachedSurface()" );
    if ( !lpDDSAttachedSurface || lpDDSAttachedSurface == this )
        return DDERR_INVALIDPARAMS;

    lpDDSAttachedSurface->AddRef();
    try {
        AttachedSurfaces.push_back( lpDDSAttachedSurface );
    } catch ( const std::bad_alloc& ) {
        lpDDSAttachedSurface->Release();
        return E_OUTOFMEMORY;
    }
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::AddOverlayDirtyRect( LPRECT lpRect ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::AddOverlayDirtyRect()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::Blt( LPRECT lpDestRect, LPDIRECTDRAWSURFACE7 lpDDSrcSurface, LPRECT lpSrcRect, DWORD dwFlags, LPDDBLTFX lpDDBltFx ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::Blt()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::BltBatch( LPDDBLTBATCH lpDDBltBatch, DWORD dwCount, DWORD dwFlags ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::BltBatch()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::BltFast( DWORD dwX, DWORD dwY, LPDIRECTDRAWSURFACE7 lpDDSrcSurface, LPRECT lpSrcRect, DWORD dwTrans ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::BltFast()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::DeleteAttachedSurface( DWORD dwFlags, LPDIRECTDRAWSURFACE7 lpDDSAttachedSurface ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::DeleteAttachedSurface()" );
    (void)dwFlags;
    if ( !lpDDSAttachedSurface ) return DDERR_INVALIDPARAMS;

    const auto it = std::find(
        AttachedSurfaces.begin(), AttachedSurfaces.end(), lpDDSAttachedSurface );
    if ( it == AttachedSurfaces.end() ) return DDERR_NOTFOUND;
    (*it)->Release();
    AttachedSurfaces.erase( it );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::EnumAttachedSurfaces( LPVOID lpContext, LPDDENUMSURFACESCALLBACK7 lpEnumSurfacesCallback ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::EnumAttachedSurfaces()" );
    if ( !lpEnumSurfacesCallback ) return DDERR_INVALIDPARAMS;

    for ( IDirectDrawSurface7* surface : AttachedSurfaces ) {
        DDSURFACEDESC2 desc{};
        desc.dwSize = sizeof( desc );
        const HRESULT result = surface->GetSurfaceDesc( &desc );
        if ( FAILED( result ) ) return result;
        if ( lpEnumSurfacesCallback( surface, &desc, lpContext )
            == DDENUMRET_CANCEL ) {
            break;
        }
    }
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::EnumOverlayZOrders( DWORD dwFlags, LPVOID lpContext, LPDDENUMSURFACESCALLBACK7 lpfnCallback ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::EnumOverlayZOrders()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::Flip( LPDIRECTDRAWSURFACE7 lpDDSurfaceTargetOverride, DWORD dwFlags ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::Flip() #####" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::GetAttachedSurface( LPDDSCAPS2 lpDDSCaps2, LPDIRECTDRAWSURFACE7* lplpDDAttachedSurface ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::GetAttachedSurface()" );
    if ( !lplpDDAttachedSurface ) return E_POINTER;
    *lplpDDAttachedSurface = nullptr;
    if ( !lpDDSCaps2 ) return DDERR_INVALIDPARAMS;
    if ( AttachedSurfaces.empty() ) return DDERR_NOTFOUND;

    *lplpDDAttachedSurface = AttachedSurfaces.front();
    AttachedSurfaces.front()->AddRef();
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::GetBltStatus( DWORD dwFlags ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::GetBltStatus()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::GetCaps( LPDDSCAPS2 lpDDSCaps2 ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::GetCaps()" );
    if ( !lpDDSCaps2 ) return E_POINTER;
    *lpDDSCaps2 = OriginalDesc.ddsCaps;
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::GetClipper( LPDIRECTDRAWCLIPPER* lplpDDClipper ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::GetClipper()" );
    if ( !lplpDDClipper ) return E_POINTER;
    *lplpDDClipper = nullptr;
    return DDERR_NOCLIPPERATTACHED;
}

HRESULT FakeDirectDrawSurface7::GetColorKey( DWORD dwFlags, LPDDCOLORKEY lpDDColorKey ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::GetColorKey()" );
    (void)dwFlags;
    if ( !lpDDColorKey ) return E_POINTER;
    ZeroMemory( lpDDColorKey, sizeof( *lpDDColorKey ) );
    return DDERR_NOCOLORKEY;
}

HRESULT FakeDirectDrawSurface7::GetDC( HDC* lphDC ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::GetDC()" );
    if ( !lphDC ) return E_POINTER;
    *lphDC = nullptr;
    return DDERR_NODC;
}

HRESULT FakeDirectDrawSurface7::GetFlipStatus( DWORD dwFlags ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::GetFlipStatus()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::GetOverlayPosition( LPLONG lplX, LPLONG lplY ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::GetOverlayPosition()" );
    if ( !lplX || !lplY ) return E_POINTER;
    *lplX = 0;
    *lplY = 0;
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::GetPalette( LPDIRECTDRAWPALETTE* lplpDDPalette ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::GetPalette()" );
    if ( !lplpDDPalette ) return E_POINTER;
    *lplpDDPalette = nullptr;
    return DDERR_NOPALETTEATTACHED;
}

HRESULT FakeDirectDrawSurface7::GetPixelFormat( LPDDPIXELFORMAT lpDDPixelFormat ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::GetPixelFormat()" );
    if ( !lpDDPixelFormat ) return E_POINTER;
    *lpDDPixelFormat = OriginalDesc.ddpfPixelFormat;
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::GetSurfaceDesc( LPDDSURFACEDESC2 lpDDSurfaceDesc ) {
    if ( !lpDDSurfaceDesc ) return E_POINTER;
    *lpDDSurfaceDesc = OriginalDesc;
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::Initialize( LPDIRECTDRAW lpDD, LPDDSURFACEDESC2 lpDDSurfaceDesc ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::Initialize()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::IsLost() {
    DebugWrite( "FakeDirectDrawSurface7(%p)::IsLost()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::Lock( LPRECT lpDestRect, LPDDSURFACEDESC2 lpDDSurfaceDesc, DWORD dwFlags, HANDLE hEvent ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::Lock(%s, %s)" );
    (void)lpDestRect;
    (void)dwFlags;
    (void)hEvent;
    if ( !lpDDSurfaceDesc ) return DDERR_INVALIDPARAMS;

    bool expected = false;
    if ( !IsLocked.compare_exchange_strong(
        expected, true, std::memory_order_acq_rel ) ) {
        return DDERR_SURFACEBUSY;
    }

    *lpDDSurfaceDesc = OriginalDesc;
    lpDDSurfaceDesc->lpSurface = nullptr;
    lpDDSurfaceDesc->lPitch = 0;

    D3D11Texture* texture = Resource ? Resource->GetEngineTexture() : nullptr;
    if ( !texture ) {
        IsLocked.store( false, std::memory_order_release );
        return DDERR_CANTLOCKSURFACE;
    }

    const UINT dataSize = texture->GetSizeInBytes( MipLevel );
    const UINT rowPitch = texture->GetRowPitchBytes( MipLevel );
    if ( dataSize == 0 || rowPitch == 0
        || rowPitch > static_cast<UINT>((std::numeric_limits<LONG>::max)()) ) {
        IsLocked.store( false, std::memory_order_release );
        return DDERR_CANTLOCKSURFACE;
    }

    delete[] Data;
    Data = new (std::nothrow) unsigned char[dataSize];
    if ( !Data ) {
        IsLocked.store( false, std::memory_order_release );
        return DDERR_OUTOFMEMORY;
    }

    lpDDSurfaceDesc->lpSurface = Data;
    lpDDSurfaceDesc->lPitch = static_cast<LONG>(rowPitch);
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::Unlock( LPRECT lpRect ) {
    DebugWrite( "FakeDirectDrawSurface7::Unlock" );
    (void)lpRect;
    if ( !IsLocked.exchange( false, std::memory_order_acq_rel ) )
        return DDERR_NOTLOCKED;

    D3D11Texture* texture = Resource ? Resource->GetEngineTexture() : nullptr;
    XRESULT updateResult = XR_FAILED;
    if ( Engine::GAPI && texture && Data ) {
        updateResult = Engine::GAPI->GetMainThreadID() != GetCurrentThreadId()
            ? texture->UpdateDataDeferred( Data, MipLevel )
            : texture->UpdateData( Data, MipLevel );
    }

    delete[] Data;
    Data = nullptr;
    return updateResult == XR_SUCCESS ? S_OK : DDERR_GENERIC;
}

HRESULT FakeDirectDrawSurface7::ReleaseDC( HDC hDC ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::ReleaseDC()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::Restore() {
    DebugWrite( "FakeDirectDrawSurface7(%p)::Restore()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::SetClipper( LPDIRECTDRAWCLIPPER lpDDClipper ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::SetClipper()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::SetColorKey( DWORD dwFlags, LPDDCOLORKEY lpDDColorKey ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::SetColorKey(%s, %s)" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::SetOverlayPosition( LONG lX, LONG lY ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::SetOverlayPosition()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::SetPalette( LPDIRECTDRAWPALETTE lpDDPalette ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::SetPalette()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::UpdateOverlay( LPRECT lpSrcRect, LPDIRECTDRAWSURFACE7 lpDDDestSurface, LPRECT lpDestRect, DWORD dwFlags, LPDDOVERLAYFX lpDDOverlayFx ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::UpdateOverlay()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::UpdateOverlayDisplay( DWORD dwFlags ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::UpdateOverlayDisplay()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::UpdateOverlayZOrder( DWORD dwFlags, LPDIRECTDRAWSURFACE7 lpDDSReference ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::UpdateOverlayZOrder()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::GetDDInterface( LPVOID* lplpDD ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::GetDDInterface()" );
    if ( !lplpDD ) return E_POINTER;
    *lplpDD = nullptr;
    return DDERR_UNSUPPORTED;
}

HRESULT FakeDirectDrawSurface7::PageLock( DWORD dwFlags ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::PageLock()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::PageUnlock( DWORD dwFlags ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::PageUnlock()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::SetSurfaceDesc( LPDDSURFACEDESC2 lpDDSurfaceDesc, DWORD dwFlags ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::PageUnlock()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::SetPrivateData( REFGUID guidTag, LPVOID lpData, DWORD cbSize, DWORD dwFlags ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::SetPrivateData()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::GetPrivateData( REFGUID guidTag, LPVOID lpBuffer, LPDWORD lpcbBufferSize ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::GetPrivateData()" );
    (void)guidTag;
    (void)lpBuffer;
    if ( !lpcbBufferSize ) return E_POINTER;
    *lpcbBufferSize = 0;
    return DDERR_NOTFOUND;
}

HRESULT FakeDirectDrawSurface7::FreePrivateData( REFGUID guidTag ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::FreePrivateData()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::GetUniquenessValue( LPDWORD lpValue ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::GetUniquenessValue()" );
    if ( !lpValue ) return E_POINTER;
    *lpValue = Uniqueness.load( std::memory_order_relaxed );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::ChangeUniquenessValue() {
    DebugWrite( "FakeDirectDrawSurface7(%p)::ChangeUniquenessValue()" );
    Uniqueness.fetch_add( 1, std::memory_order_relaxed );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::SetPriority( DWORD dwPriority ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::SetPriority()" );
    Priority.store( dwPriority, std::memory_order_relaxed );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::GetPriority( LPDWORD dwPriority ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::GetPriority()" );
    if ( !dwPriority ) return E_POINTER;
    *dwPriority = Priority.load( std::memory_order_relaxed );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::SetLOD( DWORD dwLOD ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::SetLOD()" );
    Lod.store( dwLOD, std::memory_order_relaxed );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::GetLOD( LPDWORD dwLOD ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::GetLOD()" );
    if ( !dwLOD ) return E_POINTER;
    *dwLOD = Lod.load( std::memory_order_relaxed );
    return S_OK;
}
