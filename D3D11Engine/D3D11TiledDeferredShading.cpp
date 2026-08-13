#include "pch.h"
#include <algorithm>
#include <cmath>
#include "D3D11TiledDeferredShading.h"

#include "D3D11GraphicsEngine.h"
#include "D3D11LegacyDeferredShading.h"
#include "D3D11PointLight.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "ConstantBufferStructs.h"
#include "D3D11PfxRenderer.h"
#include "D3D11_Helpers.h"
#include "RenderToTextureBuffer.h"
#include "zCVobLight.h"
#include "GSky.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

void D3D11TiledDeferredShading::Init(
    const ComPtr<ID3D11Device1>& device,
    const ComPtr<ID3D11DeviceContext1>& context ) {
    m_device = device;
    m_context = context;

    // Light buffer: dynamic structured buffer for uploading per-frame light data
    {
        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = MAX_TILED_LIGHTS * sizeof( TiledPointLight );
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.StructureByteStride = sizeof( TiledPointLight );

        HRESULT hr = m_device->CreateBuffer( &desc, nullptr, m_LightBuffer.ReleaseAndGetAddressOf() );
        if ( FAILED( hr ) || m_LightBuffer.Get() == nullptr ) {
            LogError() << "Failed to create tiled deferred light buffer. HRESULT: " << std::hex << hr;
            return;
        }
        SetDebugName( m_LightBuffer.Get(), "TiledDeferred_LightBuffer" );

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        srvDesc.Buffer.ElementWidth = MAX_TILED_LIGHTS;

        hr = m_device->CreateShaderResourceView( m_LightBuffer.Get(), &srvDesc, m_LightBufferSRV.ReleaseAndGetAddressOf() );
        if ( FAILED( hr ) || m_LightBufferSRV.Get() == nullptr ) {
            LogError() << "Failed to create tiled deferred light buffer SRV. HRESULT: " << std::hex << hr;
            m_LightBuffer.Reset();
            return;
        }
        SetDebugName( m_LightBufferSRV.Get(), "TiledDeferred_LightBuffer_SRV" );
    }

    // Index counter: single uint for atomic allocation
    {
        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = sizeof( uint32_t ) * 4; // Pad to 16 bytes
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.StructureByteStride = sizeof( uint32_t );

        HRESULT hr = m_device->CreateBuffer( &desc, nullptr, m_IndexCounter.ReleaseAndGetAddressOf() );
        if ( FAILED( hr ) || m_IndexCounter.Get() == nullptr ) {
            LogError() << "Failed to create tiled deferred index counter. HRESULT: " << std::hex << hr;
            return;
        }
        SetDebugName( m_IndexCounter.Get(), "TiledDeferred_IndexCounter" );

        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.NumElements = 4;

        hr = m_device->CreateUnorderedAccessView( m_IndexCounter.Get(), &uavDesc, m_IndexCounterUAV.ReleaseAndGetAddressOf() );
        if ( FAILED( hr ) || m_IndexCounterUAV.Get() == nullptr ) {
            LogError() << "Failed to create tiled deferred index counter UAV. HRESULT: " << std::hex << hr;
            m_IndexCounter.Reset();
            return;
        }
        SetDebugName( m_IndexCounterUAV.Get(), "TiledDeferred_IndexCounter_UAV" );
    }

    // Shadow cube array is lazy-created on first AllocateSlot() to save memory when shadows are off
}

