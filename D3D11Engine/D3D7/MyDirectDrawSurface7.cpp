#include "MyDirectDrawSurface7.h"
#include "../Engine.h"
#include "../GothicAPI.h"
#include "../D3D11GraphicsEngineBase.h"
#include "../D3D11Texture.h"
#include "../zCTexture.h"
#include "../D3D11_Helpers.h"
#include "../zFILE_VDFS.h"
#include <algorithm>
#include <limits>
#include <new>

#define DebugWriteTex(x)  DebugWrite(x)

const std::string LEAF_SUBSTR[] = { "Treetop", "Bush", "Leaf" };

MyDirectDrawSurface7::MyDirectDrawSurface7() {
    refCount = 1;
    EngineTexture = nullptr;
    Normalmap = nullptr;
    FxMap = nullptr;
    Displacementmap = nullptr;
    LockedData = nullptr;
    LockedDataSize = 0;
    GothicTexture = nullptr;
    IsLocked = false;
    IsReady = false;
    AdditionalResourcesLoaded = false;
    TextureType = ETextureType::TX_UNDEF;
    LockType = 0;
    Priority = 0;
    Lod = 0;
    Uniqueness = 0;
    Clipper = nullptr;
    ZeroMemory( &OriginalSurfaceDesc, sizeof( OriginalSurfaceDesc ) );

    // Check for test-bind mode to figure out what zCTexture-Object we are associated with
    std::string bound;
    if ( Engine::GAPI && Engine::GAPI->IsInTextureTestBindMode( bound ) ) {
        Engine::GAPI->SetTextureTestBindMode( false, "" );
        return;
    }
}

MyDirectDrawSurface7::~MyDirectDrawSurface7() {
    if ( Engine::GAPI ) Engine::GAPI->RemoveSurface( this );

    // Release mip-map chain first
    for ( LPDIRECTDRAWSURFACE7 mipmap : attachedSurfaces ) {
        mipmap->Release();
    }

    if ( Clipper ) Clipper->Release();

    // Sometimes gothic doesn't unlock a surface or this is a movie-buffer
    delete[] LockedData;

    delete EngineTexture;
    delete Normalmap;
    delete FxMap;
    delete Displacementmap;
}

/** Returns the engine texture of this surface */
D3D11Texture* MyDirectDrawSurface7::GetEngineTexture() {
    return EngineTexture;
}

/** Returns the engine texture of this surface */
D3D11Texture* MyDirectDrawSurface7::GetNormalmap() {
    return Normalmap;
}

/** Returns the fx-map for this surface */
D3D11Texture* MyDirectDrawSurface7::GetFxMap() {
    return FxMap;
}

/** Returns the displacement map used for parallax occlusion mapping */
D3D11Texture* MyDirectDrawSurface7::GetDisplacementmap() {
    return Displacementmap;
}

/** Binds this texture */
void MyDirectDrawSurface7::BindToSlot( int slot ) {
    if ( !Engine::GraphicsEngine || slot < 0
        || slot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT ) {
        return;
    }
    if ( !IsReady.load( std::memory_order_acquire ) || !EngineTexture ) {
        Engine::GraphicsEngine->UnbindTexture( slot );
        return; // Don't bind half-loaded textures!
    }

    EngineTexture->BindToPixelShader( slot );

    if ( Normalmap && slot + 1 < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT ) {
        Normalmap->BindToPixelShader( slot + 1 );
        Normalmap->BindToVertexShader( 0 );
    } else if ( slot + 1 < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT ) {
        Engine::GraphicsEngine->UnbindTexture( slot + 1 );
    }
}

