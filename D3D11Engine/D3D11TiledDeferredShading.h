#pragma once

#include "pch.h"
#include <wrl/client.h>
#include <vector>
#include <bitset>
#include <array>
#include <DirectXMath.h>
#include "WorldObjects.h"

struct RenderToTextureBuffer;
struct RenderToDepthStencilBuffer;
class D3D11PointLight;

constexpr uint32_t MAX_TILED_LIGHTS = 1024;

// A 16x16 tile is split into logarithmic view-Z clusters.
constexpr uint32_t CLUSTER_Z_SLICES = 16;
constexpr uint32_t CLUSTER_MASK_WORDS = MAX_TILED_LIGHTS / 32;
constexpr float CLUSTER_MIN_FAR_Z = 4096.0f;

constexpr uint32_t MAX_SHADOW_CUBEMAPS = 128;
constexpr uint32_t MAX_STATIC_SHADOW_CUBEMAPS = 340; // 2040 array slices, below D3D11's 2048 limit

struct TiledPointLight {
    DirectX::XMFLOAT3 PositionView;
    float Range;
    DirectX::XMFLOAT4 Color;
    DirectX::XMFLOAT3 PositionWorld;
    int32_t ShadowCubeIndex; // -1 = no shadow, else index into TextureCubeArray
    float ShadowStrength;
    float IsIndoor;
    float IgnoreIndoorOutdoorLimit;
    float ShadowSoftness;
    uint32_t ShadowFilterMode;
    uint32_t ShadowFilterPad[3];
};

static_assert( sizeof(TiledPointLight) == 80, "TiledPointLight must match the HLSL StructuredBuffer layout" );

struct LightGrid {
    uint32_t WordOccupancy;
    uint32_t Mask[CLUSTER_MASK_WORDS];
};
static_assert( sizeof(LightGrid) == (1 + CLUSTER_MASK_WORDS) * sizeof(uint32_t),
    "LightGrid must match the clustered HLSL StructuredBuffer layout" );

class D3D11TiledDeferredShading {
public:
    void Init( const Microsoft::WRL::ComPtr<ID3D11Device1>& device, const Microsoft::WRL::ComPtr<ID3D11DeviceContext1>& context );

    XRESULT DrawPointlightLights(
        std::vector<VobLightInfo*>& lights,
        RenderToTextureBuffer& color,
        RenderToTextureBuffer& normals,
        RenderToTextureBuffer& specular,
        RenderToTextureBuffer& depthCopy );

    /** Packs lights and dispatches CS_LightCulling. */
    struct CullResult {
        uint32_t TiledLightCount = 0;
        bool HasShadowedTiledLights = false;
        std::vector<VobLightInfo*> LegacyLights;
    };
    CullResult CullLights(
        std::vector<VobLightInfo*>& lights,
        RenderToTextureBuffer& depthCopy );

    /** SRVs produced by CullLights. */
    ID3D11ShaderResourceView* GetLightBufferSRV() const { return m_LightBufferSRV.Get(); }
    ID3D11ShaderResourceView* GetLightGridSRV() const { return m_LightGridSRV.Get(); }
    ID3D11ShaderResourceView* GetShadowCubeArraySRV() const { return m_ShadowCubeArraySRV.Get(); }
    ID3D11ShaderResourceView* GetDynamicShadowCubeArraySRV() const { return m_DynamicShadowCubeArraySRV.Get(); }
    ID3D11ShaderResourceView* GetStaticLowShadowCubeArraySRV() const { return m_StaticLowShadowCubeArraySRV.Get(); }
    bool IsShadowArrayCreated() const { return m_ShadowArrayCreated; }

    // Shadow cubemap array slot management
    int AllocateSlot( uint32_t shadowCubeSize, bool staticLowRes, D3D11PointLight* owner, float priority );
    void FreeSlot( int slot );
    void TouchSlotPriority( int slot, float priority );
    void DetachAllOwners();
    RenderToDepthStencilBuffer* GetSlotTarget( int slot );
    RenderToDepthStencilBuffer* GetDynamicSlotTarget( int slot );

private:
    void EnsureBuffers( uint32_t numTilesX, uint32_t numTilesY );
    void EnsureShadowArray( uint32_t shadowCubeSize );

    Microsoft::WRL::ComPtr<ID3D11Device1> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> m_context;

    // Light data buffer (dynamic structured buffer)
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_LightBuffer;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_LightBufferSRV;

    // Clustered light grid: one membership mask for every tile/Z-slice cluster.
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_LightGrid;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_LightGridSRV;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_LightGridUAV;

    // Shadow cubemap array for tiled shadowed lights (lazy-created)
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_ShadowCubeArray;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_ShadowCubeArraySRV;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_DynamicShadowCubeArray;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_DynamicShadowCubeArraySRV;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_StaticLowShadowCubeArray;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_StaticLowShadowCubeArraySRV;
    std::bitset<MAX_SHADOW_CUBEMAPS> m_SlotInUse;
    std::array<Microsoft::WRL::ComPtr<ID3D11DepthStencilView>, MAX_SHADOW_CUBEMAPS> m_SlotDSVs;
    std::array<std::unique_ptr<RenderToDepthStencilBuffer>, MAX_SHADOW_CUBEMAPS> m_SlotViews;
    std::array<Microsoft::WRL::ComPtr<ID3D11DepthStencilView>, MAX_SHADOW_CUBEMAPS> m_DynamicSlotDSVs;
    std::array<std::unique_ptr<RenderToDepthStencilBuffer>, MAX_SHADOW_CUBEMAPS> m_DynamicSlotViews;
    std::array<D3D11PointLight*, MAX_SHADOW_CUBEMAPS> m_SlotOwners{};
    std::array<float, MAX_SHADOW_CUBEMAPS> m_SlotPriorities{};
    std::bitset<MAX_STATIC_SHADOW_CUBEMAPS> m_StaticLowSlotInUse;
    std::array<D3D11PointLight*, MAX_STATIC_SHADOW_CUBEMAPS> m_StaticLowSlotOwners{};
    std::array<Microsoft::WRL::ComPtr<ID3D11DepthStencilView>, MAX_STATIC_SHADOW_CUBEMAPS> m_StaticLowSlotDSVs;
    std::array<std::unique_ptr<RenderToDepthStencilBuffer>, MAX_STATIC_SHADOW_CUBEMAPS> m_StaticLowSlotViews;
    bool m_ShadowArrayCreated = false;
    uint32_t m_ShadowCubeSize = 0;

    uint32_t m_lastNumTilesX = 0;
    uint32_t m_lastNumTilesY = 0;
};