void D3D11TiledDeferredShading::EnsureShadowArray( uint32_t shadowCubeSize ) {
    shadowCubeSize = std::clamp<uint32_t>( shadowCubeSize, 64, 512 );
    if ( m_ShadowArrayCreated && m_ShadowCubeSize == shadowCubeSize ) return;

    if ( m_ShadowArrayCreated && m_ShadowCubeSize != shadowCubeSize ) {
        // Detach every light before reusing the same numerical slots for the
        // replacement textures. Otherwise a later ClearTiledSlot() from an old
        // owner could free a slot that already belongs to another light.
        for ( D3D11PointLight* owner : m_SlotOwners ) {
            if ( owner ) owner->OnTiledSlotEvicted();
        }
        for ( D3D11PointLight* owner : m_StaticLowSlotOwners ) {
            if ( owner ) owner->OnTiledSlotEvicted();
        }
        m_SlotInUse.reset();
        m_SlotOwners.fill( nullptr );
        m_SlotPriorities.fill( FLT_MAX );
        m_StaticLowSlotInUse.reset();
        m_StaticLowSlotOwners.fill( nullptr );
        for ( auto& dsv : m_SlotDSVs ) dsv.Reset();
        for ( auto& view : m_SlotViews ) view.reset();
        for ( auto& dsv : m_DynamicSlotDSVs ) dsv.Reset();
        for ( auto& view : m_DynamicSlotViews ) view.reset();
        m_ShadowCubeArraySRV.Reset();
        m_ShadowCubeArray.Reset();
        m_DynamicShadowCubeArraySRV.Reset();
        m_DynamicShadowCubeArray.Reset();
        for ( auto& dsv : m_StaticLowSlotDSVs ) dsv.Reset();
        for ( auto& view : m_StaticLowSlotViews ) view.reset();
        m_StaticLowShadowCubeArraySRV.Reset();
        m_StaticLowShadowCubeArray.Reset();
        m_ShadowArrayCreated = false;
    }

    m_ShadowCubeSize = shadowCubeSize;

    // TextureCubeArray as depth render target + shader resource (no copies needed)
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = shadowCubeSize;
    desc.Height = shadowCubeSize;
    desc.MipLevels = 1;
    desc.ArraySize = MAX_SHADOW_CUBEMAPS * 6;
    desc.Format = DXGI_FORMAT_R16_TYPELESS;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;

    HRESULT hr = m_device->CreateTexture2D( &desc, nullptr, m_ShadowCubeArray.ReleaseAndGetAddressOf() );
    if ( FAILED( hr ) || m_ShadowCubeArray.Get() == nullptr ) {
        LogError() << "Failed to create tiled shadow cube array. HRESULT: " << std::hex << hr;
        return;
    }
    SetDebugName( m_ShadowCubeArray.Get(), "TiledDeferred_ShadowCubeArray" );

    // Animated overlays are temporal and contain only skeletal/attachment
    // casters. Capping them at 128 keeps the second persistent array at about
    // 24 MiB even when the static point-shadow preset uses 256/512 faces.
    D3D11_TEXTURE2D_DESC dynamicDesc = desc;
    const uint32_t dynamicShadowCubeSize = std::min<uint32_t>( shadowCubeSize, 128u );
    dynamicDesc.Width = dynamicShadowCubeSize;
    dynamicDesc.Height = dynamicShadowCubeSize;
    hr = m_device->CreateTexture2D( &dynamicDesc, nullptr, m_DynamicShadowCubeArray.ReleaseAndGetAddressOf() );
    if ( FAILED( hr ) || m_DynamicShadowCubeArray.Get() == nullptr ) {
        LogError() << "Failed to create tiled dynamic shadow cube array. HRESULT: " << std::hex << hr;
        m_ShadowCubeArray.Reset();
        return;
    }
    SetDebugName( m_DynamicShadowCubeArray.Get(), "TiledDeferred_DynamicShadowCubeArray" );

    // SRV for sampling in the tiled shading CS
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R16_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
    srvDesc.TextureCubeArray.MostDetailedMip = 0;
    srvDesc.TextureCubeArray.MipLevels = 1;
    srvDesc.TextureCubeArray.First2DArrayFace = 0;
    srvDesc.TextureCubeArray.NumCubes = MAX_SHADOW_CUBEMAPS;

    hr = m_device->CreateShaderResourceView( m_ShadowCubeArray.Get(), &srvDesc, m_ShadowCubeArraySRV.ReleaseAndGetAddressOf() );
    if ( FAILED( hr ) || m_ShadowCubeArraySRV.Get() == nullptr ) {
        LogError() << "Failed to create tiled shadow cube array SRV. HRESULT: " << std::hex << hr;
        m_ShadowCubeArray.Reset();
        return;
    }
    SetDebugName( m_ShadowCubeArraySRV.Get(), "TiledDeferred_ShadowCubeArray_SRV" );

    hr = m_device->CreateShaderResourceView( m_DynamicShadowCubeArray.Get(), &srvDesc, m_DynamicShadowCubeArraySRV.ReleaseAndGetAddressOf() );
    if ( FAILED( hr ) || m_DynamicShadowCubeArraySRV.Get() == nullptr ) {
        LogError() << "Failed to create tiled dynamic shadow cube array SRV. HRESULT: " << std::hex << hr;
        m_ShadowCubeArraySRV.Reset();
        m_ShadowCubeArray.Reset();
        m_DynamicShadowCubeArray.Reset();
        return;
    }
    SetDebugName( m_DynamicShadowCubeArraySRV.Get(), "TiledDeferred_DynamicShadowCubeArray_SRV" );

    D3D11_TEXTURE2D_DESC lowDesc = desc;
    lowDesc.Width = 32;
    lowDesc.Height = 32;
    lowDesc.ArraySize = MAX_STATIC_SHADOW_CUBEMAPS * 6;
    hr = m_device->CreateTexture2D( &lowDesc, nullptr, m_StaticLowShadowCubeArray.ReleaseAndGetAddressOf() );
    if ( FAILED( hr ) || !m_StaticLowShadowCubeArray ) {
        LogError() << "Failed to create static low-resolution shadow cube array. HRESULT: " << std::hex << hr;
        return;
    }
    SetDebugName( m_StaticLowShadowCubeArray.Get(), "TiledDeferred_StaticLowShadowCubeArray" );

    D3D11_SHADER_RESOURCE_VIEW_DESC lowSrvDesc = srvDesc;
    lowSrvDesc.TextureCubeArray.NumCubes = MAX_STATIC_SHADOW_CUBEMAPS;
    hr = m_device->CreateShaderResourceView( m_StaticLowShadowCubeArray.Get(), &lowSrvDesc,
        m_StaticLowShadowCubeArraySRV.ReleaseAndGetAddressOf() );
    if ( FAILED( hr ) || !m_StaticLowShadowCubeArraySRV ) {
        LogError() << "Failed to create static low-resolution shadow cube SRV. HRESULT: " << std::hex << hr;
        return;
    }
    SetDebugName( m_StaticLowShadowCubeArraySRV.Get(), "TiledDeferred_StaticLowShadowCubeArray_SRV" );

    // Per-slot DSVs (6 faces each) and RenderToDepthStencilBuffer view wrappers
    for ( uint32_t slot = 0; slot < MAX_SHADOW_CUBEMAPS; slot++ ) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
        dsvDesc.Format = DXGI_FORMAT_D16_UNORM;
        dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
        dsvDesc.Texture2DArray.FirstArraySlice = slot * 6;
        dsvDesc.Texture2DArray.ArraySize = 6;
        dsvDesc.Texture2DArray.MipSlice = 0;

        hr = m_device->CreateDepthStencilView( m_ShadowCubeArray.Get(), &dsvDesc, m_SlotDSVs[slot].ReleaseAndGetAddressOf() );
        if ( FAILED( hr ) || m_SlotDSVs[slot].Get() == nullptr ) {
            LogError() << "Failed to create tiled shadow cube array DSV. HRESULT: " << std::hex << hr;
            for ( auto& dsv : m_SlotDSVs ) dsv.Reset();
            for ( auto& view : m_SlotViews ) view.reset();
            m_ShadowCubeArraySRV.Reset();
            m_ShadowCubeArray.Reset();
            return;
        }

        // View wrapper for RenderShadowCube() interface (uses GetSizeX() and GetDepthStencilView())
        m_SlotViews[slot] = std::make_unique<RenderToDepthStencilBuffer>(
            m_ShadowCubeArray, m_SlotDSVs[slot], nullptr,
            shadowCubeSize, shadowCubeSize );

        hr = m_device->CreateDepthStencilView( m_DynamicShadowCubeArray.Get(), &dsvDesc, m_DynamicSlotDSVs[slot].ReleaseAndGetAddressOf() );
        if ( FAILED( hr ) || m_DynamicSlotDSVs[slot].Get() == nullptr ) {
            LogError() << "Failed to create tiled dynamic shadow cube array DSV. HRESULT: " << std::hex << hr;
            for ( auto& dsv : m_SlotDSVs ) dsv.Reset();
            for ( auto& view : m_SlotViews ) view.reset();
            for ( auto& dsv : m_DynamicSlotDSVs ) dsv.Reset();
            for ( auto& view : m_DynamicSlotViews ) view.reset();
            m_ShadowCubeArraySRV.Reset();
            m_ShadowCubeArray.Reset();
            m_DynamicShadowCubeArraySRV.Reset();
            m_DynamicShadowCubeArray.Reset();
            return;
        }
        m_DynamicSlotViews[slot] = std::make_unique<RenderToDepthStencilBuffer>(
            m_DynamicShadowCubeArray, m_DynamicSlotDSVs[slot], nullptr,
            dynamicShadowCubeSize, dynamicShadowCubeSize );
    }

    m_ShadowArrayCreated = true;
}

