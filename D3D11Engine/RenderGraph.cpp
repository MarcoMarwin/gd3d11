#include "RenderGraph.h"
#include "BaseGraphicsEngine.h"
#include "Engine.h"
#include "GothicAPI.h"

#include <limits>

RGResourceHandle RGBuilder::Read( RGResourceHandle handle ) {
    if ( !m_graph.IsHandleRegistered( handle ) ) {
        m_graph.Invalidate();
        LogError() << "RenderGraph rejected an invalid read handle.";
        return RG_INVALID_HANDLE;
    }
    if ( std::find( m_pass.m_reads.begin(), m_pass.m_reads.end(), handle ) == m_pass.m_reads.end() ) {
        m_pass.m_reads.push_back( handle );
    }
    return handle;
}

RGResourceHandle RGBuilder::Write( RGResourceHandle handle ) {
    if ( !m_graph.IsHandleRegistered( handle ) ) {
        m_graph.Invalidate();
        LogError() << "RenderGraph rejected an invalid write handle.";
        return RG_INVALID_HANDLE;
    }
    if ( std::find( m_pass.m_writes.begin(), m_pass.m_writes.end(), handle ) == m_pass.m_writes.end() ) {
        m_pass.m_writes.push_back( handle );
    }
    return handle;
}

RGResourceHandle RGBuilder::CreateTexture( const RGTextureDesc& desc ) {
    const RGResourceHandle handle = m_graph.RegisterResource( desc );
    return IsValidHandle( handle ) ? Write( handle ) : RG_INVALID_HANDLE;
}

RGResourceHandle RenderGraph::ImportResource(
    const std::wstring& name, RenderToTextureBuffer* externalBuffer ) {
    if ( !externalBuffer || !externalBuffer->IsValid() ) {
        Invalidate();
        LogError() << "RenderGraph rejected an invalid external texture.";
        return RG_INVALID_HANDLE;
    }
    if ( m_nextHandle > RG_MAX_RESOURCE_INDEX ) {
        Invalidate();
        LogError() << "RenderGraph resource handle space is exhausted.";
        return RG_INVALID_HANDLE;
    }

    const uint32_t index = m_nextHandle;
    const uint32_t newSize = index + 1;
    m_externalTextures.resize( newSize, nullptr );
    m_activeTextures.resize( newSize );
    m_resourceDescs.resize( newSize );

    m_externalTextures[index] = externalBuffer;
    m_resourceDescs[index] = { 0, 0, 0, name };
    m_nextHandle = newSize;
    m_compiled = false;
    return MakeHandle( index, true );
}

RGResourceHandle RenderGraph::RegisterResource( const RGTextureDesc& desc ) {
    constexpr uint32_t supportedBindFlags = D3D11_BIND_RENDER_TARGET
        | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if ( desc.width == 0 || desc.height == 0
        || desc.width > static_cast<uint32_t>((std::numeric_limits<int>::max)())
        || desc.height > static_cast<uint32_t>((std::numeric_limits<int>::max)())
        || desc.format == DXGI_FORMAT_UNKNOWN
        || (desc.textureFlags & ~supportedBindFlags) != 0 ) {
        Invalidate();
        LogError() << "RenderGraph rejected an invalid transient texture description.";
        return RG_INVALID_HANDLE;
    }
    if ( m_nextHandle > RG_MAX_RESOURCE_INDEX ) {
        Invalidate();
        LogError() << "RenderGraph resource handle space is exhausted.";
        return RG_INVALID_HANDLE;
    }

    const uint32_t index = m_nextHandle;
    const uint32_t newSize = index + 1;
    m_externalTextures.resize( newSize, nullptr );
    m_activeTextures.resize( newSize );
    m_resourceDescs.resize( newSize );

    m_resourceDescs[index] = desc;
    m_nextHandle = newSize;
    m_compiled = false;
    return MakeHandle( index, false );
}

