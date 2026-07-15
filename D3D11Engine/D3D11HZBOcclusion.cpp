#include "pch.h"
#include "D3D11HZBOcclusion.h"

#include "D3D11CShader.h"
#include "D3D11GraphicsEngine.h"
#include "D3D11ShaderManager.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "RenderToTextureBuffer.h"
#include "zTypes.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
    float ClampFloat( float value, float minValue, float maxValue ) {
        return std::max( minValue, std::min( value, maxValue ) );
    }

    int ClampInt( int value, int minValue, int maxValue ) {
        return std::max( minValue, std::min( value, maxValue ) );
    }
}

void D3D11HZBOcclusion::Reset() {
    m_valid = false;
    m_nextWriteReadback = 0;
    m_nextSubmissionSerial = 1;
    m_activeSubmissionSerial = 0;
    for ( UINT i = 0; i < ReadbackCount; ++i ) {
        m_pendingReadback[i] = false;
        m_submissionSerial[i] = 0;
    }
}

void D3D11HZBOcclusion::ReleaseResources() {
    m_depthGrid.Reset();
    m_depthGridUAV.Reset();
    for ( auto& rb : m_readback ) rb.Reset();
    m_mips.clear();
    m_width = 0;
    m_height = 0;
    Reset();
}

bool D3D11HZBOcclusion::EnsureResources( D3D11GraphicsEngine* engine, UINT width, UINT height ) {
    if ( m_width == width && m_height == height && m_depthGrid && m_depthGridUAV && m_readback[0] && m_readback[1] ) return true;

    ReleaseResources();
    if ( !engine || width == 0 || height == 0 ) return false;

    auto device = engine->GetDevice().Get();

    D3D11_TEXTURE2D_DESC gridDesc = {};
    gridDesc.Width = width;
    gridDesc.Height = height;
    gridDesc.MipLevels = 1;
    gridDesc.ArraySize = 1;
    gridDesc.Format = DXGI_FORMAT_R32_FLOAT;
    gridDesc.SampleDesc.Count = 1;
    gridDesc.Usage = D3D11_USAGE_DEFAULT;
    gridDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    if ( FAILED( device->CreateTexture2D( &gridDesc, nullptr, m_depthGrid.ReleaseAndGetAddressOf() ) ) ) return false;

    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_R32_FLOAT;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    if ( FAILED( device->CreateUnorderedAccessView( m_depthGrid.Get(), &uavDesc, m_depthGridUAV.ReleaseAndGetAddressOf() ) ) ) return false;

    D3D11_TEXTURE2D_DESC readbackDesc = gridDesc;
    readbackDesc.Usage = D3D11_USAGE_STAGING;
    readbackDesc.BindFlags = 0;
    readbackDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for ( auto& rb : m_readback ) {
        if ( FAILED( device->CreateTexture2D( &readbackDesc, nullptr, rb.ReleaseAndGetAddressOf() ) ) ) return false;
    }

    m_width = width;
    m_height = height;
    m_mips.clear();
    UINT mipWidth = width;
    UINT mipHeight = height;
    while ( mipWidth >= 1 && mipHeight >= 1 ) {
        MipLevel mip;
        mip.Width = mipWidth;
        mip.Height = mipHeight;
        mip.Depth.resize( static_cast<size_t>( mipWidth ) * static_cast<size_t>( mipHeight ), 0.0f );
        m_mips.push_back( std::move( mip ) );
        if ( mipWidth == 1 && mipHeight == 1 ) break;
        mipWidth = std::max( 1u, mipWidth / 2u );
        mipHeight = std::max( 1u, mipHeight / 2u );
    }
    return true;
}

void D3D11HZBOcclusion::BeginFrame( D3D11GraphicsEngine* engine ) {
    m_valid = false;
    if ( !engine || !m_depthGrid ) return;

    UINT readOrder[ReadbackCount] = { 0, 1 };
    if ( m_submissionSerial[readOrder[0]] > m_submissionSerial[readOrder[1]] ) {
        std::swap( readOrder[0], readOrder[1] );
    }

    auto context = engine->GetContext().Get();
    for ( UINT orderIndex = 0; orderIndex < ReadbackCount; ++orderIndex ) {
        const UINT readIndex = readOrder[orderIndex];
        if ( !m_pendingReadback[readIndex] ) continue;

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        const HRESULT hr = context->Map( m_readback[readIndex].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped );
        if ( SUCCEEDED( hr ) ) {
            if ( m_submissionSerial[readIndex] >= m_activeSubmissionSerial ) {
                m_activeViewProj = m_submittedViewProj[readIndex];
                BuildMipsFromReadback( mapped );
                m_activeSubmissionSerial = m_submissionSerial[readIndex];
                m_valid = true;
            }
            context->Unmap( m_readback[readIndex].Get(), 0 );
            m_pendingReadback[readIndex] = false;
        } else if ( hr != DXGI_ERROR_WAS_STILL_DRAWING ) {
            m_pendingReadback[readIndex] = false;
        }
    }
}