int D3D11TiledDeferredShading::AllocateSlot(
    uint32_t shadowCubeSize, bool staticLowRes, D3D11PointLight* owner, float priority ) {
    EnsureShadowArray( shadowCubeSize );
    if ( !m_ShadowArrayCreated ) {
        return -1;
    }

    if ( staticLowRes ) {
        for ( uint32_t i = 0; i < MAX_STATIC_SHADOW_CUBEMAPS; ++i ) {
            if ( !m_StaticLowSlotInUse[i] ) {
                if ( !m_StaticLowSlotViews[i] ) {
                    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
                    dsvDesc.Format = DXGI_FORMAT_D16_UNORM;
                    dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
                    dsvDesc.Texture2DArray.FirstArraySlice = i * 6;
                    dsvDesc.Texture2DArray.ArraySize = 6;
                    dsvDesc.Texture2DArray.MipSlice = 0;
                    const HRESULT hr = m_device->CreateDepthStencilView(
                        m_StaticLowShadowCubeArray.Get(), &dsvDesc,
                        m_StaticLowSlotDSVs[i].ReleaseAndGetAddressOf() );
                    if ( FAILED( hr ) || !m_StaticLowSlotDSVs[i] ) {
                        LogError() << "Failed to create static low-resolution shadow DSV. HRESULT: " << std::hex << hr;
                        continue;
                    }
                    m_StaticLowSlotViews[i] = std::make_unique<RenderToDepthStencilBuffer>(
                        m_StaticLowShadowCubeArray, m_StaticLowSlotDSVs[i], nullptr, 32, 32 );
                }
                m_StaticLowSlotInUse[i] = true;
                m_StaticLowSlotOwners[i] = owner;
                return static_cast<int>(MAX_SHADOW_CUBEMAPS + i);
            }
        }
        return -1;
    }
    for ( uint32_t i = 0; i < MAX_SHADOW_CUBEMAPS; i++ ) {
        if ( !m_SlotInUse[i] ) {
            m_SlotInUse[i] = true;
            m_SlotOwners[i] = owner;
            m_SlotPriorities[i] = priority;
            return static_cast<int>(i);
        }
    }

    // Sticky ownership: a challenger only replaces the least relevant detail
    // slot when it is substantially closer. This prevents camera turns from
    // shuffling cube indices while still guaranteeing a slot for the hero.
    uint32_t worstSlot = 0;
    for ( uint32_t i = 1; i < MAX_SHADOW_CUBEMAPS; ++i ) {
        if ( m_SlotPriorities[i] > m_SlotPriorities[worstSlot] ) worstSlot = i;
    }
    constexpr float kIncumbentBias = 0.35f;
    if ( priority < m_SlotPriorities[worstSlot] * kIncumbentBias ) {
        if ( m_SlotOwners[worstSlot] && m_SlotOwners[worstSlot] != owner ) {
            m_SlotOwners[worstSlot]->OnTiledSlotEvicted();
        }
        m_SlotOwners[worstSlot] = owner;
        m_SlotPriorities[worstSlot] = priority;
        return static_cast<int>(worstSlot);
    }
    return -1;
}

