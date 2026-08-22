#include "D3D11IndirectBuffer.h"

#include "pch.h"
#include "D3D11GraphicsEngineBase.h"
#include "Engine.h"
#include <DirectXMesh.h>
#include "D3D11_Helpers.h"

D3D11IndirectBuffer::D3D11IndirectBuffer() : SizeInBytes(0) {}

D3D11IndirectBuffer::~D3D11IndirectBuffer() {}

/** Creates the buffer with the given arguments */
XRESULT D3D11IndirectBuffer::Init( void* initData, unsigned int sizeInBytes, EBindFlags EBindFlags, EUsageFlags usage, ECPUAccessFlags cpuAccess, const std::string& fileName, unsigned int structuredByteSize ) {
    HRESULT hr;
    D3D11GraphicsEngineBase* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);

    if ( sizeInBytes == 0 ) {
        LogError() << "IndirectBuffer size can't be 0!";
        return XR_FAILED;
    }

    if ( !engine || !engine->GetDevice() || !engine->GetContext()
        || cpuAccess != ECPUAccessFlags::CA_NONE
        || sizeInBytes % 4u != 0 ) {
        LogError() << "Invalid indirect buffer descriptor for " << fileName;
        return XR_FAILED;
    }

    IndirectBuffer.Reset();
    UnorderedAccessView.Reset();
    ShaderResourceView.Reset();
    SizeInBytes = 0;

    SizeInBytes = sizeInBytes;

    // Create our own IndirectBuffer
    D3D11_BUFFER_DESC bufferDesc = {};
    bufferDesc.ByteWidth = sizeInBytes;
    bufferDesc.Usage = static_cast<D3D11_USAGE>(usage);
    bufferDesc.BindFlags = static_cast<D3D11_USAGE>(EBindFlags);
    bufferDesc.CPUAccessFlags = static_cast<D3D11_USAGE>(cpuAccess);
    bufferDesc.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
    bufferDesc.StructureByteStride = 0;

    // Check for unordered access
    if ( (EBindFlags & EBindFlags::B_UNORDERED_ACCESS) != 0 ) {
        bufferDesc.MiscFlags |= D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    }

    // Allocate a minimal buffer when no initial data is provided.
    char* data = nullptr;
    if ( !initData ) {
        data = new char[bufferDesc.ByteWidth];
        memset( data, 0, bufferDesc.ByteWidth );

        initData = data;
    }

    D3D11_SUBRESOURCE_DATA InitData;
    InitData.pSysMem = initData;
    InitData.SysMemPitch = 0;
    InitData.SysMemSlicePitch = 0;

    LE( engine->GetDevice()->CreateBuffer( &bufferDesc, &InitData, IndirectBuffer.ReleaseAndGetAddressOf() ) );
    if ( FAILED( hr ) || !IndirectBuffer.Get() ) {
        LogError() << "[D3D11BufferCreate] type=indirect name=" << fileName
            << " bytes=" << sizeInBytes
            << " bind=0x" << std::hex << static_cast<unsigned int>( EBindFlags )
            << " usage=" << std::dec << static_cast<unsigned int>( usage )
            << " misc=0x" << std::hex << bufferDesc.MiscFlags
            << " hr=0x" << std::hex << static_cast<unsigned long>( hr );
        delete[] data;
        SizeInBytes = 0;
        return XR_FAILED;
    }

    // Check for unordered access again to create the UAV
    if ( (EBindFlags & EBindFlags::B_UNORDERED_ACCESS) != 0 ) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        uavDesc.Buffer.FirstElement = 0;
        uavDesc.Buffer.NumElements = sizeInBytes / 4;

        hr = engine->GetDevice()->CreateUnorderedAccessView( IndirectBuffer.Get(), &uavDesc, UnorderedAccessView.ReleaseAndGetAddressOf() );
        if ( FAILED( hr ) || !UnorderedAccessView ) {
            LogError() << "[D3D11BufferView] type=indirect view=UAV name=" << fileName
                << " bytes=" << sizeInBytes
                << " hr=0x" << std::hex << static_cast<unsigned long>( hr );
            delete[] data;
            IndirectBuffer.Reset();
            SizeInBytes = 0;
            return XR_FAILED;
        }
    }

    if ( (EBindFlags & EBindFlags::B_SHADER_RESOURCE) != 0 ) {
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        srvDesc.BufferEx.FirstElement = 0;
        srvDesc.BufferEx.NumElements = sizeInBytes / 4;
        srvDesc.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        hr = engine->GetDevice()->CreateShaderResourceView(
            IndirectBuffer.Get(), &srvDesc, ShaderResourceView.ReleaseAndGetAddressOf() );
        if ( FAILED( hr ) || !ShaderResourceView ) {
            LogError() << "[D3D11BufferView] type=indirect view=SRV name=" << fileName
                << " bytes=" << sizeInBytes
                << " hr=0x" << std::hex << static_cast<unsigned long>( hr );
            delete[] data;
            IndirectBuffer.Reset();
            UnorderedAccessView.Reset();
            SizeInBytes = 0;
            return XR_FAILED;
        }
        SetDebugName( ShaderResourceView.Get(), fileName + "_SRV" );
    }

    SetDebugName( IndirectBuffer.Get(), fileName );

    delete[] data;

    return XR_SUCCESS;
}

