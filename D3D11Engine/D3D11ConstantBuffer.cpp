#include "pch.h"
#include "D3D11ConstantBuffer.h"
#include "D3D11GraphicsEngineBase.h"
#include "Engine.h"
#include "GothicAPI.h"

D3D11ConstantBuffer::D3D11ConstantBuffer( int size, void* data ) {
    if ( size <= 0 || (size % 16) != 0 || size > D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 16 ) {
        LogError() << "Invalid constant-buffer size: " << size;
        return;
    }

    D3D11GraphicsEngineBase* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);
    if ( !engine || !engine->GetDevice() ) {
        LogError() << "Cannot create constant buffer without a D3D11 device.";
        return;
    }

    OriginalSize = static_cast<UINT>(size);
    std::vector<uint8_t> zeroData;
    const void* initialBytes = data;
    if ( !initialBytes ) {
        zeroData.resize( OriginalSize, 0 );
        initialBytes = zeroData.data();
    }

    D3D11_SUBRESOURCE_DATA initialData{};
    initialData.pSysMem = initialBytes;

    CD3D11_BUFFER_DESC bufferDesc(
        OriginalSize, D3D11_BIND_CONSTANT_BUFFER, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE );
    const HRESULT hr = engine->GetDevice()->CreateBuffer(
        &bufferDesc, &initialData, Buffer.ReleaseAndGetAddressOf() );
    if ( FAILED( hr ) ) {
        LogError() << "Failed to create constant buffer: 0x" << std::hex << static_cast<unsigned long>(hr);
        OriginalSize = 0;
    }
}
/** Updates the buffer */
D3D11ConstantBuffer* D3D11ConstantBuffer::UpdateBuffer( const void* data ) {
    return UpdateBuffer( data, OriginalSize );
}

D3D11ConstantBuffer* D3D11ConstantBuffer::UpdateBuffer( const void* data, UINT size ) {
    D3D11GraphicsEngineBase* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);
    if ( !engine || !engine->GetContext() || !Buffer || OriginalSize == 0 ) {
        LogError() << "Cannot update an invalid constant buffer.";
        return nullptr;
    }
    if ( size > OriginalSize ) {
        LogError() << "Constant-buffer update exceeds allocation: " << size << " > " << OriginalSize;
        return nullptr;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT hr = engine->GetContext()->Map( Buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped );
    if ( FAILED( hr ) || !mapped.pData ) {
        LogError() << "Failed to map constant buffer: 0x" << std::hex << static_cast<unsigned long>(hr);
        return nullptr;
    }

    if ( !data || size < OriginalSize ) {
        ZeroMemory( mapped.pData, OriginalSize );
    }
    if ( data && size != 0 ) {
        memcpy( mapped.pData, data, size );
    }

    engine->GetContext()->Unmap( Buffer.Get(), 0 );
    BufferDirty = true;
    return this;
}
/** Binds the buffer */
D3D11ConstantBuffer* D3D11ConstantBuffer::BindToVertexShader( int slot ) {
    auto* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);
    if ( !Buffer || slot < 0 || slot >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT
        || !engine || !engine->GetContext() ) {
        return this;
    }
    engine->GetContext()->VSSetConstantBuffers( slot, 1, Buffer.GetAddressOf() );
    BufferDirty = false;
    return this;
}

D3D11ConstantBuffer* D3D11ConstantBuffer::BindToPixelShader( int slot ) {
    auto* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);
    if ( !Buffer || slot < 0 || slot >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT
        || !engine || !engine->GetContext() ) {
        return this;
    }
    engine->GetContext()->PSSetConstantBuffers( slot, 1, Buffer.GetAddressOf() );
    BufferDirty = false;
    return this;
}

D3D11ConstantBuffer* D3D11ConstantBuffer::BindToGeometryShader( int slot ) {
    auto* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);
    if ( !Buffer || slot < 0 || slot >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT
        || !engine || !engine->GetContext() ) {
        return this;
    }
    engine->GetContext()->GSSetConstantBuffers( slot, 1, Buffer.GetAddressOf() );
    BufferDirty = false;
    return this;
}

D3D11ConstantBuffer* D3D11ConstantBuffer::BindToComputeShader( int slot ) {
    auto* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);
    if ( !Buffer || slot < 0 || slot >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT
        || !engine || !engine->GetContext() ) {
        return this;
    }
    engine->GetContext()->CSSetConstantBuffers( slot, 1, Buffer.GetAddressOf() );
    BufferDirty = false;
    return this;
}

/** Returns whether this buffer has been updated since the last bind */
bool D3D11ConstantBuffer::IsDirty() {
    return BufferDirty;
}