void D3D11TiledDeferredShading::FreeSlot( int slot ) {
    if ( slot >= static_cast<int>(MAX_SHADOW_CUBEMAPS)
        && slot < static_cast<int>(MAX_SHADOW_CUBEMAPS + MAX_STATIC_SHADOW_CUBEMAPS) ) {
        const int lowSlot = slot - MAX_SHADOW_CUBEMAPS;
        m_StaticLowSlotInUse[lowSlot] = false;
        m_StaticLowSlotOwners[lowSlot] = nullptr;
    } else if ( slot >= 0 && static_cast<uint32_t>(slot) < MAX_SHADOW_CUBEMAPS ) {
        m_SlotInUse[slot] = false;
        m_SlotOwners[slot] = nullptr;
        m_SlotPriorities[slot] = FLT_MAX;
    }
}

void D3D11TiledDeferredShading::TouchSlotPriority( int slot, float priority ) {
    if ( slot >= 0 && static_cast<uint32_t>(slot) < MAX_SHADOW_CUBEMAPS ) {
        m_SlotPriorities[slot] = priority;
    }
}

RenderToDepthStencilBuffer* D3D11TiledDeferredShading::GetSlotTarget( int slot ) {
    if ( slot >= static_cast<int>(MAX_SHADOW_CUBEMAPS)
        && slot < static_cast<int>(MAX_SHADOW_CUBEMAPS + MAX_STATIC_SHADOW_CUBEMAPS)
        && m_ShadowArrayCreated )
        return m_StaticLowSlotViews[slot - MAX_SHADOW_CUBEMAPS].get();
    if ( slot >= 0 && static_cast<uint32_t>(slot) < MAX_SHADOW_CUBEMAPS && m_ShadowArrayCreated )
        return m_SlotViews[slot].get();
    return nullptr;
}

RenderToDepthStencilBuffer* D3D11TiledDeferredShading::GetDynamicSlotTarget( int slot ) {
    if ( slot >= 0 && static_cast<uint32_t>(slot) < MAX_SHADOW_CUBEMAPS && m_ShadowArrayCreated )
        return m_DynamicSlotViews[slot].get();
    return nullptr;
}

