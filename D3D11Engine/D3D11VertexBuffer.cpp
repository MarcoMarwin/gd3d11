#include "D3D11VertexBuffer.h"

#include "pch.h"
#include "D3D11GraphicsEngineBase.h"
#include "Engine.h"
#include <meshoptimizer/src/meshoptimizer.h>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>
#include "D3D11_Helpers.h"

namespace {
    constexpr float kOverdrawThreshold = 1.05f;
    constexpr int kNormalQuantizationBits = 10;

    void ConvertIndicesToUInt32( const VERTEX_INDEX* src, size_t count, std::vector<unsigned int>& dst ) {
        dst.resize( count );
        for ( size_t i = 0; i < count; ++i ) {
            dst[i] = src[i];
        }
    }

    bool ConvertIndicesToVertexIndex( const std::vector<unsigned int>& src, VERTEX_INDEX* dst, size_t dstCount ) {
        const unsigned int maxVertexIndex = static_cast<unsigned int>(std::numeric_limits<VERTEX_INDEX>::max());
        if ( src.size() > dstCount ) {
            return false;
        }

        for ( size_t i = 0; i < src.size(); ++i ) {
            if ( src[i] > maxVertexIndex ) {
                return false;
            }

            dst[i] = static_cast<VERTEX_INDEX>(src[i]);
        }

        return true;
    }

    bool ValidateIndices( const VERTEX_INDEX* indices, unsigned int numIndices,
        unsigned int numVertices, const char* operation ) {
        for ( unsigned int i = 0; i < numIndices; ++i ) {
            if ( indices[i] >= numVertices ) {
                LogError() << operation << ": index " << indices[i]
                    << " is outside the vertex range " << numVertices;
                return false;
            }
        }
        return true;
    }

    bool IsValidVertexDataSize( unsigned int numVertices, unsigned int stride ) {
        return numVertices == 0
            || static_cast<size_t>(stride) <= (std::numeric_limits<size_t>::max)() / numVertices;
    }

    float DequantizeSnorm( int v, int bits ) {
        const int maxValue = (1 << (bits - 1)) - 1;
        if ( v > maxValue ) {
            v = maxValue;
        } else if ( v < -maxValue ) {
            v = -maxValue;
        }

        return static_cast<float>(v) / static_cast<float>(maxValue);
    }

    void BuildQuantizedVertexKeyBuffer( const byte* srcVertices, unsigned int numVertices, unsigned int stride, std::vector<byte>& outKeyBuffer ) {
        const size_t totalBytes = static_cast<size_t>(numVertices) * stride;
        outKeyBuffer.assign( srcVertices, srcVertices + totalBytes );

        // Quantize attributes in the key stream to collapse tiny floating-point drift during reindexing.
        if ( stride != sizeof( ExVertexStruct ) ) {
            return;
        }

        for ( unsigned int i = 0; i < numVertices; ++i ) {
            ExVertexStruct v{};
            const size_t offset = static_cast<size_t>(i) * stride;
            memcpy( &v, srcVertices + offset, sizeof( v ) );

            if ( std::isfinite( v.Normal.x ) ) {
                v.Normal.x = DequantizeSnorm( meshopt_quantizeSnorm( v.Normal.x, kNormalQuantizationBits ), kNormalQuantizationBits );
            }
            if ( std::isfinite( v.Normal.y ) ) {
                v.Normal.y = DequantizeSnorm( meshopt_quantizeSnorm( v.Normal.y, kNormalQuantizationBits ), kNormalQuantizationBits );
            }
            if ( std::isfinite( v.Normal.z ) ) {
                v.Normal.z = DequantizeSnorm( meshopt_quantizeSnorm( v.Normal.z, kNormalQuantizationBits ), kNormalQuantizationBits );
            }

            if ( std::isfinite( v.TexCoord.x ) ) v.TexCoord.x = meshopt_dequantizeHalf( meshopt_quantizeHalf( v.TexCoord.x ) );
            if ( std::isfinite( v.TexCoord.y ) ) v.TexCoord.y = meshopt_dequantizeHalf( meshopt_quantizeHalf( v.TexCoord.y ) );
            if ( std::isfinite( v.TexCoord2.x ) ) v.TexCoord2.x = meshopt_dequantizeHalf( meshopt_quantizeHalf( v.TexCoord2.x ) );
            if ( std::isfinite( v.TexCoord2.y ) ) v.TexCoord2.y = meshopt_dequantizeHalf( meshopt_quantizeHalf( v.TexCoord2.y ) );

            memcpy( outKeyBuffer.data() + offset, &v, sizeof( v ) );
        }
    }
}

