#pragma once

#include "pch.h"

struct RenderToTextureBuffer;
struct zTBBox3D;
class D3D11GraphicsEngine;

class D3D11HZBOcclusion {
public:
    D3D11HZBOcclusion() = default;
    ~D3D11HZBOcclusion() = default;

    void Reset();
    bool Update( D3D11GraphicsEngine* engine, RenderToTextureBuffer* depthCopy, const XMMATRIX& view, const XMFLOAT4X4& projection, INT2 resolution );
    bool IsBoxOccluded( const zTBBox3D& bbox, float meshSize, bool allowTinyCull ) const;

private:
    struct HZBConstants {
        UINT OutputWidth;
        UINT OutputHeight;
        UINT InputWidth;
        UINT InputHeight;
    };

    struct MipLevel {
        UINT Width = 0;
        UINT Height = 0;
        std::vector<float> Depth;
    };

    bool EnsureResources( D3D11GraphicsEngine* engine, UINT width, UINT height );
    void BuildMipsFromReadback( const D3D11_MAPPED_SUBRESOURCE& mapped );
    bool ProjectBox( const zTBBox3D& bbox, float& minX, float& minY, float& maxX, float& maxY, float& nearestDepth ) const;
    float SampleMipDepth( const MipLevel& mip, int x, int y ) const;
    void ReleaseResources();

    static constexpr UINT HZBWidth = 256;
    static constexpr UINT HZBHeight = 144;
    static constexpr UINT ReadbackCount = 2;

    UINT m_width = 0;
    UINT m_height = 0;
    UINT m_writeReadback = 0;
    bool m_pendingReadback[ReadbackCount] = {};
    bool m_valid = false;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_depthGrid;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_depthGridUAV;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_readback[ReadbackCount];
    XMFLOAT4X4 m_submittedViewProj[ReadbackCount] = {};
    XMFLOAT4X4 m_activeViewProj = {};
    std::vector<MipLevel> m_mips;
};