void D3D11TiledDeferredShading::EnsureBuffers( uint32_t numTilesX, uint32_t numTilesY ) {
    uint32_t totalTiles = numTilesX * numTilesY;

    if ( numTilesX == m_lastNumTilesX && numTilesY == m_lastNumTilesY )
        return;

    m_lastNumTilesX = numTilesX;
    m_lastNumTilesY = numTilesY;


    // Light index list: global flat array of light indices
    {
        const uint32_t MAX_LIGHT_INDEX_ENTRIES = MAX_LIGHTS_PER_TILE * totalTiles;

        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = MAX_LIGHT_INDEX_ENTRIES * sizeof( uint32_t );
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.StructureByteStride = sizeof( uint32_t );

        m_device->CreateBuffer( &desc, nullptr, m_LightIndexList.ReleaseAndGetAddressOf() );
        SetDebugName( m_LightIndexList.Get(), "TiledDeferred_LightIndexList" );

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        srvDesc.Buffer.ElementWidth = MAX_LIGHT_INDEX_ENTRIES;

        m_device->CreateShaderResourceView( m_LightIndexList.Get(), &srvDesc, m_LightIndexListSRV.ReleaseAndGetAddressOf() );
        SetDebugName( m_LightIndexListSRV.Get(), "TiledDeferred_LightIndexList_SRV" );

        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.NumElements = MAX_LIGHT_INDEX_ENTRIES;

        m_device->CreateUnorderedAccessView( m_LightIndexList.Get(), &uavDesc, m_LightIndexListUAV.ReleaseAndGetAddressOf() );
        SetDebugName( m_LightIndexListUAV.Get(), "TiledDeferred_LightIndexList_UAV" );
    }

    // Recreate light grid buffer for new tile count
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = totalTiles * sizeof( LightGrid );
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    desc.StructureByteStride = sizeof( LightGrid );

    m_device->CreateBuffer( &desc, nullptr, m_LightGrid.ReleaseAndGetAddressOf() );
    SetDebugName( m_LightGrid.Get(), "TiledDeferred_LightGrid" );

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    srvDesc.Buffer.ElementWidth = totalTiles;

    m_device->CreateShaderResourceView( m_LightGrid.Get(), &srvDesc, m_LightGridSRV.ReleaseAndGetAddressOf() );
    SetDebugName( m_LightGridSRV.Get(), "TiledDeferred_LightGrid_SRV" );

    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.NumElements = totalTiles;

    m_device->CreateUnorderedAccessView( m_LightGrid.Get(), &uavDesc, m_LightGridUAV.ReleaseAndGetAddressOf() );
    SetDebugName( m_LightGridUAV.Get(), "TiledDeferred_LightGrid_UAV" );
}

