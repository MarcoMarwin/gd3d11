#include "pch.h"
#include "D3D11DeferredRenderer.h"
#include "D3D11GraphicsEngine.h"
#include "D3D11ShaderManager.h"
#include "D3D11ShadowMap.h"
#include "D3D11PShader.h"
#include "RenderGraph.h"
#include "RenderToTextureBuffer.h"
#include "zCTexture.h"
#include "zCMaterial.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "GothicGraphicsState.h"
#include "GSky.h"

static ID3D11ShaderResourceView* s_nullSRVs[16] = { nullptr };

void D3D11DeferredRenderer::AddGeometryPasses( RenderGraph& graph,
    D3D11GraphicsEngine& engine,
    RGResourceHandle colorResource,
    RGResourceHandle velocityBufferHandle,
    RGResourceHandle backBufferHandle,
    RGResourceHandle& outNormalsResource,
    RGResourceHandle& outSpecularResource,
    RGResourceHandle& outReactiveMaskResource,
    RGResourceHandle& outTransparencyAndCompositionMaskResource ) {

    RGResourceHandle normalsResource = RG_INVALID_HANDLE;
    RGResourceHandle specularResource = RG_INVALID_HANDLE;
    RGResourceHandle reactiveMaskResource = RG_INVALID_HANDLE;
    RGResourceHandle transparencyAndCompositionMaskResource = RG_INVALID_HANDLE;

    const auto& rendererSettings = Engine::GAPI->GetRendererState().RendererSettings;
    const bool fsr3MasksActive = rendererSettings.AntiAliasingMode == GothicRendererSettings::AA_FSR3
        && rendererSettings.Upscaler == GothicRendererSettings::UPSCALER_FSR_3;

    graph.AddPass( RG_PASS_NAME("G-Buffer Pass"), [&, colorResource, velocityBufferHandle, backBufferHandle, fsr3MasksActive]( RGBuilder& builder, RenderPass& pass ) {
        auto size = engine.GetResolution();
        normalsResource = builder.CreateTexture( { static_cast<uint32_t>(size.x), static_cast<uint32_t>(size.y), DXGI_FORMAT_R16G16_FLOAT, L"GBufferNormals" } );
        specularResource = builder.CreateTexture( { static_cast<uint32_t>(size.x), static_cast<uint32_t>(size.y), DXGI_FORMAT_R16G16B16A16_FLOAT, L"GBufferSpecular" } );
        if ( fsr3MasksActive ) {
            reactiveMaskResource = builder.CreateTexture( { static_cast<uint32_t>(size.x), static_cast<uint32_t>(size.y), DXGI_FORMAT_R8_UNORM, L"ReactiveMask" } );
            transparencyAndCompositionMaskResource = builder.CreateTexture( { static_cast<uint32_t>(size.x), static_cast<uint32_t>(size.y), DXGI_FORMAT_R8_UNORM, L"TransparencyAndCompositionMask" } );
        }
        builder.Write( colorResource );
        builder.Write( normalsResource );
        builder.Write( specularResource );
        if ( fsr3MasksActive ) {
            builder.Write( velocityBufferHandle );
            builder.Write( reactiveMaskResource );
            builder.Write( transparencyAndCompositionMaskResource );
        }
        builder.Write( backBufferHandle );

        pass.m_executeCallback = [&engine, colorResource, normalsResource, specularResource, reactiveMaskResource, transparencyAndCompositionMaskResource, velocityBufferHandle, fsr3MasksActive]( const RenderGraph& graph ) -> void {
            TracyD3D11ZoneCGX( "D3D11DeferredRenderer::G-Buffer Pass" );
            const auto& context = engine.GetContext();
            context->VSSetShaderResources( 0, 8, s_nullSRVs );
            context->PSSetShaderResources( 0, 8, s_nullSRVs );
            context->CSSetShaderResources( 0, 8, s_nullSRVs );

            auto normals = graph.GetPhysicalTexture( normalsResource );
            auto specular = graph.GetPhysicalTexture( specularResource );
            auto reactiveMask = fsr3MasksActive ? graph.GetPhysicalTexture( reactiveMaskResource ) : nullptr;
            auto transparencyAndCompositionMask = fsr3MasksActive ? graph.GetPhysicalTexture( transparencyAndCompositionMaskResource ) : nullptr;
            auto velocityBuffer = fsr3MasksActive ? graph.GetPhysicalTexture( velocityBufferHandle ) : nullptr;
            ID3D11RenderTargetView* rtvs[] = {
                graph.GetPhysicalTexture( colorResource )->GetRenderTargetView().Get(),
                normals ? normals->GetRenderTargetView().Get() : nullptr,
                specular ? specular->GetRenderTargetView().Get() : nullptr,
                velocityBuffer ? velocityBuffer->GetRenderTargetView().Get() : nullptr,
                transparencyAndCompositionMask ? transparencyAndCompositionMask->GetRenderTargetView().Get() : nullptr,
                reactiveMask ? reactiveMask->GetRenderTargetView().Get() : nullptr,
            };

            constexpr float black[] { 0.f, 0.f, 0.f, 0.f };
            const UINT rtvCount = fsr3MasksActive ? 6u : 3u;
            for ( UINT i = 1; i < rtvCount; i++ ) {
                if ( rtvs[i] )
                    context->ClearRenderTargetView( rtvs[i], black );
            }
            context->OMSetRenderTargets( rtvCount, rtvs, engine.GetDepthBuffer()->GetDepthStencilView().Get() );

            Engine::GAPI->DrawWorldMeshNaive();

            engine.SetViewport( ViewportInfo( 0, 0, engine.GetResolution() ) );

            engine.StoreVobPreviousTransforms();
        };
    } );

    outNormalsResource = normalsResource;
    outSpecularResource = specularResource;
    outReactiveMaskResource = reactiveMaskResource;
    outTransparencyAndCompositionMaskResource = transparencyAndCompositionMaskResource;
}