bool D3D11HZBOcclusion::Capture( D3D11GraphicsEngine* engine, RenderToTextureBuffer* depthCopy, const XMMATRIX& view, const XMFLOAT4X4& projection, INT2 resolution ) {
    if ( FeatureLevel10Compatibility || !engine || !depthCopy || resolution.x <= 0 || resolution.y <= 0 ) {
        Reset();
        return false;
    }

    const UINT width = HZBWidth;
    const UINT height = HZBHeight;
    if ( !EnsureResources( engine, width, height ) ) {
        Reset();
        return false;
    }

    UINT writeIndex = ReadbackCount;
    for ( UINT offset = 0; offset < ReadbackCount; ++offset ) {
        const UINT candidate = (m_nextWriteReadback + offset) % ReadbackCount;
        if ( !m_pendingReadback[candidate] ) {
            writeIndex = candidate;
            break;
        }
    }
    if ( writeIndex == ReadbackCount ) return false;

    auto shader = engine->GetShaderManager().GetCShader( CShaderID::CS_HZBOcclusion );
    if ( !shader ) return false;

    auto context = engine->GetContext().Get();
    ID3D11RenderTargetView* nullRTVs[8] = {};
    ID3D11ShaderResourceView* nullSRVs[8] = {};
    ID3D11UnorderedAccessView* nullUAVs[1] = {};
    context->OMSetRenderTargets( 8, nullRTVs, nullptr );
    context->CSSetShaderResources( 0, 8, nullSRVs );
    context->CSSetUnorderedAccessViews( 0, 1, nullUAVs, nullptr );

    shader->Apply();
    HZBConstants constants = { width, height, static_cast<UINT>( resolution.x ), static_cast<UINT>( resolution.y ) };
    shader->GetBuffer( "HZBConstants" ).Update( &constants ).Bind();
    ID3D11ShaderResourceView* depthSRV = depthCopy->GetShaderResView().Get();
    ID3D11UnorderedAccessView* outputUAV = m_depthGridUAV.Get();
    context->CSSetShaderResources( 0, 1, &depthSRV );
    context->CSSetUnorderedAccessViews( 0, 1, &outputUAV, nullptr );
    context->Dispatch( (width + 7u) / 8u, (height + 7u) / 8u, 1 );
    context->CSSetUnorderedAccessViews( 0, 1, nullUAVs, nullptr );
    context->CSSetShaderResources( 0, 8, nullSRVs );
    context->CSSetShader( nullptr, nullptr, 0 );

    const XMMATRIX cpuViewProj = XMMatrixTranspose( XMMatrixMultiply( XMLoadFloat4x4( &projection ), view ) );
    XMStoreFloat4x4( &m_submittedViewProj[writeIndex], cpuViewProj );
    context->CopyResource( m_readback[writeIndex].Get(), m_depthGrid.Get() );

    m_pendingReadback[writeIndex] = true;
    m_submissionSerial[writeIndex] = m_nextSubmissionSerial++;
    m_nextWriteReadback = (writeIndex + 1u) % ReadbackCount;
    return true;
}

void D3D11HZBOcclusion::BuildMipsFromReadback( const D3D11_MAPPED_SUBRESOURCE& mapped ) {
    if ( m_mips.empty() || !mapped.pData ) return;

    MipLevel& base = m_mips[0];
    for ( UINT y = 0; y < base.Height; ++y ) {
        const auto* row = reinterpret_cast<const float*>( reinterpret_cast<const byte*>( mapped.pData ) + static_cast<size_t>( y ) * mapped.RowPitch );
        for ( UINT x = 0; x < base.Width; ++x ) {
            base.Depth[static_cast<size_t>( y ) * base.Width + x] = row[x];
        }
    }

    for ( size_t mipIndex = 1; mipIndex < m_mips.size(); ++mipIndex ) {
        const MipLevel& src = m_mips[mipIndex - 1];
        MipLevel& dst = m_mips[mipIndex];
        for ( UINT y = 0; y < dst.Height; ++y ) {
            for ( UINT x = 0; x < dst.Width; ++x ) {
                const UINT sx = x * 2u;
                const UINT sy = y * 2u;
                float conservativeDepth = 1.0f;
                for ( UINT oy = 0; oy < 2u; ++oy ) {
                    for ( UINT ox = 0; ox < 2u; ++ox ) {
                        const UINT ix = std::min( sx + ox, src.Width - 1u );
                        const UINT iy = std::min( sy + oy, src.Height - 1u );
                        conservativeDepth = std::min( conservativeDepth, src.Depth[static_cast<size_t>( iy ) * src.Width + ix] );
                    }
                }
                dst.Depth[static_cast<size_t>( y ) * dst.Width + x] = conservativeDepth;
            }
        }
    }
}