/** Creates the vertexbuffer with the given arguments */
XRESULT D3D11VertexBuffer::Init( void* initData, unsigned int sizeInBytes, EBindFlags bindFlags, EUsageFlags usage, ECPUAccessFlags cpuAccess, const std::string& fileName, unsigned int structuredByteSize ) {
    D3D11GraphicsEngineBase* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);
    if ( !engine || !engine->GetDevice() || sizeInBytes == 0 || IsMapped ) {
        LogError() << "Invalid vertex-buffer creation request.";
        return XR_INVALID_ARG;
    }

    const UINT nativeBindFlags = static_cast<UINT>(bindFlags);
    constexpr UINT supportedBindFlags = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER
        | D3D11_BIND_STREAM_OUTPUT | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    const UINT nativeCPUAccess = static_cast<UINT>(cpuAccess);
    const bool validUsage = usage == U_DEFAULT || usage == U_DYNAMIC || usage == U_IMMUTABLE;
    const bool validCPUAccess = (nativeCPUAccess & ~(D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE)) == 0;
    const bool validUsageAccess = (usage == U_DYNAMIC && nativeCPUAccess == D3D11_CPU_ACCESS_WRITE)
        || ((usage == U_DEFAULT || usage == U_IMMUTABLE) && nativeCPUAccess == 0);
    if ( nativeBindFlags == 0 || (nativeBindFlags & ~supportedBindFlags) != 0
        || !validUsage || !validCPUAccess || !validUsageAccess ) {
        LogError() << "Invalid vertex-buffer bind, usage, or CPU-access flags.";
        return XR_INVALID_ARG;
    }

    const bool wantsSRV = (nativeBindFlags & D3D11_BIND_SHADER_RESOURCE) != 0;
    const bool wantsUAV = (nativeBindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0;
    if ( wantsSRV && (structuredByteSize == 0 || structuredByteSize > 2048
        || (structuredByteSize % sizeof( uint32_t )) != 0
        || (sizeInBytes % structuredByteSize) != 0) ) {
        LogError() << "Structured vertex-buffer size/stride mismatch.";
        return XR_INVALID_ARG;
    }
    if ( !wantsSRV && structuredByteSize != 0 ) {
        LogError() << "A structured vertex stride requires shader-resource binding.";
        return XR_INVALID_ARG;
    }
    if ( wantsUAV && !wantsSRV && (sizeInBytes % sizeof( uint32_t )) != 0 ) {
        LogError() << "Raw UAV vertex-buffer size must be divisible by four.";
        return XR_INVALID_ARG;
    }

    D3D11_BUFFER_DESC bufferDesc{};
    bufferDesc.ByteWidth = sizeInBytes;
    bufferDesc.Usage = static_cast<D3D11_USAGE>(usage);
    bufferDesc.BindFlags = nativeBindFlags;
    bufferDesc.CPUAccessFlags = nativeCPUAccess;
    if ( wantsSRV ) {
        bufferDesc.MiscFlags |= D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bufferDesc.StructureByteStride = structuredByteSize;
    } else if ( wantsUAV ) {
        bufferDesc.MiscFlags |= D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    }

    if ( usage == U_IMMUTABLE && !initData ) {
        LogError() << "Immutable vertex buffers require initial data.";
        return XR_INVALID_ARG;
    }
    D3D11_SUBRESOURCE_DATA initialData{};
    initialData.pSysMem = initData;

    Microsoft::WRL::ComPtr<ID3D11Buffer> newBuffer;
    HRESULT hr = engine->GetDevice()->CreateBuffer(
        &bufferDesc, initData ? &initialData : nullptr, newBuffer.GetAddressOf() );
    if ( FAILED( hr ) ) {
        LogError() << "Failed to create vertex buffer " << fileName << ": 0x" << std::hex << static_cast<unsigned long>(hr);
        return XR_FAILED;
    }

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> newSRV;
    if ( wantsSRV ) {
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        srvDesc.Buffer.FirstElement = 0;
        srvDesc.Buffer.NumElements = sizeInBytes / structuredByteSize;
        hr = engine->GetDevice()->CreateShaderResourceView( newBuffer.Get(), &srvDesc, newSRV.GetAddressOf() );
        if ( FAILED( hr ) ) {
            LogError() << "Failed to create vertex-buffer SRV " << fileName << ": 0x" << std::hex << static_cast<unsigned long>(hr);
            return XR_FAILED;
        }
    }

    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> newUAV;
    if ( wantsUAV ) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.FirstElement = 0;
        if ( wantsSRV ) {
            uavDesc.Format = DXGI_FORMAT_UNKNOWN;
            uavDesc.Buffer.NumElements = sizeInBytes / structuredByteSize;
        } else {
            uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
            uavDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
            uavDesc.Buffer.NumElements = sizeInBytes / sizeof( uint32_t );
        }
        hr = engine->GetDevice()->CreateUnorderedAccessView( newBuffer.Get(), &uavDesc, newUAV.GetAddressOf() );
        if ( FAILED( hr ) ) {
            LogError() << "Failed to create vertex-buffer UAV " << fileName << ": 0x" << std::hex << static_cast<unsigned long>(hr);
            return XR_FAILED;
        }
    }

    VertexBuffer = std::move( newBuffer );
    ShaderResourceView = std::move( newSRV );
    UnorderedAccessView = std::move( newUAV );
    SizeInBytes = sizeInBytes;
    IsMapped = false;
    SetDebugName( VertexBuffer.Get(), fileName );
    SetDebugName( ShaderResourceView.Get(), fileName + "_SRV" );
    SetDebugName( UnorderedAccessView.Get(), fileName + "_UAV" );
    return XR_SUCCESS;
}
/** Updates the vertexbuffer with the given data */
XRESULT D3D11VertexBuffer::UpdateBuffer( const void* data, UINT size ) {
    if ( !VertexBuffer || !data ) return XR_INVALID_ARG;
    if ( size == 0 ) size = SizeInBytes;
    if ( size > SizeInBytes ) return XR_INVALID_ARG;

    void* mappedData = nullptr;
    UINT mappedSize = 0;

    if ( XR_SUCCESS != Map( EMapFlags::M_WRITE_DISCARD, &mappedData, &mappedSize ) ) {
        return XR_FAILED;
    }
    if ( !mappedData || size > mappedSize ) {
        Unmap();
        return XR_FAILED;
    }

    if ( size < mappedSize ) ZeroMemory( mappedData, mappedSize );
    memcpy( mappedData, data, size );
    return Unmap();
}