void D3D11DeferredRenderer::AddLightingPasses( RenderGraph& graph,
    D3D11GraphicsEngine& engine,
    RGResourceHandle colorResource,
    RGResourceHandle normalsResource,
    RGResourceHandle specularResource,
    RGResourceHandle rainExclusionMaskResource,
    RGResourceHandle backBufferHandle,
    std::vector<VobLightInfo*>& frameLights ) {

    graph.AddPass( RG_PASS_NAME("Draw Lighting"), [&, colorResource, normalsResource, specularResource, rainExclusionMaskResource, backBufferHandle]( RGBuilder& builder, RenderPass& pass ) {
        builder.Read( colorResource );
        builder.Read( normalsResource );
        builder.Read( specularResource );
        if ( rainExclusionMaskResource != RG_INVALID_HANDLE ) {
            builder.Read( rainExclusionMaskResource );
        }
        builder.Write( backBufferHandle );

        pass.m_executeCallback = [&engine, &frameLights, colorResource, normalsResource, specularResource, rainExclusionMaskResource]( const RenderGraph& graph ) -> void {
            TracyD3D11ZoneCGX( "D3D11DeferredRenderer::Draw Lighting" );
            auto colorTexture = graph.GetPhysicalTexture( colorResource );
            auto normalsTexture = graph.GetPhysicalTexture( normalsResource );
            auto specularTexture = graph.GetPhysicalTexture( specularResource );
            auto rainExclusionMaskTexture = rainExclusionMaskResource != RG_INVALID_HANDLE
                ? graph.GetPhysicalTexture( rainExclusionMaskResource )
                : nullptr;

            engine.CopyDepthStencil(); // always needed due to depth testing!

            engine.GetShadowMaps()->DrawLighting( frameLights,
                *colorTexture,
                *normalsTexture,
                *specularTexture,
                rainExclusionMaskTexture,
                *engine.GetDepthBufferCopy() );

            if ( !Engine::GAPI->GetRendererState().RendererSettings.FixViewFrustum ) {
                frameLights.clear();
            }
        };
    } );
}

bool D3D11DeferredRenderer::BindShaderForTexture( D3D11ShaderManager& shaderManager,
    std::shared_ptr<D3D11PShader>& activePS,
    zCTexture* texture,
    bool forceAlphaTest,
    int zMatAlphaFunc,
    MaterialInfo::EMaterialType materialType,
    PShaderID resolvedDiffuseNormalmapped,
    PShaderID resolvedDiffuseNormalmappedFxMap,
    PShaderID resolvedDiffuseNormalmappedAlphatest,
    PShaderID resolvedDiffuseNormalmappedAlphatestFxMap,
    bool ) {

    auto active = activePS;
    auto newShader = activePS;
    bool bindParticleAtmosphere = false;

    bool blendAdd = zMatAlphaFunc == zMAT_ALPHA_FUNC_ADD;
    bool blendBlend = zMatAlphaFunc == zMAT_ALPHA_FUNC_BLEND;
    bool linZ = (Engine::GAPI->GetRendererState().GraphicsState.FF_GSwitches & GSWITCH_LINEAR_DEPTH) != 0;
    const bool normalmapsEnabled = Engine::GAPI->GetRendererState().RendererSettings.AllowNormalmaps;
    const bool hasNormalmap = normalmapsEnabled && texture->GetSurface()->GetNormalmap() != nullptr;
    const bool useNormalmapShader = hasNormalmap;
    const bool hasFxMap = hasNormalmap && texture->GetSurface()->GetFxMap();

    if ( materialType == MaterialInfo::MT_Portal ) {
        newShader = shaderManager.GetPShader( PShaderID::PS_PortalDiffuse );
    } else if ( materialType == MaterialInfo::MT_WaterfallFoam ) {
        newShader = shaderManager.GetPShader( PShaderID::PS_WaterfallFoam );
    } else if ( linZ ) {
        newShader = shaderManager.GetPShader( PShaderID::PS_LinDepth );
    } else if ( blendAdd || blendBlend ) {
        newShader = shaderManager.GetPShader( PShaderID::PS_ParticleSimple_FF );
        bindParticleAtmosphere = true;
    } else if ( texture->HasAlphaChannel() || forceAlphaTest ) {
        if ( hasFxMap ) {
            newShader = shaderManager.GetPShader( resolvedDiffuseNormalmappedAlphatestFxMap );
        } else if ( useNormalmapShader ) {
            newShader = shaderManager.GetPShader( resolvedDiffuseNormalmappedAlphatest );
        } else {
            newShader = shaderManager.GetPShader( PShaderID::PS_DiffuseAlphaTest );
        }
    } else {
        if ( hasFxMap ) {
            newShader = shaderManager.GetPShader( resolvedDiffuseNormalmappedFxMap );
        } else if ( useNormalmapShader ) {
            newShader = shaderManager.GetPShader( resolvedDiffuseNormalmapped );
        } else {
            newShader = shaderManager.GetPShader( PShaderID::PS_Diffuse );
        }
    }

    bool changed = active != newShader;
    if ( changed ) {
        activePS = newShader;
        activePS->Apply();
    }
    if ( materialType == MaterialInfo::MT_WaterfallFoam || bindParticleAtmosphere ) {
        if ( GSky* sky = Engine::GAPI->GetSky() ) {
            activePS->GetBuffer( "Atmosphere" ).Update( &sky->GetAtmosphereCB() ).Bind();
        }
    }
    return changed;
}