bool D3D11HZBOcclusion::ProjectBox( const zTBBox3D& bbox, float& minX, float& minY, float& maxX, float& maxY, float& nearestDepth ) const {
    if ( m_width == 0 || m_height == 0 ) return false;

    const XMMATRIX viewProj = XMLoadFloat4x4( &m_activeViewProj );
    const XMFLOAT3 corners[8] = {
        { bbox.Min.x, bbox.Min.y, bbox.Min.z }, { bbox.Max.x, bbox.Min.y, bbox.Min.z },
        { bbox.Min.x, bbox.Max.y, bbox.Min.z }, { bbox.Max.x, bbox.Max.y, bbox.Min.z },
        { bbox.Min.x, bbox.Min.y, bbox.Max.z }, { bbox.Max.x, bbox.Min.y, bbox.Max.z },
        { bbox.Min.x, bbox.Max.y, bbox.Max.z }, { bbox.Max.x, bbox.Max.y, bbox.Max.z }
    };

    minX = std::numeric_limits<float>::max();
    minY = std::numeric_limits<float>::max();
    maxX = -std::numeric_limits<float>::max();
    maxY = -std::numeric_limits<float>::max();
    nearestDepth = 0.0f;

    for ( const auto& corner : corners ) {
        XMVECTOR clip = XMVector4Transform( XMVectorSet( corner.x, corner.y, corner.z, 1.0f ), viewProj );
        XMFLOAT4 clipF;
        XMStoreFloat4( &clipF, clip );
        if ( clipF.w <= 0.0001f ) return false;

        const float invW = 1.0f / clipF.w;
        const float ndcX = clipF.x * invW;
        const float ndcY = clipF.y * invW;
        const float depth = clipF.z * invW;
        if ( depth <= 0.0f || depth >= 1.0f ) return false;

        const float sx = (ndcX * 0.5f + 0.5f) * static_cast<float>( m_width );
        const float sy = (1.0f - (ndcY * 0.5f + 0.5f)) * static_cast<float>( m_height );
        minX = std::min( minX, sx );
        minY = std::min( minY, sy );
        maxX = std::max( maxX, sx );
        maxY = std::max( maxY, sy );
        nearestDepth = std::max( nearestDepth, depth );
    }

    if ( maxX < 0.0f || maxY < 0.0f || minX >= static_cast<float>( m_width ) || minY >= static_cast<float>( m_height ) ) return false;
    minX = ClampFloat( minX - 2.0f, 0.0f, static_cast<float>( m_width - 1u ) );
    minY = ClampFloat( minY - 2.0f, 0.0f, static_cast<float>( m_height - 1u ) );
    maxX = ClampFloat( maxX + 2.0f, 0.0f, static_cast<float>( m_width - 1u ) );
    maxY = ClampFloat( maxY + 2.0f, 0.0f, static_cast<float>( m_height - 1u ) );
    return maxX > minX && maxY > minY;
}

float D3D11HZBOcclusion::SampleMipDepth( const MipLevel& mip, int x, int y ) const {
    x = ClampInt( x, 0, static_cast<int>( mip.Width ) - 1 );
    y = ClampInt( y, 0, static_cast<int>( mip.Height ) - 1 );
    return mip.Depth[static_cast<size_t>( y ) * mip.Width + static_cast<size_t>( x )];
}

bool D3D11HZBOcclusion::IsBoxOccluded( const zTBBox3D& bbox ) const {
    if ( !m_valid || m_mips.empty() ) return false;

    float minX, minY, maxX, maxY, nearestDepth;
    if ( !ProjectBox( bbox, minX, minY, maxX, maxY, nearestDepth ) ) return false;

    const float rectW = maxX - minX;
    const float rectH = maxY - minY;
    const float rectArea = rectW * rectH;

    if ( rectArea > static_cast<float>( m_width * m_height ) * 0.06f ) return false;
    if ( rectW > 72.0f || rectH > 72.0f ) return false;

    size_t mipIndex = 0;
    float mipMinX = minX;
    float mipMinY = minY;
    float mipMaxX = maxX;
    float mipMaxY = maxY;
    while ( mipIndex + 1 < m_mips.size() && std::max( mipMaxX - mipMinX, mipMaxY - mipMinY ) > 7.0f ) {
        ++mipIndex;
        mipMinX *= 0.5f;
        mipMinY *= 0.5f;
        mipMaxX *= 0.5f;
        mipMaxY *= 0.5f;
    }

    const MipLevel& mip = m_mips[mipIndex];
    const int ix0 = ClampInt( static_cast<int>( std::floor( mipMinX ) ), 0, static_cast<int>( mip.Width ) - 1 );
    const int iy0 = ClampInt( static_cast<int>( std::floor( mipMinY ) ), 0, static_cast<int>( mip.Height ) - 1 );
    const int ix1 = ClampInt( static_cast<int>( std::ceil( mipMaxX ) ), 0, static_cast<int>( mip.Width ) - 1 );
    const int iy1 = ClampInt( static_cast<int>( std::ceil( mipMaxY ) ), 0, static_cast<int>( mip.Height ) - 1 );

    const float depthBias = std::max( 0.0035f, nearestDepth * 0.0015f );
    const float requiredOccluderDepth = nearestDepth + depthBias;

    for ( int y = iy0; y <= iy1; ++y ) {
        for ( int x = ix0; x <= ix1; ++x ) {
            if ( SampleMipDepth( mip, x, y ) <= requiredOccluderDepth ) return false;
        }
    }
    return true;
}