#include "D3D11IndirectBuffer.h"

#include "pch.h"
#include "D3D11GraphicsEngineBase.h"
#include "Engine.h"
#include "D3D11_Helpers.h"

/** Creates the buffer with the given arguments */
XRESULT D3D11IndirectBuffer::Init( void* initData, unsigned int sizeInBytes, EBindFlags bindFlags, EUsageFlags usage, ECPUAccessFlags cpuAccess, const std::string& fileName, unsigned int structuredByteSize ) {
    auto* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);
    if ( !engine || !engine->GetDevice() || IsMapped || sizeInBytes == 0
        || (sizeInBytes % sizeof( uint32_t )) != 0 || structuredByteSize != 0 ) {
        LogError() << "Invalid indirect-buffer creation request.";
        return XR_INVALID_ARG;
    }

    const UINT nativeBindFlags = static_cast<UINT>(bindFlags);
    constexpr UINT supportedBindFlags = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER
        | D3D11_BIND_STREAM_OUTPUT | D3D11_BIND_UNORDERED_ACCESS;
    const UINT nativeCPUAccess = static_cast<UINT>(cpuAccess);
    const bool validUsage = usage == U_DEFAULT || usage == U_DYNAMIC || usage == U_IMMUTABLE;
    const bool validCPUAccess = (nativeCPUAccess & ~(D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE)) == 0;
    const bool validUsageAccess = (usage == U_DYNAMIC && nativeCPUAccess == D3D11_CPU_ACCESS_WRITE)
        || ((usage == U_DEFAULT || usage == U_IMMUTABLE) && nativeCPUAccess == 0);
    const bool wantsUAV = (nativeBindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0;
    if ( (nativeBindFlags & ~supportedBindFlags) != 0 || !validUsage
        || !validCPUAccess || !validUsageAccess || (wantsUAV && usage != U_DEFAULT)
        || (usage == U_IMMUTABLE && !initData) ) {
        LogError() << "Invalid indirect-buffer bind, usage, or CPU-access flags.";
        return XR_INVALID_ARG;
    }

    D3D11_BUFFER_DESC bufferDesc{};
    bufferDesc.ByteWidth = sizeInBytes;
    bufferDesc.Usage = static_cast<D3D11_USAGE>(usage);
    bufferDesc.BindFlags = nativeBindFlags;
    bufferDesc.CPUAccessFlags = nativeCPUAccess;
    bufferDesc.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
    if ( wantsUAV ) {
        bufferDesc.MiscFlags |= D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    }

    D3D11_SUBRESOURCE_DATA initialData{};
    initialData.pSysMem = initData;

    Microsoft::WRL::ComPtr<ID3D11Buffer> newBuffer;
    HRESULT hr = engine->GetDevice()->CreateBuffer(
        &bufferDesc, initData ? &initialData : nullptr, newBuffer.GetAddressOf() );
    if ( FAILED( hr ) ) {
        LogError() << "Failed to create indirect buffer " << fileName
            << ": 0x" << std::hex << static_cast<unsigned long>(hr);
        return XR_FAILED;
    }

    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> newUAV;
    if ( wantsUAV ) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        uavDesc.Buffer.NumElements = sizeInBytes / sizeof( uint32_t );
        hr = engine->GetDevice()->CreateUnorderedAccessView(
            newBuffer.Get(), &uavDesc, newUAV.GetAddressOf() );
        if ( FAILED( hr ) ) {
            LogError() << "Failed to create indirect-buffer UAV " << fileName
                << ": 0x" << std::hex << static_cast<unsigned long>(hr);
            return XR_FAILED;
        }
    }

    IndirectBuffer = std::move( newBuffer );
    UnorderedAccessView = std::move( newUAV );
    SizeInBytes = sizeInBytes;
    IsMapped = false;
    SetDebugName( IndirectBuffer.Get(), fileName );
    SetDebugName( UnorderedAccessView.Get(), fileName + "_UAV" );
    return XR_SUCCESS;
}
/** Updates the buffer with the given data */
XRESULT D3D11IndirectBuffer::UpdateBuffer( void* data, UINT size ) {
    if ( !IsValid() || !data ) return XR_INVALID_ARG;

    const UINT copySize = size == 0 ? SizeInBytes : size;
    if ( copySize > SizeInBytes ) return XR_INVALID_ARG;

    void* mappedData = nullptr;
    UINT mappedSize = 0;
    if ( Map( EMapFlags::M_WRITE_DISCARD, &mappedData, &mappedSize ) != XR_SUCCESS ) {
        return XR_FAILED;
    }
    if ( !mappedData || copySize > mappedSize ) {
        Unmap();
        return XR_FAILED;
    }

    if ( copySize < mappedSize ) ZeroMemory( mappedData, mappedSize );
    memcpy( mappedData, data, copySize );
    return Unmap();
}

/** Maps the buffer */
XRESULT D3D11IndirectBuffer::Map( int flags, void** dataPtr, UINT* size ) {
    if ( !dataPtr || !size ) {
        return XR_INVALID_ARG;
    }
    *dataPtr = nullptr;
    *size = 0;
    if ( !IsValid() ) return XR_FAILED;
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
        IndirectBuffer.Get(), 0, static_cast<D3D11_MAP>(flags), 0, &res );
    if ( FAILED( mapResult ) ) return XR_FAILED;
    if ( !res.pData ) {
        context->Unmap( IndirectBuffer.Get(), 0 );
        return XR_FAILED;
    }

    IsMapped = true;
    *dataPtr = res.pData;
    *size = SizeInBytes;
    return XR_SUCCESS;
}

/** Unmaps the buffer */
XRESULT D3D11IndirectBuffer::Unmap() {
    auto* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);
    if ( !IsMapped ) return XR_INVALID_ARG;
    if ( !IsValid() || !engine || !engine->GetContext() ) return XR_FAILED;

    engine->GetContext()->Unmap( IndirectBuffer.Get(), 0 );
    IsMapped = false;
    return XR_SUCCESS;
}

/** Returns the size in bytes of this buffer */
unsigned int D3D11IndirectBuffer::GetSizeInBytes() const {
    return SizeInBytes;
}