bool RenderGraph::Compile() {
    m_compiled = false;
    if ( !m_valid ) {
        LogError() << "RenderGraph compilation rejected an invalid graph.";
        return false;
    }
    if ( m_passes.size() > UINT32_MAX ) {
        LogError() << "RenderGraph contains too many passes.";
        return false;
    }

    m_resourceLifetimes.assign(
        m_nextHandle, { UINT32_MAX, UINT32_MAX, 0, false } );

    for ( size_t passIndex = 0; passIndex < m_passes.size(); ++passIndex ) {
        const auto& pass = m_passes[passIndex];
        if ( !pass ) {
            LogError() << "RenderGraph contains a null pass.";
            return false;
        }
        const uint32_t passNumber = static_cast<uint32_t>(passIndex);

        for ( RGResourceHandle writeHandle : pass->m_writes ) {
            if ( !IsHandleRegistered( writeHandle ) ) {
                LogError() << "RenderGraph pass writes an invalid resource handle.";
                return false;
            }
            Lifetime& lifetime = m_resourceLifetimes[GetHandleIndex( writeHandle )];
            lifetime.firstPass = (std::min)(lifetime.firstPass, passNumber);
            lifetime.lastPass = (std::max)(lifetime.lastPass, passNumber);
        }

        for ( RGResourceHandle readHandle : pass->m_reads ) {
            if ( !IsHandleRegistered( readHandle ) ) {
                LogError() << "RenderGraph pass reads an invalid resource handle.";
                return false;
            }
            Lifetime& lifetime = m_resourceLifetimes[GetHandleIndex( readHandle )];
            lifetime.firstReadPass = (std::min)(lifetime.firstReadPass, passNumber);
            lifetime.lastPass = (std::max)(lifetime.lastPass, passNumber);
            lifetime.isRead = true;
        }
    }

    for ( uint32_t index = 0; index < m_nextHandle; ++index ) {
        const Lifetime& lifetime = m_resourceLifetimes[index];
        if ( m_externalTextures[index] != nullptr || !lifetime.isRead ) continue;
        if ( lifetime.firstPass == UINT32_MAX ) {
            LogError() << "RenderGraph transient resource is read but never written.";
            return false;
        }
        if ( lifetime.firstReadPass < lifetime.firstPass ) {
            LogError() << "RenderGraph transient resource is read before its first write.";
            return false;
        }
    }

    m_compiled = true;
    return true;
}

bool RenderGraph::Execute() {
    ZoneScopedN( "RenderGraph::Execute" );
    if ( !m_compiled && !Compile() ) return false;

    ReleaseAllResources();
    for ( size_t passIndex = 0; passIndex < m_passes.size(); ++passIndex ) {
        const auto& pass = m_passes[passIndex];

        bool isPassDead = !pass->m_writes.empty();
        for ( RGResourceHandle writeHandle : pass->m_writes ) {
            const uint32_t index = GetHandleIndex( writeHandle );
            if ( IsExternalHandle( writeHandle ) || m_resourceLifetimes[index].isRead ) {
                isPassDead = false;
                break;
            }
        }
        if ( isPassDead ) {
            ReleaseResourcesForPass( passIndex );
            continue;
        }

        if ( !AllocateResourcesForPass( passIndex ) ) {
            LogError() << "RenderGraph execution stopped because a transient texture could not be allocated.";
            ReleaseAllResources();
            return false;
        }

        try {
            if ( pass->m_executeCallback ) {
                std::unique_ptr<GraphicsEventRecord> event;
                if ( Engine::GraphicsEngine ) {
                    event = Engine::GraphicsEngine->RecordGraphicsEvent(
                        { pass->m_name.wide, pass->m_name.narrow } );
                }
                ZoneScoped;
                ZoneName( pass->m_name.narrow, strlen( pass->m_name.narrow ) );
                pass->m_executeCallback( *this );
            }
        } catch ( const std::exception& error ) {
            LogError() << "RenderGraph pass failed: " << error.what();
            ReleaseAllResources();
            return false;
        } catch ( ... ) {
            LogError() << "RenderGraph pass failed unexpectedly.";
            ReleaseAllResources();
            return false;
        }

        ReleaseResourcesForPass( passIndex );
    }

    ReleaseAllResources();
    return true;
}

bool RenderGraph::AllocateResourcesForPass( size_t passIndex ) {
    if ( passIndex > UINT32_MAX || !m_texturePool ) return false;
    const uint32_t passNumber = static_cast<uint32_t>(passIndex);

    for ( size_t index = 0; index < m_resourceLifetimes.size(); ++index ) {
        const Lifetime& lifetime = m_resourceLifetimes[index];
        if ( lifetime.firstPass != passNumber || m_externalTextures[index] != nullptr
            || !lifetime.isRead ) {
            continue;
        }

        const RGTextureDesc& desc = m_resourceDescs[index];
        TexturePool::Description poolDesc{
            static_cast<int>(desc.width), static_cast<int>(desc.height),
            static_cast<DXGI_FORMAT>(desc.format), desc.textureFlags
        };
        m_activeTextures[index] = m_texturePool->Acquire( poolDesc );
        if ( !m_activeTextures[index] ) return false;
    }
    return true;
}