/** Maps the buffer */
XRESULT D3D11VertexBuffer::Map( int flags, void** dataPtr, UINT* size ) {
    if ( !dataPtr || !size ) {
        return XR_INVALID_ARG;
    }
    *dataPtr = nullptr;
    *size = 0;
    if ( !VertexBuffer ) {
        return XR_FAILED;
    }
    if ( IsMapped ) return XR_FAILED;
    if ( flags != M_READ && flags != M_WRITE && flags != M_READ_WRITE
        && flags != M_WRITE_DISCARD && flags != M_WRITE_NO_OVERWRITE ) {
        return XR_INVALID_ARG;
    }

    D3D11_MAPPED_SUBRESOURCE res{};
    auto* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);
    auto context = engine ? engine->GetContext() : nullptr;
    if ( !context ) return XR_FAILED;

    const HRESULT mapResult = context->Map(
        VertexBuffer.Get(), 0, static_cast<D3D11_MAP>(flags), 0, &res );
    if ( FAILED( mapResult ) ) return XR_FAILED;
    if ( !res.pData ) {
        context->Unmap( VertexBuffer.Get(), 0 );
        return XR_FAILED;
    }

    IsMapped = true;
    *dataPtr = res.pData;
    *size = SizeInBytes;
    return XR_SUCCESS;
}