/** Updates the buffer with the given data */
XRESULT D3D11IndirectBuffer::UpdateBuffer( void* data, UINT size ) {
    if ( !IndirectBuffer || !data ) {
        return XR_FAILED;
    }

    void* mappedData;
    UINT bsize;

    if ( SizeInBytes < size ) {
        size = SizeInBytes;
    }

    if ( XR_SUCCESS == Map( EMapFlags::M_WRITE_DISCARD, &mappedData, &bsize ) ) {
        if ( size ) {
            bsize = size;
        }
        // Copy data
        memcpy( mappedData, data, bsize );
        Unmap();

        return XR_SUCCESS;
    }

    return XR_FAILED;
}

XRESULT D3D11IndirectBuffer::UpdateBufferSubresource( const void* data, UINT size ) {
    if ( !IndirectBuffer || !data ) {
        return XR_FAILED;
    }

    if ( size == 0 ) {
        size = SizeInBytes;
    }
    if ( size == 0 || size > SizeInBytes ) {
        return XR_FAILED;
    }

    auto* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);
    if ( !engine || !engine->GetContext() ) {
        return XR_FAILED;
    }

    D3D11_BOX box = { 0, 0, 0, size, 1, 1 };
    engine->GetContext()->UpdateSubresource( IndirectBuffer.Get(), 0, &box, data, 0, 0 );
    return XR_SUCCESS;
}

/** Maps the buffer */
XRESULT D3D11IndirectBuffer::Map( int flags, void** dataPtr, UINT* size ) {
    auto* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);
    if ( !IndirectBuffer || !dataPtr || !size || !engine || !engine->GetContext() ) {
        return XR_FAILED;
    }

    D3D11_MAPPED_SUBRESOURCE res;
    if ( FAILED( engine->GetContext()->Map( IndirectBuffer.Get(), 0, static_cast<D3D11_MAP>(flags), 0, &res ) ) ) {
        return XR_FAILED;
    }

    *dataPtr = res.pData;
    *size = SizeInBytes;

    return XR_SUCCESS;
}

/** Unmaps the buffer */
XRESULT D3D11IndirectBuffer::Unmap() {
    auto* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);
    if ( !IndirectBuffer || !engine || !engine->GetContext() ) {
        return XR_FAILED;
    }

    engine->GetContext()->Unmap( IndirectBuffer.Get(), 0 );
    return XR_SUCCESS;
}

/** Returns the size in bytes of this buffer */
unsigned int D3D11IndirectBuffer::GetSizeInBytes() const {
    return SizeInBytes;
}
