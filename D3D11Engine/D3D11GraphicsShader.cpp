#include "pch.h"
#include "D3D11GraphicsShader.h"
#include <d3dcompiler.h>

#include "D3D11ConstantBuffer.h"
#include "D3D11_Helpers.h"

GraphicsShaderConstantBuffer& GraphicsShaderConstantBuffer::Bind() {
    return Bind(slot);
}

GraphicsShaderConstantBuffer& GraphicsShaderConstantBuffer::Bind(UINT slot) {
    if ( buffer ) {
        shader->BindBuffer(slot, buffer);
    }
    return *this;
}

int32_t D3D11GraphicsShader::GetInputIndex( StringID name )
{
    auto kvp = InputSemanticToIndex.find( name );
    if (kvp != InputSemanticToIndex.end()) {
        return kvp->second;
    }
    return -1;
}

GraphicsShaderConstantBuffer D3D11GraphicsShader::GetBuffer(StringID name) {
    auto kvp = ConstantBuffersByName.find( name );
    if (kvp != ConstantBuffersByName.end()) {
        return GraphicsShaderConstantBuffer(kvp->second.first, kvp->second.second, this);
    }
    return GraphicsShaderConstantBuffer(nullptr, INVALID_SHADER_CB_SLOT, nullptr);
}

GraphicsShaderConstantBuffer D3D11GraphicsShader::GetBuffer(UINT slot) {
    if (slot >= ConstantBufferIndexBySlot.size()) {
        return {nullptr, INVALID_SHADER_CB_SLOT, nullptr};
    }
    const auto idx = ConstantBufferIndexBySlot[slot];
    if ( idx >= ConstantBuffers.size() ) {
        return { nullptr, INVALID_SHADER_CB_SLOT, nullptr };
    }
    return GraphicsShaderConstantBuffer(ConstantBuffers[idx].get(), slot, this);
}

HRESULT D3D11GraphicsShader::ReflectShaderResources( ID3DBlob* shaderBlob ) {
    Microsoft::WRL::ComPtr<ID3D11ShaderReflection> pReflection;
    HRESULT hr = D3DReflect(
        shaderBlob->GetBufferPointer(),
        shaderBlob->GetBufferSize(),
        IID_PPV_ARGS( &pReflection )
    );

    if ( SUCCEEDED( hr ) ) {
        D3D11_SHADER_DESC shaderDesc;
        pReflection->GetDesc( &shaderDesc );

        OnReflectShader(shaderBlob, pReflection.Get(), shaderDesc);
    }
    return hr;
}

void D3D11GraphicsShader::OnReflectShader(ID3DBlob* blob, ID3D11ShaderReflection* pReflection, const D3D11_SHADER_DESC& shaderDesc)
{
    for (size_t i = 0; i< ConstantBuffers.size(); ++i) {
        ConstantBuffers[i].reset();
    }
    ConstantBufferIndexBySlot.fill(INVALID_SHADER_CB_SLOT);
    ConstantBuffersByName.clear();
    ConstantBuffersByName.reserve(shaderDesc.ConstantBuffers);

    // Loop through every resource bound to this shader
    size_t cbIndex = 0;
    for ( UINT i = 0; i < shaderDesc.BoundResources; ++i ) {
        D3D11_SHADER_INPUT_BIND_DESC resourceDesc = {};
        if ( FAILED( pReflection->GetResourceBindingDesc( i, &resourceDesc ) ) )
            continue;

        OnReflectShaderResource(pReflection, shaderDesc, resourceDesc);

        if ( resourceDesc.Type == D3D_SHADER_INPUT_TYPE::D3D_SIT_CBUFFER ) {
            if ( cbIndex >= ConstantBuffers.size()
                || resourceDesc.BindPoint >= ConstantBufferIndexBySlot.size() ) {
                LogError() << "Shader constant buffer exceeds D3D11 slot storage: "
                    << (resourceDesc.Name ? resourceDesc.Name : "<unnamed>")
                    << " at b" << resourceDesc.BindPoint;
                continue;
            }

            auto pCB = pReflection->GetConstantBufferByName( resourceDesc.Name );

            D3D11_SHADER_BUFFER_DESC cbDesc;
            if ( SUCCEEDED( pCB->GetDesc( &cbDesc ) ) ) {
                // cbDesc.Size is the total byte size of the buffer, padded to always be a multiple of 16
                UINT paddedSize = ((cbDesc.Size * resourceDesc.BindCount) + 15) & ~15;

                // Ignore the bind-point here, due to global-per-frame CBs
                ConstantBuffers[cbIndex] = std::make_unique<D3D11ConstantBuffer>( paddedSize, nullptr );
#ifdef DEBUG_D3D11
                SetDebugName(ConstantBuffers[cbIndex]->Get().Get(), resourceDesc.Name);
#endif
                ConstantBuffersByName[StringID::make(resourceDesc.Name)] = {ConstantBuffers[cbIndex].get(), resourceDesc.BindPoint};
                ConstantBufferIndexBySlot[resourceDesc.BindPoint] = static_cast<byte>(cbIndex);
                ++cbIndex;
            }
        }
    }
}

void D3D11GraphicsShader::OnReflectShaderResource(
    ID3D11ShaderReflection* pReflection,
    const D3D11_SHADER_DESC& shaderDesc,
    const D3D11_SHADER_INPUT_BIND_DESC& resourceDesc)
{
    InputSemanticToIndex[StringID::make(resourceDesc.Name)] = resourceDesc.BindPoint;
}