/** Loads additional resources if possible */
void MyDirectDrawSurface7::LoadAdditionalResources( zCTexture* ownedTexture ) {
    if ( !ownedTexture || !Engine::GAPI || !Engine::GraphicsEngine ) return;

    if ( !GothicTexture ) {
        GothicTexture = ownedTexture;
        TextureName = GothicTexture->GetNameWithoutExtView();

        // Find texture type
        if ( Toolbox::StringContainsOneOf( TextureName, LEAF_SUBSTR, std::size( LEAF_SUBSTR ) ) ) {
            TextureType = ETextureType::TX_LEAF;
        }

        Engine::GAPI->AddSurface( TextureName, this );

        // Set texture name
        if ( EngineTexture ) {
            SetDebugName( EngineTexture->GetTextureObject().Get(), "D3D11Texture(\"" + TextureName + "\")->Texture" );
            SetDebugName( EngineTexture->GetShaderResourceView().Get(), "D3D11Texture(\"" + TextureName + "\")->ShaderResourceView" );
        }
    }

    if ( TextureName.empty()
        || !Engine::GAPI->GetRendererState().RendererSettings.AllowNormalmaps ) {
        return;
    }

    bool expected = false;
    if ( !AdditionalResourcesLoaded.compare_exchange_strong(
        expected, true, std::memory_order_acq_rel ) ) {
        return;
    }

    auto createTexture = []() -> std::unique_ptr<D3D11Texture> {
        D3D11Texture* texture = nullptr;
        if ( !Engine::GraphicsEngine
            || Engine::GraphicsEngine->CreateTexture( &texture ) != XR_SUCCESS
            || !texture ) {
            return {};
        }
        return std::unique_ptr<D3D11Texture>( texture );
    };

    constexpr long MaxPackedTextureBytes = 512L * 1024L * 1024L;
    auto loadPackedTexture = [&]( const std::string& suffix )
        -> std::unique_ptr<D3D11Texture> {
        const std::string debugName = TextureName + suffix + ".DDS";
        auto file = zFILE_VDFS::Create(
            (R"(\_WORK\DATA\TEXTURES\REPLACEMENTS\)" + debugName).c_str() );
        if ( !file || !file->Exists() || file->Open( false ) != zERROR_NONE )
            return {};

        const long size = file->Size();
        if ( size <= 0 || size > MaxPackedTextureBytes ) {
            file->Close();
            return {};
        }

        std::vector<uint8_t> storage( static_cast<size_t>(size) );
        const long bytesRead = file->Read( storage.data(), size );
        file->Close();
        if ( bytesRead != size ) return {};

        auto texture = createTexture();
        if ( !texture
            || texture->Init( storage.data(), storage.size(), debugName )
                != XR_SUCCESS ) {
            return {};
        }
        return texture;
    };

    auto loadDiskTexture = [&]( const std::string& path )
        -> std::unique_ptr<D3D11Texture> {
        if ( !Toolbox::FileExists( path ) ) return {};
        auto texture = createTexture();
        if ( !texture || texture->Init( path ) != XR_SUCCESS ) return {};
        return texture;
    };

    std::unique_ptr<D3D11Texture> normalMap =
        loadPackedTexture( "_NORMAL" );
    std::unique_ptr<D3D11Texture> fxMap =
        loadPackedTexture( "_FX" );

    thread_local std::string replacementsFolder;
    thread_local std::string fileName;
    constexpr int MaxReplacementDirectories = 1024;

    for ( int index = 0; !normalMap && index < MaxReplacementDirectories; ++index ) {
        replacementsFolder = "system/GD3D11/textures/replacements/Normalmaps_"
            + std::to_string( index );
        if ( !Toolbox::FolderExists( replacementsFolder ) ) break;
        fileName = replacementsFolder + "/" + TextureName + "_normal.dds";
        normalMap = loadDiskTexture( fileName );
    }
    if ( !normalMap ) {
        fileName = "system/GD3D11/textures/replacements/Normalmaps_"
            + Engine::GAPI->GetGameName() + "/" + TextureName + "_normal.dds";
        normalMap = loadDiskTexture( fileName );
    }

    for ( int index = 0; !fxMap && index < MaxReplacementDirectories; ++index ) {
        replacementsFolder = "system/GD3D11/textures/replacements/Normalmaps_"
            + std::to_string( index );
        if ( !Toolbox::FolderExists( replacementsFolder ) ) break;
        fileName = replacementsFolder + "/" + TextureName + "_fx.dds";
        fxMap = loadDiskTexture( fileName );
    }
    if ( !fxMap ) {
        fileName = "system/GD3D11/textures/replacements/Normalmaps_"
            + Engine::GAPI->GetGameName() + "/" + TextureName + "_fx.dds";
        fxMap = loadDiskTexture( fileName );
    }

    std::unique_ptr<D3D11Texture> displacementMap;
    if ( normalMap ) {
        for ( int index = 0;
            !displacementMap && index < MaxReplacementDirectories; ++index ) {
            replacementsFolder =
                "system/GD3D11/textures/replacements/Displacementmaps_"
                + std::to_string( index );
            if ( !Toolbox::FolderExists( replacementsFolder ) ) break;
            fileName = replacementsFolder + "/" + TextureName + "_disp.dds";
            displacementMap = loadDiskTexture( fileName );
        }
        if ( !displacementMap ) {
            fileName =
                "system/GD3D11/textures/replacements/Displacementmaps_"
                + Engine::GAPI->GetGameName() + "/" + TextureName + "_disp.dds";
            displacementMap = loadDiskTexture( fileName );
        }
    }

    Normalmap = normalMap.release();
    FxMap = fxMap.release();
    Displacementmap = displacementMap.release();
}