XRESULT D3D11TiledDeferredShading::DrawPointlightLights(
    std::vector<VobLightInfo*>& lights,
    RenderToTextureBuffer& color,
    RenderToTextureBuffer& normals,
    RenderToTextureBuffer& specular,
    RenderToTextureBuffer& depthCopy ) {

    auto graphicsEngine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto _ = graphicsEngine->RecordGraphicsEvent( GE_NAME( "TiledPointlightLights" ) );
    auto& context = graphicsEngine->GetContext();

    // ---- Pass 1: Pack lights + cull ----
    auto cullResult = CullLights( lights, depthCopy );

    INT2 resolution = Engine::GraphicsEngine->GetResolution();
    uint32_t numTilesX = (resolution.x + TILE_SIZE - 1) / TILE_SIZE;
    uint32_t numTilesY = (resolution.y + TILE_SIZE - 1) / TILE_SIZE;

    // ---- Pass 2: Tiled Shading (compute) ----
    if ( cullResult.TiledLightCount > 0 ) {
        auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
        XMMATRIX viewRaw = Engine::GAPI->GetViewMatrixXM();

        // Unbind HDR as RTV before binding as UAV
        ID3D11RenderTargetView* nullRTV = nullptr;
        context->OMSetRenderTargets( 1, &nullRTV, nullptr );

        auto csTiledShading = graphicsEngine->GetShaderManager().GetCShader( CShaderID::CS_TiledShading );
        csTiledShading->Apply();

        // Fill and bind shading constant buffer
        TiledShadingConstantBuffer shadeCB = {};
        shadeCB.ViewportSize = float2( static_cast<float>(resolution.x), static_cast<float>(resolution.y) );
        {
            auto& proj = Engine::GAPI->GetProjectionMatrix();
            shadeCB.ProjParams = float4( 1.0f / proj._11, 1.0f / proj._22, proj._43, proj._33 );
        }
        shadeCB.LimitLightIntensity = settings.LimitLightIntesity ? 1 : 0;
        shadeCB.NumTilesX = numTilesX;
        XMStoreFloat4x4( &shadeCB.InvView, XMMatrixInverse( nullptr, viewRaw ) );

        csTiledShading->GetBuffer( "TiledShadingConstantBuffer" ).Update( &shadeCB ).Bind();
        if ( GSky* sky = Engine::GAPI->GetSky() ) {
            auto& atmoCB = sky->GetAtmosphereCB();
            csTiledShading->GetBuffer( "Atmosphere" ).Update( &atmoCB ).Bind();
        }

        // Bind GBuffer SRVs to CS
        context->CSSetShaderResources( 0, 1, color.GetShaderResView().GetAddressOf() );
        context->CSSetShaderResources( 1, 1, normals.GetShaderResView().GetAddressOf() );
        context->CSSetShaderResources( 2, 1, depthCopy.GetShaderResView().GetAddressOf() );
        context->CSSetShaderResources( 7, 1, specular.GetShaderResView().GetAddressOf() );

        // Bind linear sampler to CS slot 0 (required for GBuffer SampleLevel calls)
        ID3D11SamplerState* linearSampler = graphicsEngine->GetDefaultSamplerState();
        context->CSSetSamplers( 0, 1, &linearSampler );

        // Bind tiled data SRVs
        context->CSSetShaderResources( 8, 1, m_LightBufferSRV.GetAddressOf() );
        context->CSSetShaderResources( 9, 1, m_LightGridSRV.GetAddressOf() );
        context->CSSetShaderResources( 10, 1, m_LightIndexListSRV.GetAddressOf() );

        // Bind comparison sampler unconditionally — the runtime validates at Dispatch
        // even if the shader branches around SampleCmpLevelZero
        graphicsEngine->GetShadowMaps()->BindSamplerToCS( context.Get(), 2 );

        // Bind shadow cubemap array SRV
        if ( cullResult.HasShadowedTiledLights && m_ShadowArrayCreated ) {
            ID3D11ShaderResourceView* shadowArrays[2] = {
                m_ShadowCubeArraySRV.Get(), m_DynamicShadowCubeArraySRV.Get()
            };
            context->CSSetShaderResources( 11, 2, shadowArrays );
            context->CSSetShaderResources( 20, 1, m_StaticLowShadowCubeArraySRV.GetAddressOf() );
        }

        // Bind HDR UAV
        auto& hdrUAV = graphicsEngine->GetHDRBackBuffer().GetUnorderedAccessView();
        context->CSSetUnorderedAccessViews( 0, 1, hdrUAV.GetAddressOf(), nullptr );

        context->Dispatch( numTilesX, numTilesY, 1 );

        // Unbind everything
        ID3D11UnorderedAccessView* nullUAV = nullptr;
        context->CSSetUnorderedAccessViews( 0, 1, &nullUAV, nullptr );
        ID3D11ShaderResourceView* nullSRVs[13] = {};
        context->CSSetShaderResources( 0, 13, nullSRVs );
        ID3D11ShaderResourceView* nullLowShadow = nullptr;
        context->CSSetShaderResources( 20, 1, &nullLowShadow );
        context->CSSetShader( nullptr, nullptr, 0 );

        // Restore HDR as RTV
        context->OMSetRenderTargets( 1, graphicsEngine->GetHDRBackBuffer().GetRenderTargetView().GetAddressOf(),
            graphicsEngine->GetDepthBuffer()->GetDepthStencilView().Get() );
    }

    // Draw lights that couldn't go through the tiled path (mismatched shadow cube size, overflow)
    if ( !cullResult.LegacyLights.empty() ) {
        D3D11LegacyDeferredShading legacy;
        legacy.DrawPointlightLights( cullResult.LegacyLights, color, normals, specular, depthCopy );
    }

    return XR_SUCCESS;
}