/** Unmaps the buffer */
XRESULT D3D11VertexBuffer::Unmap() {
    auto* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);
    if ( !IsMapped ) return XR_INVALID_ARG;
    if ( !VertexBuffer || !engine || !engine->GetContext() ) return XR_FAILED;
    engine->GetContext()->Unmap( VertexBuffer.Get(), 0 );
    IsMapped = false;
    return XR_SUCCESS;
}

/** Returns the D3D11-Buffer object */
Microsoft::WRL::ComPtr <ID3D11Buffer>& D3D11VertexBuffer::GetVertexBuffer() {
    return VertexBuffer;
}

/** Optimizes the given set of vertices */
XRESULT D3D11VertexBuffer::OptimizeVertices( VERTEX_INDEX* indices, byte* vertices, unsigned int numIndices, unsigned int numVertices, unsigned int stride, std::vector<VERTEX_INDEX>* outShadowIndices ) {
    if ( numIndices == 0 || numVertices == 0 ) {
        if ( outShadowIndices ) outShadowIndices->clear();
        return XR_SUCCESS;
    }
    if ( !indices || !vertices || stride == 0 || stride > 256
        || !IsValidVertexDataSize( numVertices, stride ) ) {
        if ( outShadowIndices ) outShadowIndices->clear();
        return XR_INVALID_ARG;
    }

    const size_t maxVertexCount = static_cast<size_t>((std::numeric_limits<VERTEX_INDEX>::max)()) + 1u;
    if ( numVertices > maxVertexCount ) {
        LogError() << "OptimizeVertices: numVertices exceeds VERTEX_INDEX range";
        if ( outShadowIndices ) outShadowIndices->clear();
        return XR_FAILED;
    }
    if ( !ValidateIndices( indices, numIndices, numVertices, "OptimizeVertices" ) ) {
        if ( outShadowIndices ) outShadowIndices->clear();
        return XR_INVALID_ARG;
    }

    ZoneScoped;

    std::vector<unsigned int> indexData;
    ConvertIndicesToUInt32( indices, numIndices, indexData );

    std::vector<unsigned int> remap( numVertices );
    const size_t fetchedVertexCount = meshopt_optimizeVertexFetchRemap( remap.data(), indexData.data(), numIndices, numVertices );
    if ( fetchedVertexCount == 0 || fetchedVertexCount > numVertices ) {
        if ( outShadowIndices ) outShadowIndices->clear();
        return XR_FAILED;
    }

    std::vector<unsigned int> remappedIndices( numIndices );
    meshopt_remapIndexBuffer( remappedIndices.data(), indexData.data(), numIndices, remap.data() );

    std::vector<byte> remappedVertices( static_cast<size_t>(numVertices) * stride );
    memcpy( remappedVertices.data(), vertices, remappedVertices.size() );
    meshopt_remapVertexBuffer( remappedVertices.data(), vertices, numVertices, stride, remap.data() );

    if ( outShadowIndices ) {
        std::vector<unsigned int> shadowIndices( numIndices );
        meshopt_generateShadowIndexBuffer( shadowIndices.data(),
            remappedIndices.data(),
            numIndices,
            remappedVertices.data(),
            fetchedVertexCount,
            sizeof( float ) * 3,
            stride );

        outShadowIndices->resize( numIndices );
        if ( !ConvertIndicesToVertexIndex( shadowIndices, outShadowIndices->data(), outShadowIndices->size() ) ) {
            LogError() << "OptimizeVertices: shadow index exceeds VERTEX_INDEX range";
            outShadowIndices->clear();
            return XR_FAILED;
        }
    }

    if ( !ConvertIndicesToVertexIndex( remappedIndices, indices, numIndices ) ) {
        LogError() << "OptimizeVertices: remapped index exceeds VERTEX_INDEX range";
        if ( outShadowIndices ) {
            outShadowIndices->clear();
        }
        return XR_FAILED;
    }

    memcpy( vertices, remappedVertices.data(), remappedVertices.size() );

    return XR_SUCCESS;
}