HRESULT MyDirectDrawSurface7::QueryInterface( REFIID riid, LPVOID* ppvObj ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::QueryInterface(%s)" );
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

ULONG MyDirectDrawSurface7::AddRef() {
    DebugWriteTex( "IDirectDrawSurface7(%p)::AddRef(%i)" );
    return refCount.fetch_add( 1, std::memory_order_relaxed ) + 1;
}

ULONG MyDirectDrawSurface7::Release() {
    DebugWriteTex( "IDirectDrawSurface7(%p)::Release(%i)" );
    const ULONG references =
        refCount.fetch_sub( 1, std::memory_order_acq_rel ) - 1;
    if ( references == 0 ) {
        delete this;
    }
    return references;
}

HRESULT MyDirectDrawSurface7::AddAttachedSurface( LPDIRECTDRAWSURFACE7 lpDDSAttachedSurface ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::AddAttachedSurface()" );
    if ( !lpDDSAttachedSurface || lpDDSAttachedSurface == this )
        return DDERR_INVALIDPARAMS;

    lpDDSAttachedSurface->AddRef();
    try {
        attachedSurfaces.push_back( lpDDSAttachedSurface );
    } catch ( const std::bad_alloc& ) {
        lpDDSAttachedSurface->Release();
        return E_OUTOFMEMORY;
    }
    return S_OK;
}

HRESULT MyDirectDrawSurface7::AddOverlayDirtyRect( LPRECT lpRect ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::AddOverlayDirtyRect()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::Blt( LPRECT lpDestRect, LPDIRECTDRAWSURFACE7 lpDDSrcSurface, LPRECT lpSrcRect, DWORD dwFlags, LPDDBLTFX lpDDBltFx ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::Blt()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::BltBatch( LPDDBLTBATCH lpDDBltBatch, DWORD dwCount, DWORD dwFlags ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::BltBatch()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::BltFast( DWORD dwX, DWORD dwY, LPDIRECTDRAWSURFACE7 lpDDSrcSurface, LPRECT lpSrcRect, DWORD dwTrans ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::BltFast()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::DeleteAttachedSurface( DWORD dwFlags, LPDIRECTDRAWSURFACE7 lpDDSAttachedSurface ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::DeleteAttachedSurface()" );
    (void)dwFlags;
    if ( !lpDDSAttachedSurface ) return DDERR_INVALIDPARAMS;

    const auto it = std::find(
        attachedSurfaces.begin(), attachedSurfaces.end(), lpDDSAttachedSurface );
    if ( it == attachedSurfaces.end() ) return DDERR_NOTFOUND;
    (*it)->Release();
    attachedSurfaces.erase( it );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::EnumAttachedSurfaces( LPVOID lpContext, LPDDENUMSURFACESCALLBACK7 lpEnumSurfacesCallback ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::EnumAttachedSurfaces()" );
    if ( !lpEnumSurfacesCallback ) return DDERR_INVALIDPARAMS;

    for ( IDirectDrawSurface7* surface : attachedSurfaces ) {
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

HRESULT MyDirectDrawSurface7::EnumOverlayZOrders( DWORD dwFlags, LPVOID lpContext, LPDDENUMSURFACESCALLBACK7 lpfnCallback ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::EnumOverlayZOrders()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::Flip( LPDIRECTDRAWSURFACE7 lpDDSurfaceTargetOverride, DWORD dwFlags ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::Flip() #####" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::GetAttachedSurface( LPDDSCAPS2 lpDDSCaps2, LPDIRECTDRAWSURFACE7* lplpDDAttachedSurface ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::GetAttachedSurface()" );
    if ( !lplpDDAttachedSurface ) return E_POINTER;
    *lplpDDAttachedSurface = nullptr;
    if ( !lpDDSCaps2 ) return DDERR_INVALIDPARAMS;
    if ( attachedSurfaces.empty() ) return DDERR_NOTFOUND;

    *lplpDDAttachedSurface = attachedSurfaces.front();
    attachedSurfaces.front()->AddRef();
    return S_OK;
}

HRESULT MyDirectDrawSurface7::GetBltStatus( DWORD dwFlags ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::GetBltStatus()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::GetCaps( LPDDSCAPS2 lpDDSCaps2 ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::GetCaps()" );
    if ( !lpDDSCaps2 ) return E_POINTER;
    *lpDDSCaps2 = OriginalSurfaceDesc.ddsCaps;
    return S_OK;
}

HRESULT MyDirectDrawSurface7::GetClipper( LPDIRECTDRAWCLIPPER* lplpDDClipper ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::GetClipper()" );
    if ( !lplpDDClipper ) return E_POINTER;
    *lplpDDClipper = Clipper;
    if ( !Clipper ) return DDERR_NOCLIPPERATTACHED;
    Clipper->AddRef();
    return S_OK;
}

HRESULT MyDirectDrawSurface7::GetColorKey( DWORD dwFlags, LPDDCOLORKEY lpDDColorKey ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::GetColorKey()" );
    (void)dwFlags;
    if ( !lpDDColorKey ) return E_POINTER;
    ZeroMemory( lpDDColorKey, sizeof( *lpDDColorKey ) );
    return DDERR_NOCOLORKEY;
}

HRESULT MyDirectDrawSurface7::GetDC( HDC* lphDC ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::GetDC()" );
    if ( !lphDC ) return E_POINTER;
    *lphDC = nullptr;
    return DDERR_NODC;
}

HRESULT MyDirectDrawSurface7::GetFlipStatus( DWORD dwFlags ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::GetFlipStatus()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::GetOverlayPosition( LPLONG lplX, LPLONG lplY ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::GetOverlayPosition()" );
    if ( !lplX || !lplY ) return E_POINTER;
    *lplX = 0;
    *lplY = 0;
    return S_OK;
}

HRESULT MyDirectDrawSurface7::GetPalette( LPDIRECTDRAWPALETTE* lplpDDPalette ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::GetPalette()" );
    if ( !lplpDDPalette ) return E_POINTER;
    *lplpDDPalette = nullptr;
    return DDERR_NOPALETTEATTACHED;
}

HRESULT MyDirectDrawSurface7::GetPixelFormat( LPDDPIXELFORMAT lpDDPixelFormat ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::GetPixelFormat()" );
    if ( !lpDDPixelFormat ) return E_POINTER;
    *lpDDPixelFormat = OriginalSurfaceDesc.ddpfPixelFormat;
    return S_OK;
}

HRESULT MyDirectDrawSurface7::GetSurfaceDesc( LPDDSURFACEDESC2 lpDDSurfaceDesc ) {
    if ( !lpDDSurfaceDesc ) return E_POINTER;
    *lpDDSurfaceDesc = OriginalSurfaceDesc;
    return S_OK;
}

HRESULT MyDirectDrawSurface7::Initialize( LPDIRECTDRAW lpDD, LPDDSURFACEDESC2 lpDDSurfaceDesc ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::Initialize()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::IsLost() {
    DebugWriteTex( "IDirectDrawSurface7(%p)::IsLost()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::Lock( LPRECT lpDestRect, LPDDSURFACEDESC2 lpDDSurfaceDesc, DWORD dwFlags, HANDLE hEvent ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::Lock()" );
    (void)lpDestRect;
    (void)hEvent;
    if ( !lpDDSurfaceDesc ) return DDERR_INVALIDPARAMS;

    bool expected = false;
    if ( !IsLocked.compare_exchange_strong(
        expected, true, std::memory_order_acq_rel ) ) {
        return DDERR_SURFACEBUSY;
    }

    LockType = dwFlags;
    *lpDDSurfaceDesc = OriginalSurfaceDesc;
    lpDDSurfaceDesc->lpSurface = nullptr;
    lpDDSurfaceDesc->lPitch = 0;

    // Gothic combines DDLOCK_READONLY with other flags for framebuffer readback.
    if ( (LockType & DDLOCK_READONLY) != 0 && LockType != DDLOCK_READONLY ) {
        extern bool CreatingThumbnail;
        if ( !Engine::GraphicsEngine ) {
            IsLocked.store( false, std::memory_order_release );
            return DDERR_GENERIC;
        }

        byte* data = nullptr;
        INT2 bufferSize{};
        int pixelSize = 0;
        auto* graphicsEngine =
            reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);
        graphicsEngine->ResetPresentPending();
        Engine::GraphicsEngine->OnStartWorldRendering();
        const XRESULT readbackResult = Engine::GraphicsEngine->GetBackbufferData(
            CreatingThumbnail, &data, bufferSize, pixelSize );
        graphicsEngine->ResetPresentPending();

        const uint64_t pitch =
            bufferSize.x > 0 && pixelSize > 0
            ? static_cast<uint64_t>(bufferSize.x) * pixelSize : 0;
        const uint64_t dataSize =
            bufferSize.y > 0 ? pitch * static_cast<uint64_t>(bufferSize.y) : 0;
        if ( readbackResult != XR_SUCCESS || !data
            || bufferSize.x <= 0 || bufferSize.y <= 0 || pixelSize != 4
            || pitch > static_cast<uint64_t>((std::numeric_limits<LONG>::max)())
            || dataSize > static_cast<uint64_t>((std::numeric_limits<size_t>::max)()) ) {
            CreatingThumbnail = false;
            delete[] data;
            data = nullptr;
            LockType = 0;
            IsLocked.store( false, std::memory_order_release );
            return DDERR_GENERIC;
        }

        lpDDSurfaceDesc->ddpfPixelFormat.dwRGBBitCount = 32;
        lpDDSurfaceDesc->ddpfPixelFormat.dwRBitMask = 0x00FF0000;
        lpDDSurfaceDesc->ddpfPixelFormat.dwGBitMask = 0x0000FF00;
        lpDDSurfaceDesc->ddpfPixelFormat.dwBBitMask = 0x000000FF;
        lpDDSurfaceDesc->ddpfPixelFormat.dwRGBAlphaBitMask = 0;
        lpDDSurfaceDesc->lPitch = static_cast<LONG>(pitch);
        lpDDSurfaceDesc->dwWidth = static_cast<DWORD>(bufferSize.x);
        lpDDSurfaceDesc->dwHeight = static_cast<DWORD>(bufferSize.y);
        lpDDSurfaceDesc->lpSurface = data;

        delete[] LockedData;
        LockedData = data;
        LockedDataSize = static_cast<size_t>(dataSize);
        CreatingThumbnail = false;
        return S_OK;
    }

    if ( !EngineTexture ) {
        LockType = 0;
        IsLocked.store( false, std::memory_order_release );
        return DDERR_CANTLOCKSURFACE;
    }

    const int bpp =
        Toolbox::GetNumberOfBits( OriginalSurfaceDesc.ddpfPixelFormat.dwRBitMask )
        + Toolbox::GetNumberOfBits( OriginalSurfaceDesc.ddpfPixelFormat.dwGBitMask )
        + Toolbox::GetNumberOfBits( OriginalSurfaceDesc.ddpfPixelFormat.dwBBitMask )
        + Toolbox::GetNumberOfBits( OriginalSurfaceDesc.ddpfPixelFormat.dwRGBAlphaBitMask );
    const UINT dataSize = EngineTexture->GetSizeInBytes( 0 );
    const UINT rowPitch = EngineTexture->GetRowPitchBytes( 0 );
    if ( dataSize == 0 || rowPitch == 0
        || rowPitch > static_cast<UINT>((std::numeric_limits<LONG>::max)()) ) {
        LockType = 0;
        IsLocked.store( false, std::memory_order_release );
        return DDERR_CANTLOCKSURFACE;
    }

    if ( bpp != 24 || !LockedData || LockedDataSize < dataSize ) {
        delete[] LockedData;
        LockedData = new (std::nothrow) unsigned char[dataSize];
        LockedDataSize = LockedData ? dataSize : 0;
    }
    if ( !LockedData ) {
        LockType = 0;
        IsLocked.store( false, std::memory_order_release );
        return DDERR_OUTOFMEMORY;
    }

    lpDDSurfaceDesc->lpSurface = LockedData;
    lpDDSurfaceDesc->lPitch = static_cast<LONG>(rowPitch);
    return S_OK;
}

#pragma warning(push)
#pragma warning(disable: 6386)
HRESULT MyDirectDrawSurface7::Unlock( LPRECT lpRect ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::Unlock()" );
    (void)lpRect;
    if ( !IsLocked.exchange( false, std::memory_order_acq_rel ) )
        return DDERR_NOTLOCKED;

    const DWORD completedLockType = LockType;
    LockType = 0;

    // Framebuffer readback memory is owned only for the duration of the lock.
    if ( (completedLockType & DDLOCK_READONLY) != 0
        && completedLockType != DDLOCK_READONLY ) {
        delete[] LockedData;
        LockedData = nullptr;
        LockedDataSize = 0;
        return S_OK;
    }

    const int bpp =
        Toolbox::GetNumberOfBits( OriginalSurfaceDesc.ddpfPixelFormat.dwRBitMask )
        + Toolbox::GetNumberOfBits( OriginalSurfaceDesc.ddpfPixelFormat.dwGBitMask )
        + Toolbox::GetNumberOfBits( OriginalSurfaceDesc.ddpfPixelFormat.dwBBitMask )
        + Toolbox::GetNumberOfBits( OriginalSurfaceDesc.ddpfPixelFormat.dwRGBAlphaBitMask );
    const bool keepMovieBuffer = bpp == 24;
    if ( !Engine::GAPI || !EngineTexture || !LockedData ) {
        if ( !keepMovieBuffer ) {
            delete[] LockedData;
            LockedData = nullptr;
            LockedDataSize = 0;
        }
        return DDERR_GENERIC;
    }

    // Texture slot 7 identifies the zCTexture while resource data is loaded.
    if ( zCTexture* boundTexture = Engine::GAPI->GetBoundTexture( 7 ) ) {
        try {
            LoadAdditionalResources( boundTexture );
        } catch ( ... ) {
            // Additional maps are optional; the base texture must still upload.
        }
    }

    XRESULT updateResult = XR_FAILED;
    if ( Engine::GAPI->GetMainThreadID() != GetCurrentThreadId() ) {
        updateResult = EngineTexture->UpdateDataDeferred( LockedData, 0 );
        if ( updateResult == XR_SUCCESS )
            Engine::GAPI->AddFrameLoadedTexture( this );
    } else {
        updateResult = EngineTexture->UpdateData( LockedData, 0 );
        if ( updateResult == XR_SUCCESS )
            SetReady( true );
    }

    if ( !keepMovieBuffer ) {
        delete[] LockedData;
        LockedData = nullptr;
        LockedDataSize = 0;
    }
    return updateResult == XR_SUCCESS ? S_OK : DDERR_GENERIC;
}
#pragma warning(pop)

HRESULT MyDirectDrawSurface7::ReleaseDC( HDC hDC ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::ReleaseDC()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::Restore() {
    DebugWriteTex( "IDirectDrawSurface7(%p)::Restore()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::SetClipper( LPDIRECTDRAWCLIPPER lpDDClipper ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::SetClipper()" );

    if ( lpDDClipper ) lpDDClipper->AddRef();
    if ( Clipper ) Clipper->Release();
    Clipper = lpDDClipper;
    if ( !Clipper ) return S_OK;

    HWND hWnd = nullptr;
    const HRESULT result = Clipper->GetHWnd( &hWnd );
    if ( FAILED( result ) ) return result;
    if ( Engine::GAPI ) Engine::GAPI->OnSetWindow( hWnd );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::SetColorKey( DWORD dwFlags, LPDDCOLORKEY lpDDColorKey ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::SetColorKey(%s, %s)" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::SetOverlayPosition( LONG lX, LONG lY ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::SetOverlayPosition()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::SetPalette( LPDIRECTDRAWPALETTE lpDDPalette ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::SetPalette()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::UpdateOverlay( LPRECT lpSrcRect, LPDIRECTDRAWSURFACE7 lpDDDestSurface, LPRECT lpDestRect, DWORD dwFlags, LPDDOVERLAYFX lpDDOverlayFx ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::UpdateOverlay()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::UpdateOverlayDisplay( DWORD dwFlags ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::UpdateOverlayDisplay()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::UpdateOverlayZOrder( DWORD dwFlags, LPDIRECTDRAWSURFACE7 lpDDSReference ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::UpdateOverlayZOrder()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::GetDDInterface( LPVOID* lplpDD ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::GetDDInterface()" );
    if ( !lplpDD ) return E_POINTER;
    *lplpDD = nullptr;
    return DDERR_UNSUPPORTED;
}

HRESULT MyDirectDrawSurface7::PageLock( DWORD dwFlags ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::PageLock()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::PageUnlock( DWORD dwFlags ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::PageUnlock()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::SetSurfaceDesc( LPDDSURFACEDESC2 lpDDSurfaceDesc, DWORD dwFlags ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::SetSurfaceDesc()" );
    (void)dwFlags;
    if ( !lpDDSurfaceDesc ) return DDERR_INVALIDPARAMS;
    if ( IsLocked.load( std::memory_order_acquire ) ) return DDERR_SURFACEBUSY;

    const DDSURFACEDESC2 candidate = *lpDDSurfaceDesc;
    if ( candidate.dwWidth == 0 ) {
        if ( GothicTexture && Engine::GAPI ) Engine::GAPI->RemoveSurface( this );
        OriginalSurfaceDesc = candidate;
        SAFE_DELETE( EngineTexture );
        SAFE_DELETE( Normalmap );
        SAFE_DELETE( FxMap );
        SAFE_DELETE( Displacementmap );
        delete[] LockedData;
        LockedData = nullptr;
        LockedDataSize = 0;
        GothicTexture = nullptr;
        TextureName.clear();
        TextureType = ETextureType::TX_UNDEF;
        AdditionalResourcesLoaded.store( false, std::memory_order_release );
        IsReady.store( false, std::memory_order_release );
        return S_OK;
    }
    if ( candidate.dwHeight == 0
        || candidate.dwWidth > static_cast<DWORD>((std::numeric_limits<int>::max)())
        || candidate.dwHeight > static_cast<DWORD>((std::numeric_limits<int>::max)()) ) {
        return DDERR_INVALIDPARAMS;
    }
    if ( !Engine::GraphicsEngine ) return DDERR_GENERIC;

    const int bpp =
        Toolbox::GetNumberOfBits( candidate.ddpfPixelFormat.dwRBitMask )
        + Toolbox::GetNumberOfBits( candidate.ddpfPixelFormat.dwGBitMask )
        + Toolbox::GetNumberOfBits( candidate.ddpfPixelFormat.dwBBitMask )
        + Toolbox::GetNumberOfBits( candidate.ddpfPixelFormat.dwRGBAlphaBitMask );

    D3D11Texture::ETextureFormat format;
    if ( bpp == 16 ) {
        switch ( candidate.ddpfPixelFormat.dwFourCC ) {
        case 1: format = D3D11Texture::ETextureFormat::TF_B5G5R5A1; break;
        case 2: format = D3D11Texture::ETextureFormat::TF_B4G4R4A4; break;
        default: format = D3D11Texture::ETextureFormat::TF_B5G6R5; break;
        }
    } else if ( bpp == 24 || bpp == 32 ) {
        format = D3D11Texture::ETextureFormat::TF_B8G8R8A8;
    } else if ( bpp == 8
        || ((candidate.ddpfPixelFormat.dwFlags & (DDPF_ALPHA | DDPF_LUMINANCE)) != 0
            && candidate.ddpfPixelFormat.dwRGBBitCount <= 8) ) {
        format = D3D11Texture::ETextureFormat::TF_R8;
    } else if ( bpp == 0
        && (candidate.ddpfPixelFormat.dwFlags & DDPF_FOURCC) != 0 ) {
        switch ( candidate.ddpfPixelFormat.dwFourCC ) {
        case FOURCC_DXT1:
            format = D3D11Texture::ETextureFormat::TF_DXT1;
            break;
        case FOURCC_DXT2:
        case FOURCC_DXT3:
            format = D3D11Texture::ETextureFormat::TF_DXT3;
            break;
        case FOURCC_DXT4:
        case FOURCC_DXT5:
            format = D3D11Texture::ETextureFormat::TF_DXT5;
            break;
        default:
            return DDERR_INVALIDPIXELFORMAT;
        }
    } else {
        return DDERR_INVALIDPIXELFORMAT;
    }

    UINT mipMapCount = 1;
    if ( (candidate.ddsCaps.dwCaps & DDSCAPS_MIPMAP) != 0 ) {
        mipMapCount = candidate.dwMipMapCount == 0 ? 1 : candidate.dwMipMapCount;
        if ( mipMapCount > 32 ) return DDERR_INVALIDPARAMS;
    }

    std::unique_ptr<D3D11Texture> newEngineTexture(
        new (std::nothrow) D3D11Texture() );
    if ( !newEngineTexture ) return DDERR_OUTOFMEMORY;

    XRESULT initResult = XR_FAILED;
    try {
        initResult = newEngineTexture->Init(
            INT2( static_cast<int>(candidate.dwWidth),
                static_cast<int>(candidate.dwHeight) ),
            format, mipMapCount, nullptr, "DirectDrawSurface7" );
    } catch ( ... ) {
        return DDERR_OUTOFMEMORY;
    }
    if ( initResult != XR_SUCCESS ) return DDERR_OUTOFVIDEOMEMORY;

    if ( GothicTexture && Engine::GAPI ) Engine::GAPI->RemoveSurface( this );
    delete EngineTexture;
    EngineTexture = newEngineTexture.release();
    SAFE_DELETE( Normalmap );
    SAFE_DELETE( FxMap );
    SAFE_DELETE( Displacementmap );
    GothicTexture = nullptr;
    TextureName.clear();
    TextureType = ETextureType::TX_UNDEF;
    AdditionalResourcesLoaded.store( false, std::memory_order_release );
    IsReady.store( false, std::memory_order_release );
    delete[] LockedData;
    LockedData = nullptr;
    LockedDataSize = 0;
    OriginalSurfaceDesc = candidate;
    return S_OK;
}

HRESULT MyDirectDrawSurface7::SetPrivateData( REFGUID guidTag, LPVOID lpData, DWORD cbSize, DWORD dwFlags ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::SetPrivateData()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::GetPrivateData( REFGUID guidTag, LPVOID lpBuffer, LPDWORD lpcbBufferSize ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::GetPrivateData()" );
    (void)guidTag;
    (void)lpBuffer;
    if ( !lpcbBufferSize ) return E_POINTER;
    *lpcbBufferSize = 0;
    return DDERR_NOTFOUND;
}

HRESULT MyDirectDrawSurface7::FreePrivateData( REFGUID guidTag ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::FreePrivateData()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::GetUniquenessValue( LPDWORD lpValue ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::GetUniquenessValue()" );
    if ( !lpValue ) return E_POINTER;
    *lpValue = Uniqueness.load( std::memory_order_relaxed );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::ChangeUniquenessValue() {
    DebugWriteTex( "IDirectDrawSurface7(%p)::ChangeUniquenessValue()" );
    Uniqueness.fetch_add( 1, std::memory_order_relaxed );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::SetPriority( DWORD dwPriority ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::SetPriority()" );
    Priority.store( dwPriority, std::memory_order_relaxed );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::GetPriority( LPDWORD dwPriority ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::GetPriority()" );
    if ( !dwPriority ) return E_POINTER;
    *dwPriority = Priority.load( std::memory_order_relaxed );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::SetLOD( DWORD dwLOD ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::SetLOD()" );
    Lod.store( dwLOD, std::memory_order_relaxed );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::GetLOD( LPDWORD dwLOD ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::GetLOD()" );
    if ( !dwLOD ) return E_POINTER;
    *dwLOD = Lod.load( std::memory_order_relaxed );
    return S_OK;
}

/** Returns the name of this surface */
const std::string& MyDirectDrawSurface7::GetTextureName() {
    return TextureName;
}