D3D11TiledDeferredShading::CullResult D3D11TiledDeferredShading::CullLights(
    std::vector<VobLightInfo*>& lights,
    RenderToTextureBuffer& depthCopy ) {

    auto graphicsEngine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto _ = graphicsEngine->RecordGraphicsEvent( GE_NAME( "CullLights" ) );
    auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    const bool fsr3TemporalShadows = settings.Upscaler == GothicRendererSettings::UPSCALER_FSR_3
        && settings.AntiAliasingMode == GothicRendererSettings::AA_FSR3
        && settings.ResolutionScalePercent < 100;
    const bool temporalPointlightShadows = fsr3TemporalShadows;
    const float pointlightRenderScale = fsr3TemporalShadows
        ? std::clamp( static_cast<float>(settings.ResolutionScalePercent) * 0.01f, 0.33f, 1.0f )
        : 1.0f;
    const float temporalPointlightFilterScale = fsr3TemporalShadows
        ? std::clamp( 1.0f / std::sqrt(pointlightRenderScale), 1.0f, 1.75f )
        : 1.0f;
    const float minimumTemporalShadowSoftness = temporalPointlightShadows
        ? 0.35f * temporalPointlightFilterScale
        : 0.0f;
    const float shadowFadeStartRatio = fsr3TemporalShadows
        ? std::clamp( 0.65f + (pointlightRenderScale - 0.5f) * 0.20f, 0.65f, 0.75f )
        : 0.75f;
    auto& context = graphicsEngine->GetContext();

    XMMATRIX viewRaw = Engine::GAPI->GetViewMatrixXM();
    XMMATRIX view = XMMatrixTranspose( viewRaw );

    INT2 resolution = Engine::GraphicsEngine->GetResolution();
    uint32_t numTilesX = (resolution.x + TILE_SIZE - 1) / TILE_SIZE;
    uint32_t numTilesY = (resolution.y + TILE_SIZE - 1) / TILE_SIZE;

    CullResult result = {};

    if ( m_LightBuffer.Get() == nullptr || m_LightBufferSRV.Get() == nullptr
        || m_IndexCounter.Get() == nullptr || m_IndexCounterUAV.Get() == nullptr ) {
        LogError() << "Tiled deferred resources are missing; skipping tiled light culling.";
        return result;
    }

    EnsureBuffers( numTilesX, numTilesY );

    // Partition lights: all lights go tiled where possible.
    // Shadowed lights with a tiled slot render directly into the shared array (no copies).
    // Shadowed lights without a tiled slot (256x256 or overflow) fall back to legacy.
    bool hasShadowedTiledLights = false;

    // Map light buffer
    D3D11_MAPPED_SUBRESOURCE mapped;
    if ( !SUCCEEDED( context->Map( m_LightBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped ) ) ) {
        LogError() << "Failed to map light buffer.";
        return result;
    }
    TiledPointLight* lightData = reinterpret_cast<TiledPointLight*>(mapped.pData);

    const auto camPos = Engine::GAPI->GetCameraPositionXM();

    for ( auto const& light : lights ) {
        zCVobLight* vob = light->Vob;

        if ( !vob->IsEnabled() ) continue;

        // Check if this light has shadows
        D3D11PointLight* pl = nullptr;
        bool hasShadow = false;
        if ( settings.EnablePointlightShadows > 0 ) {
            pl = light->LightShadowBuffers ? static_cast<D3D11PointLight*>(light->LightShadowBuffers.get()) : nullptr;
            if ( pl && pl->IsInited() && pl->HasShadowMap( 1 ) ) {
                hasShadow = !pl->IsTiledStaticLowRes() || pl->IsStaticShadowReady();
            }
        }

        // A missing shadow slot must never suppress the light itself. Render it
        // unshadowed until a slot becomes available again.
        if ( hasShadow && pl->GetTiledSlot() < 0 ) {
            hasShadow = false;
        }

        if ( result.TiledLightCount >= MAX_TILED_LIGHTS )
            continue;

        vob->DoAnimation();

        float4 lightColor = float4( vob->GetLightColor() );
        if ( !light->AllowsPointlightShadows && !light->IsDynamicVobLight && !light->IsVisualFXLight ) {
            lightColor.x *= 0.35f;
            lightColor.y *= 0.35f;
            lightColor.z *= 0.35f;
        }
        float lightRange = vob->GetLightRange();
        float3 posWorld = light->GetEffectivePositionWorld();

        // Distance fade
        float dist;
        XMStoreFloat( &dist, XMVector3Length( XMLoadFloat3( posWorld.toXMFLOAT3() ) - camPos ) );

        const float shadowFadeEnd = std::max( settings.GetEffectiveVisualFXDrawRadius() - lightRange, 1.0f );
        const float shadowFadeStart = shadowFadeEnd * shadowFadeStartRatio;
        const float shadowFadeT = std::clamp(
            (shadowFadeEnd - dist) / std::max( shadowFadeEnd - shadowFadeStart, 1.0f ), 0.0f, 1.0f );
        const float shadowDistanceFade = shadowFadeT * shadowFadeT * (3.0f - 2.0f * shadowFadeT);

        if ( dist + lightRange < settings.GetEffectiveVisualFXDrawRadius() ) {
            float fadeEnd = settings.GetEffectiveVisualFXDrawRadius();
            float fadeFactor = std::min( 1.0f, std::max( 0.0f, ((fadeEnd - (dist + lightRange)) / lightRange) ) );
            lightColor.x *= fadeFactor;
            lightColor.y *= fadeFactor;
            lightColor.z *= fadeFactor;
        }

        float lightFactor = 1.2f;
        lightColor.x *= lightFactor;
        lightColor.y *= lightFactor;
        lightColor.z *= lightFactor;

        if ( lightColor.x <= 0.0f && lightColor.y <= 0.0f && lightColor.z <= 0.0f )
            continue;

        FXMVECTOR posWorldVec = XMLoadFloat3( posWorld.toXMFLOAT3() );
        XMFLOAT3 posView;
        XMStoreFloat3( &posView, XMVector3TransformCoord( posWorldVec, view ) );

        TiledPointLight& tl = lightData[result.TiledLightCount];
        tl.PositionView = posView;
        tl.Range = lightRange;
        tl.Color = XMFLOAT4( lightColor.x, lightColor.y, lightColor.z, lightColor.w );
        tl.PositionWorld = XMFLOAT3( posWorld.x, posWorld.y, posWorld.z );
        tl.ShadowStrength = shadowDistanceFade;
        tl.IsIndoor = light->Vob && light->Vob->IsIndoorVob() ? 1.0f : 0.0f;
        tl.IgnoreIndoorOutdoorLimit = light->IgnoreIndoorOutdoorLimit ? 1.0f : 0.0f;
        tl.ShadowSoftness = std::max(
            settings.ShadowSoftness * 2.0f, minimumTemporalShadowSoftness );

        if ( hasShadow ) {
            constexpr int kShadowHasDynamic = 0x40000000;
            constexpr int kShadowLowStatic = 0x20000000;
            const int physicalSlot = pl->IsTiledStaticLowRes()
                ? pl->GetTiledSlot() - static_cast<int>(MAX_SHADOW_CUBEMAPS)
                : pl->GetTiledSlot();
            tl.ShadowCubeIndex = physicalSlot
                | (pl->HasValidDynamicOverlay() ? kShadowHasDynamic : 0)
                | (pl->IsTiledStaticLowRes() ? kShadowLowStatic : 0);
            hasShadowedTiledLights = true;
        } else {
            tl.ShadowCubeIndex = -1;
        }

        result.TiledLightCount++;
        Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnLights++;
    }

    context->Unmap( m_LightBuffer.Get(), 0 );

    // Dispatch CS_LightCulling if we have lights
    if ( result.TiledLightCount > 0 ) {
        auto csLightCull = graphicsEngine->GetShaderManager().GetCShader( CShaderID::CS_LightCulling );
        csLightCull->Apply();

        LightCullingConstantBuffer cullCB = {};
        cullCB.Proj = Engine::GAPI->GetProjectionMatrix();
        cullCB.ScreenWidth = static_cast<uint32_t>(resolution.x);
        cullCB.ScreenHeight = static_cast<uint32_t>(resolution.y);
        cullCB.TotalLights = result.TiledLightCount;
        cullCB.MaxBufferIndices = (numTilesX * numTilesY) * MAX_LIGHTS_PER_TILE;

        csLightCull->GetBuffer( "LightCullingConstantBuffer" ).Update( &cullCB ).Bind();

        context->CSSetShaderResources( 0, 1, depthCopy.GetShaderResView().GetAddressOf() );
        context->CSSetShaderResources( 1, 1, m_LightBufferSRV.GetAddressOf() );

        UINT clearVal[4] = { 0, 0, 0, 0 };
        context->ClearUnorderedAccessViewUint( m_IndexCounterUAV.Get(), clearVal );

        ID3D11UnorderedAccessView* uavs[3] = { m_LightGridUAV.Get(), m_LightIndexListUAV.Get(), m_IndexCounterUAV.Get() };
        context->CSSetUnorderedAccessViews( 0, 3, uavs, nullptr );

        context->Dispatch( numTilesX, numTilesY, 1 );

        // Unbind
        ID3D11UnorderedAccessView* nullUAVs[3] = { nullptr, nullptr, nullptr };
        context->CSSetUnorderedAccessViews( 0, 3, nullUAVs, nullptr );
        ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
        context->CSSetShaderResources( 0, 2, nullSRVs );
        context->CSSetShader( nullptr, nullptr, 0 );
    }

    result.HasShadowedTiledLights = hasShadowedTiledLights;
    return result;
}