/** Optimizes the given set of vertices */
XRESULT D3D11VertexBuffer::OptimizeFaces( VERTEX_INDEX* indices, byte* vertices, unsigned int numIndices, unsigned int numVertices, unsigned int stride ) {
    if ( numIndices == 0 || numVertices == 0 ) return XR_SUCCESS;
    if ( !indices || !vertices || numIndices < 3 || (numIndices % 3) != 0
        || stride == 0 || stride > 256 || !IsValidVertexDataSize( numVertices, stride ) ) {
        return XR_INVALID_ARG;
    }

    const size_t maxVertexCount = static_cast<size_t>((std::numeric_limits<VERTEX_INDEX>::max)()) + 1u;
    if ( numVertices > maxVertexCount ) {
        LogError() << "OptimizeFaces: numVertices exceeds VERTEX_INDEX range";
        return XR_FAILED;
    }
    if ( !ValidateIndices( indices, numIndices, numVertices, "OptimizeFaces" ) ) {
        return XR_INVALID_ARG;
    }

    ZoneScoped;

    std::vector<unsigned int> indexData;
    ConvertIndicesToUInt32( indices, numIndices, indexData );

    // Step 1: Indexing/reindexing with a quantized key stream to reduce float drift duplicates.
    std::vector<byte> remapKeyVertices;
    BuildQuantizedVertexKeyBuffer( vertices, numVertices, stride, remapKeyVertices );

    std::vector<unsigned int> remap( numVertices );
    const size_t indexedVertexCount = meshopt_generateVertexRemap( remap.data(),
        indexData.data(),
        numIndices,
        remapKeyVertices.data(),
        numVertices,
        stride );
    if ( indexedVertexCount == 0 ) {
        return XR_FAILED;
    }

    std::vector<unsigned int> reindexedIndices( numIndices );
    meshopt_remapIndexBuffer( reindexedIndices.data(), indexData.data(), numIndices, remap.data() );

    std::vector<byte> reindexedVertices( static_cast<size_t>(numVertices) * stride );
    memcpy( reindexedVertices.data(), vertices, reindexedVertices.size() );
    meshopt_remapVertexBuffer( reindexedVertices.data(), vertices, numVertices, stride, remap.data() );

    indexData.swap( reindexedIndices );

    // Step 2: Vertex cache optimization.
    meshopt_optimizeVertexCache( indexData.data(), indexData.data(), numIndices, indexedVertexCount );

    // Step 3 (optional): Overdraw optimization.
    if ( stride == sizeof( ExVertexStruct ) ) {
        meshopt_optimizeOverdraw( indexData.data(),
            indexData.data(),
            numIndices,
            reinterpret_cast<const float*>(reindexedVertices.data()),
            indexedVertexCount,
            stride,
            kOverdrawThreshold );
    }

    std::vector<VERTEX_INDEX> optimizedIndices( numIndices );
    if ( !ConvertIndicesToVertexIndex( indexData, optimizedIndices.data(), optimizedIndices.size() ) ) {
        LogError() << "OptimizeFaces: remapped index exceeds VERTEX_INDEX range";
        return XR_FAILED;
    }

    memcpy( vertices, reindexedVertices.data(), reindexedVertices.size() );
    memcpy( indices, optimizedIndices.data(), optimizedIndices.size() * sizeof( VERTEX_INDEX ) );
    return XR_SUCCESS;
}

/** Returns the size in bytes of this buffer */
unsigned int D3D11VertexBuffer::GetSizeInBytes() const {
    return SizeInBytes;
}

/** Returns the SRV of this buffer, if it represents a structured buffer */
Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& D3D11VertexBuffer::GetShaderResourceView() {
    return ShaderResourceView;
}
