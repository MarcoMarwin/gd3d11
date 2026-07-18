#include "pch.h"
#include "D3D11Effect.h"
#include "D3D11GraphicsEngineBase.h"
#include "Engine.h"
#include "D3D11ShaderManager.h"
#include "GothicAPI.h"
#include "D3D11VertexBuffer.h"
#include "D3D11VShader.h"
#include "D3D11GShader.h"
#include "D3D11PShader.h"
#include "D3D11ConstantBuffer.h"
#include "GSky.h"
#include <DDSTextureLoader.h>
#include "RenderToTextureBuffer.h"
#include "D3D11_Helpers.h"

// TODO: Remove this!
#include "D3D11GraphicsEngine.h"
#include "oCGame.h"
#include "zFILE_VDFS.h"
#include <array>
#include <new>

constexpr float snowSpeedFactor = 0.15f;

namespace {
    const float2 snowScale( 3.0f, 3.0f );
    const float2 rainScale( 30.0f / 10.0f, 30.0f / 8.0f );
    constexpr float MAX_RAIN_RANGE = 100000.0f;
    constexpr float MAX_RAIN_VELOCITY = 100000.0f;

    float SanitizeRainRange( float value, float fallback ) {
        return std::isfinite( value ) && value > 0.0f
            ? (std::min)(value, MAX_RAIN_RANGE)
            : fallback;
    }

    XMFLOAT3 SanitizeRainVelocity( const XMFLOAT3& velocity ) {
        auto sanitize = []( float value ) {
            return std::isfinite( value )
                ? (std::max)(-MAX_RAIN_VELOCITY, (std::min)(value, MAX_RAIN_VELOCITY))
                : 0.0f;
        };
        return XMFLOAT3( sanitize( velocity.x ), sanitize( velocity.y ), sanitize( velocity.z ) );
    }
}

D3D11Effect::D3D11Effect() {
    RainBufferStatic = nullptr;
    RainBufferDrawFrom = nullptr;
    RainBufferStreamTo = nullptr;
    RainBufferInitial = nullptr;
}

D3D11Effect::~D3D11Effect() {
    delete RainBufferStatic;
    delete RainBufferInitial;
    delete RainBufferDrawFrom;
    delete RainBufferStreamTo;
}

ID3D11BlendState* D3D11Effect::GetRainReactiveBlendState() {
    if ( m_RainReactiveBlendState ) {
        return m_RainReactiveBlendState.Get();
    }

    auto* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);
    if ( !engine || !engine->GetDevice().Get() ) {
        return nullptr;
    }

    D3D11_BLEND_DESC desc = {};
    desc.IndependentBlendEnable = TRUE;

    auto& colorTarget = desc.RenderTarget[0];
    colorTarget.BlendEnable = TRUE;
    colorTarget.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    colorTarget.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    colorTarget.BlendOp = D3D11_BLEND_OP_ADD;
    colorTarget.SrcBlendAlpha = D3D11_BLEND_ONE;
    colorTarget.DestBlendAlpha = D3D11_BLEND_ZERO;
    colorTarget.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    colorTarget.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    auto& reactiveTarget = desc.RenderTarget[1];
    reactiveTarget.BlendEnable = TRUE;
    reactiveTarget.SrcBlend = D3D11_BLEND_ONE;
    reactiveTarget.DestBlend = D3D11_BLEND_ONE;
    reactiveTarget.BlendOp = D3D11_BLEND_OP_MAX;
    reactiveTarget.SrcBlendAlpha = D3D11_BLEND_ONE;
    reactiveTarget.DestBlendAlpha = D3D11_BLEND_ONE;
    reactiveTarget.BlendOpAlpha = D3D11_BLEND_OP_MAX;
    reactiveTarget.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED;

    if ( FAILED( engine->GetDevice()->CreateBlendState( &desc, m_RainReactiveBlendState.GetAddressOf() ) ) ) {
        LogError() << "Rain: Failed to create independent FSR3 reactive-mask blend state.";
        return nullptr;
    }
    return m_RainReactiveBlendState.Get();
}

/** Loads a texturearray. Use like the following: Put path and prefix as parameter. The files must then be called name_xxxx.dds */
HRESULT LoadTextureArray( Microsoft::WRL::ComPtr<ID3D11Device1> pd3dDevice, Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context, const char* sTexturePrefix, int iNumTextures, ID3D11Texture2D** ppTex2D, ID3D11ShaderResourceView** ppSRV );

/** Fills vectors of random raindrop data, split into mutable and immutable parts */
void D3D11Effect::FillRandomRaindropData( std::vector<RainParticleDynamic>& dynamicData, std::vector<RainParticleStatic>& staticData ) {
    /** Base taken from Nvidias Rain-Sample **/

    const auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    const float radius = SanitizeRainRange( settings.RainRadiusRange, 5000.0f );
    const float height = SanitizeRainRange( settings.RainHeightRange, 1000.0f );

    for ( size_t i = 0; i < dynamicData.size(); i++ ) {
        // Vogel-disc distribution avoids conspicuous pairs while retaining a
        // subtle angular jitter so the rain does not form a visible pattern.
        const float normalizedRadius = sqrt( (static_cast<float>(i) + 0.5f)
            / static_cast<float>(dynamicData.size()) );
        const float angle = static_cast<float>(i) * 2.39996323f
            + (Toolbox::frand() - 0.5f) * 0.12f;
        const float SeedX = cos( angle ) * normalizedRadius * radius * 0.5f;
        const float SeedZ = sin( angle ) * normalizedRadius * radius * 0.5f;
        float SeedY = Toolbox::frand() * height;

        //add some random speed to the particles, to prevent all the particles from following exactly the same trajectory
        //additionally, random speeds in the vertical direction ensure that temporal aliasing is minimized
        float SpeedX = 40.0f * (Toolbox::frand() / 20.0f);
        float SpeedZ = 40.0f * (Toolbox::frand() / 20.0f);
        float SpeedY = 40.0f * (Toolbox::frand() / 10.0f);

        // Mutable data
        RainParticleDynamic& dynamic = dynamicData[i];
        dynamic.position = float3( SeedX + Engine::GAPI->GetCameraPosition().x, SeedY + Engine::GAPI->GetCameraPosition().y, SeedZ + Engine::GAPI->GetCameraPosition().z );
        dynamic.velocity = XMFLOAT3( SpeedX, SpeedY, SpeedZ );

        // Immutable data
        RainParticleStatic& immutable = staticData[i];
        immutable.seed = float3( SeedX, SeedY, SeedZ );
        immutable.randomBrightness = Toolbox::frand();

        //get an integer between 1 and 8 inclusive to decide which of the 8 types of rain textures the particle will use
        short* s = reinterpret_cast<short*>(&immutable.drawMode);
        s[0] = static_cast<short>( floor( Toolbox::frand() * 8 + 1 ) );
        s[1] = static_cast<short>( floor( Toolbox::frand() * 0xFFFF ) ); // Just a random number
    }
}

XRESULT D3D11Effect::EnsureRainBuffers( UINT numParticles, bool useCompute ) {
    auto* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);
    if ( !engine || !Engine::GAPI || numParticles == 0 || numParticles > MAX_RAIN_PARTICLES ) {
        return XR_INVALID_ARG;
    }

    auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    settings.RainRadiusRange = SanitizeRainRange( settings.RainRadiusRange, 5000.0f );
    settings.RainHeightRange = SanitizeRainRange( settings.RainHeightRange, 1000.0f );
    settings.RainGlobalVelocity = SanitizeRainVelocity( settings.RainGlobalVelocity );

    const UINT alignedCount = useCompute ? ((numParticles + 127u) / 128u) * 128u : numParticles;
    const UINT previousAlignedCount = ((RainBufferParticleCount + 127u) / 128u) * 128u;
    const bool hasRequiredBuffers = RainBufferStatic && RainBufferDrawFrom
        && (useCompute || (RainBufferInitial && RainBufferStreamTo));
    const bool particleCapacityChanged = useCompute
        ? previousAlignedCount != alignedCount
        : RainBufferParticleCount != numParticles;

    if ( hasRequiredBuffers
        && RainBuffersUseCompute == useCompute
        && RainBufferRadius == settings.RainRadiusRange
        && RainBufferHeight == settings.RainHeightRange
        && !particleCapacityChanged ) {
        RainBufferParticleCount = numParticles;
        return XR_SUCCESS;
    }

    try {
        std::vector<RainParticleDynamic> dynamicParticles( alignedCount );
        std::vector<RainParticleStatic> staticParticles( alignedCount );
        FillRandomRaindropData( dynamicParticles, staticParticles );

        auto newStatic = std::make_unique<D3D11VertexBuffer>();
        auto newDrawFrom = std::make_unique<D3D11VertexBuffer>();
        std::unique_ptr<D3D11VertexBuffer> newInitial;
        std::unique_ptr<D3D11VertexBuffer> newStreamTo;

        const unsigned int staticBytes = static_cast<unsigned int>(staticParticles.size() * sizeof( RainParticleStatic ));
        const unsigned int dynamicBytes = static_cast<unsigned int>(dynamicParticles.size() * sizeof( RainParticleDynamic ));
        const auto staticUsage = useCompute ? D3D11VertexBuffer::U_DEFAULT : D3D11VertexBuffer::U_IMMUTABLE;

        if ( newStatic->Init( staticParticles.data(), staticBytes,
                D3D11VertexBuffer::B_SHADER_RESOURCE, staticUsage, D3D11VertexBuffer::CA_NONE,
                useCompute ? "D3D11Effect::DrawRain_CS::RainBufferStatic" : "D3D11Effect::DrawRain::RainBufferStatic",
                sizeof( RainParticleStatic ) ) != XR_SUCCESS ) {
            return XR_FAILED;
        }

        if ( useCompute ) {
            if ( newDrawFrom->Init( dynamicParticles.data(), dynamicBytes,
                    static_cast<D3D11VertexBuffer::EBindFlags>(D3D11VertexBuffer::B_VERTEXBUFFER | D3D11VertexBuffer::B_UNORDERED_ACCESS),
                    D3D11VertexBuffer::U_DEFAULT, D3D11VertexBuffer::CA_NONE,
                    "D3D11Effect::DrawRain_CS::RainBufferDrawFrom", sizeof( float ) ) != XR_SUCCESS ) {
                return XR_FAILED;
            }
        } else {
            newInitial = std::make_unique<D3D11VertexBuffer>();
            newStreamTo = std::make_unique<D3D11VertexBuffer>();

            if ( newInitial->Init( dynamicParticles.data(), dynamicBytes,
                    D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_DEFAULT,
                    D3D11VertexBuffer::CA_NONE, "D3D11Effect::DrawRain::RainBufferInitial" ) != XR_SUCCESS
                || newDrawFrom->Init( dynamicParticles.data(), dynamicBytes,
                    static_cast<D3D11VertexBuffer::EBindFlags>(D3D11VertexBuffer::B_VERTEXBUFFER | D3D11VertexBuffer::B_STREAM_OUT),
                    D3D11VertexBuffer::U_DEFAULT, D3D11VertexBuffer::CA_NONE,
                    "D3D11Effect::DrawRain::RainBufferDrawFrom" ) != XR_SUCCESS
                || newStreamTo->Init( dynamicParticles.data(), dynamicBytes,
                    static_cast<D3D11VertexBuffer::EBindFlags>(D3D11VertexBuffer::B_VERTEXBUFFER | D3D11VertexBuffer::B_STREAM_OUT),
                    D3D11VertexBuffer::U_DEFAULT, D3D11VertexBuffer::CA_NONE,
                    "D3D11Effect::DrawRain::RainBufferStreamTo" ) != XR_SUCCESS ) {
                return XR_FAILED;
            }
        }

        if ( LoadRainResources() != XR_SUCCESS ) {
            return XR_FAILED;
        }

        delete RainBufferStatic;
        delete RainBufferInitial;
        delete RainBufferDrawFrom;
        delete RainBufferStreamTo;
        RainBufferStatic = newStatic.release();
        RainBufferInitial = newInitial.release();
        RainBufferDrawFrom = newDrawFrom.release();
        RainBufferStreamTo = newStreamTo.release();
        RainBufferRadius = settings.RainRadiusRange;
        RainBufferHeight = settings.RainHeightRange;
        RainBufferParticleCount = numParticles;
        RainBuffersUseCompute = useCompute;
        RainStreamOutFirstFrame = true;
        return XR_SUCCESS;
    } catch ( const std::bad_alloc& ) {
        LogError() << "Rain: Not enough memory to allocate " << alignedCount << " particles.";
        return XR_FAILED;
    }
}

/** Draws GPU-Based rain */
XRESULT D3D11Effect::DrawRain( bool outputResolution, bool useRainExclusionMask ) {
    auto* e = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);
    if ( !e || !Engine::GAPI || !e->GetContext() ) {
        return XR_FAILED;
    }
    GothicRendererState& state = Engine::GAPI->GetRendererState();

    // Get shaders
    auto streamOutGS = e->GetShaderManager().GetGShader( GShaderID::GS_ParticleStreamOut );
    auto particleAdvanceVS = e->GetShaderManager().GetVShader( VShaderID::VS_AdvanceRain );
    auto particleVS = e->GetShaderManager().GetVShader( VShaderID::VS_ParticlePointShaded );
    
    bool isSnow = oCGame::GetGame()
        && oCGame::GetGame()->_zCSession_world
        && oCGame::GetGame()->_zCSession_world->GetSkyControllerOutdoor()
        && oCGame::GetGame()->_zCSession_world->GetSkyControllerOutdoor()->GetWeatherType() == zTWEATHER_SNOW;

    auto rainPS = e->GetShaderManager().GetPShader( isSnow ? PShaderID::PS_Rain_Snow : PShaderID::PS_Rain );

    // artificially increase the number of particles for snow, to make it look better.
    // Snowflakes are bigger and slower than raindrops, so we can get away with less particles for rain, but for snow we need more to make it look good.
    UINT numParticles = SanitizeRainParticleCount( state.RendererSettings.RainNumParticles );
    state.RendererSettings.RainNumParticles = numParticles;
    if ( numParticles == 0 ) {
        return XR_SUCCESS;
    }
    if ( !streamOutGS || !particleAdvanceVS || !particleVS || !rainPS || !Engine::GAPI->GetSky() ) {
        LogError() << "Rain: Required stream-out shaders or sky state are unavailable.";
        return XR_FAILED;
    }
    if ( EnsureRainBuffers( numParticles, false ) != XR_SUCCESS ) {
        LogError() << "Rain: Failed to create stream-out resources.";
        return XR_FAILED;
    }
    auto velocity = SanitizeRainVelocity( state.RendererSettings.RainGlobalVelocity );
    if ( isSnow ) {
        // make snow a lot slower
        velocity = XMFLOAT3( velocity.x * snowSpeedFactor, velocity.y * snowSpeedFactor, velocity.z * snowSpeedFactor );
    }

    // Update constantbuffer for the advance-VS
    AdvanceRainConstantBuffer acb = {};
    XMFLOAT3 LightPosition_XMFloat3;
    XMStoreFloat3( &LightPosition_XMFloat3, XMLoadFloat3( &Engine::GAPI->GetSky()->GetAtmosphereSettings().LightDirection ) * Engine::GAPI->GetSky()->GetAtmosphereSettings().OuterRadius + Engine::GAPI->GetCameraPositionXM() );
    acb.AR_LightPosition = LightPosition_XMFloat3;
    acb.AR_FPS = state.RendererInfo.FPS;
    acb.AR_Radius = state.RendererSettings.RainRadiusRange;
    acb.AR_Height = state.RendererSettings.RainHeightRange;
    acb.AR_CameraPosition = Engine::GAPI->GetCameraPosition();
    acb.AR_GlobalVelocity = velocity;
    acb.AR_MoveRainParticles = state.RendererSettings.RainMoveParticles ? 1 : 0;
    acb.AR_Pad1.x = useRainExclusionMask ? 1.0f : 0.0f;
    acb.AR_Pad1.y = !outputResolution
        && state.RendererSettings.AntiAliasingMode == GothicRendererSettings::AA_FSR3
        && state.RendererSettings.Upscaler == GothicRendererSettings::UPSCALER_FSR_3 ? 1.0f : 0.0f;
    const INT2 backbufferResolution = e->GetBackbufferResolution();
    acb.AR_Pad1.z = outputResolution && backbufferResolution.x > 0
        ? static_cast<float>(e->GetResolution().x) / static_cast<float>(backbufferResolution.x)
        : 0.0f;
    auto advRainBuf = particleAdvanceVS->GetBuffer( "AdvanceRainConstantBuffer" );
    if ( !advRainBuf.Update( &acb ).Bind().Succeeded()
        || !advRainBuf.GetRawBuffer()
        || !advRainBuf.GetRawBuffer()->IsValid() ) {
        LogError() << "Rain: Failed to update the advance constant buffer.";
        return XR_FAILED;
    }
    advRainBuf.GetRawBuffer()->BindToPixelShader( 1 );

    if ( RainStreamOutFirstFrame || (state.RendererSettings.RainMoveParticles && !Engine::GAPI->IsGamePaused()) ) {
        D3D11VertexBuffer* sourceBuffer = RainStreamOutFirstFrame ? RainBufferInitial : RainBufferDrawFrom;
        if ( !sourceBuffer
            || !sourceBuffer->GetVertexBuffer()
            || !RainBufferStatic
            || !RainBufferStatic->GetShaderResourceView()
            || !RainBufferStreamTo
            || !RainBufferStreamTo->GetVertexBuffer() ) {
            LogError() << "Rain: Stream-out buffers became unavailable.";
            return XR_FAILED;
        }
        if ( particleAdvanceVS->Apply() != XR_SUCCESS || streamOutGS->Apply() != XR_SUCCESS ) {
            LogError() << "Rain: Failed to apply stream-out shaders.";
            return XR_FAILED;
        }

        UINT stride = sizeof( RainParticleDynamic );
        UINT offset = 0;

        // No fallible setup follows SOSetTargets, so every bound target is always released below.
        e->GetContext()->IASetVertexBuffers( 0, 1, sourceBuffer->GetVertexBuffer().GetAddressOf(), &stride, &offset );
        e->GetContext()->VSSetShaderResources( 1, 1, RainBufferStatic->GetShaderResourceView().GetAddressOf() );
        e->GetContext()->SOSetTargets( 1, RainBufferStreamTo->GetVertexBuffer().GetAddressOf(), &offset );
        e->GetContext()->PSSetShader( nullptr, nullptr, 0 );
        e->GetContext()->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_POINTLIST );
        e->SetDefaultStates();
        e->UpdateRenderStates();
        e->GetContext()->DrawInstanced( 1, numParticles, 0, 0 );

        ID3D11Buffer* noStreamTarget = nullptr;
        e->GetContext()->SOSetTargets( 1, &noStreamTarget, nullptr );
        std::swap( RainBufferDrawFrom, RainBufferStreamTo );
        RainStreamOutFirstFrame = false;
    }

    // Prepare all fallible draw resources before modifying the tracked render state.
    ParticleGSInfoConstantBuffer gcb = {};
    gcb.CameraPosition = Engine::GAPI->GetCameraPosition();
    gcb.PGS_RainFxWeight = Engine::GAPI->GetRainFXWeight();
    gcb.PGS_RainHeight = state.RendererSettings.RainHeightRange;
    gcb.PGS_RainScale = isSnow ? snowScale : rainScale;
    if ( !isSnow && acb.AR_Pad1.y > 0.5f ) {
        gcb.PGS_RainScale.x *= 1.6f;
        gcb.PGS_RainScale.y *= 1.1f;
    }

    ParticlePointShadingConstantBuffer scb = {};
    scb.View = GetRainShadowmapCameraRepl().ViewReplacement;
    scb.Projection = GetRainShadowmapCameraRepl().ProjectionReplacement;

    auto particleInfoBuffer = particleVS->GetBuffer( "ParticleGSInfo" );
    auto particleShadingBuffer = particleVS->GetBuffer( "ParticlePointShadingConstantBuffer" );
    if ( !RainShadowmap
        || !RainShadowmap->IsValid()
        || !RainBufferDrawFrom
        || !RainBufferDrawFrom->GetVertexBuffer()
        || !RainBufferStatic
        || !RainBufferStatic->GetShaderResourceView()
        || !m_RainDropShadowSamplerState
        || !(isSnow ? SnowTextureArraySRV.Get() : RainTextureArraySRV.Get())
        || !particleInfoBuffer.Update( &gcb ).Bind().Succeeded()
        || !particleShadingBuffer.Update( &scb ).Bind().Succeeded()
        || particleVS->Apply() != XR_SUCCESS
        || rainPS->Apply() != XR_SUCCESS ) {
        LogError() << "Rain: Failed to prepare particle draw resources.";
        return XR_FAILED;
    }

    // ---- Draw the rain ----
    state.BlendState.SetAlphaBlending();
    state.BlendState.SetDirty();

    // Disable depth-write
    state.DepthState.DepthWriteEnabled = false;
    state.DepthState.SetDirty();

    // Disable culling
    state.RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_NONE;
    state.RasterizerState.SetDirty();

    // Rendering instances only
    e->GetContext()->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP );
    e->UpdateRenderStates();

    Microsoft::WRL::ComPtr<ID3D11BlendState> previousBlendState;
    FLOAT previousBlendFactor[4] = {};
    UINT previousSampleMask = 0xffffffff;
    e->GetContext()->OMGetBlendState( previousBlendState.GetAddressOf(), previousBlendFactor, &previousSampleMask );
    if ( ID3D11BlendState* rainBlendState = GetRainReactiveBlendState() ) {
        const FLOAT blendFactor[4] = {};
        e->GetContext()->OMSetBlendState( rainBlendState, blendFactor, 0xffffffff );
    }

    e->GetContext()->GSSetShader( nullptr, 0, 0 );

    RainShadowmap->BindToVertexShader( e->GetContext().Get(), 0 );

    // Bind immutable particle data as StructuredBuffer SRV
    e->GetContext()->VSSetShaderResources( 1, 1, RainBufferStatic->GetShaderResourceView().GetAddressOf() );

    // Bind the shadow comparison sampler to the vertex shader at slot 2 (SS_Comp in shader)
    e->GetContext()->VSSetSamplers( 2, 1, m_RainDropShadowSamplerState.GetAddressOf() );

    // Bind view/proj
    e->SetupVS_ExConstantBuffer();

    // Bind droplets
    e->GetContext()->PSSetShaderResources( 0, 1, isSnow
        ? SnowTextureArraySRV.GetAddressOf()
        : RainTextureArraySRV.GetAddressOf() );

    // Draw the vertexbuffer
    {
        UINT stride = sizeof( RainParticleDynamic );
        UINT offset = 0;
        e->GetContext()->IASetVertexBuffers( 0, 1, RainBufferDrawFrom->GetVertexBuffer().GetAddressOf(), &stride, &offset );
        e->GetContext()->DrawInstanced( 4, numParticles, 0, 0 );
    }

    // Reset this
    e->GetContext()->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
    e->GetContext()->GSSetShader( nullptr, 0, 0 );
    e->GetContext()->OMSetBlendState( previousBlendState.Get(), previousBlendFactor, previousSampleMask );
    return XR_SUCCESS;
}

XRESULT D3D11Effect::DrawRain_CS( bool outputResolution, bool useRainExclusionMask ) {
    auto* e = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);
    if ( !e || !Engine::GAPI || !e->GetContext() ) {
        return XR_FAILED;
    }
    GothicRendererState& state = Engine::GAPI->GetRendererState();

    // Get shaders
    auto advanceRainCS = e->GetShaderManager().GetCShader( CShaderID::CS_AdvanceRain );
    auto particleVS = e->GetShaderManager().GetVShader( VShaderID::VS_ParticlePointShaded );

    bool isSnow = oCGame::GetGame()
        && oCGame::GetGame()->_zCSession_world
        && oCGame::GetGame()->_zCSession_world->GetSkyControllerOutdoor()
        && oCGame::GetGame()->_zCSession_world->GetSkyControllerOutdoor()->GetWeatherType() == zTWEATHER_SNOW;
    
    auto rainPS = e->GetShaderManager().GetPShader( isSnow ? PShaderID::PS_Rain_Snow : PShaderID::PS_Rain );

    // artificially increase the number of particles for snow, to make it look better.
    // Snowflakes are bigger and slower than raindrops, so we can get away with less particles for rain, but for snow we need more to make it look good.
    UINT numParticles = SanitizeRainParticleCount( state.RendererSettings.RainNumParticles );
    state.RendererSettings.RainNumParticles = numParticles;
    if ( numParticles == 0 ) {
        return XR_SUCCESS;
    }
    if ( !advanceRainCS || !particleVS || !rainPS || !Engine::GAPI->GetSky() ) {
        LogError() << "Rain: Required compute shaders or sky state are unavailable.";
        return XR_FAILED;
    }
    if ( EnsureRainBuffers( numParticles, true ) != XR_SUCCESS ) {
        LogError() << "Rain: Failed to create compute resources.";
        return XR_FAILED;
    }
    auto velocity = SanitizeRainVelocity( state.RendererSettings.RainGlobalVelocity );
    if ( isSnow ) {
        // make snow a lot slower
        velocity = XMFLOAT3(velocity.x * snowSpeedFactor, velocity.y * snowSpeedFactor, velocity.z * snowSpeedFactor );
    }

    // Update constantbuffer for the advance-CS
    AdvanceRainConstantBuffer acb = {};
    XMFLOAT3 LightPosition_XMFloat3;
    XMStoreFloat3( &LightPosition_XMFloat3, XMLoadFloat3( &Engine::GAPI->GetSky()->GetAtmosphereSettings().LightDirection ) * Engine::GAPI->GetSky()->GetAtmosphereSettings().OuterRadius + Engine::GAPI->GetCameraPositionXM() );
    acb.AR_LightPosition = LightPosition_XMFloat3;
    acb.AR_FPS = state.RendererInfo.FPS;
    acb.AR_Radius = state.RendererSettings.RainRadiusRange;
    acb.AR_Height = state.RendererSettings.RainHeightRange;
    acb.AR_CameraPosition = Engine::GAPI->GetCameraPosition();
    acb.AR_GlobalVelocity = velocity;
    acb.AR_MoveRainParticles = state.RendererSettings.RainMoveParticles ? 1 : 0;
    acb.AR_Pad1.x = useRainExclusionMask ? 1.0f : 0.0f;
    acb.AR_Pad1.y = !outputResolution
        && state.RendererSettings.AntiAliasingMode == GothicRendererSettings::AA_FSR3
        && state.RendererSettings.Upscaler == GothicRendererSettings::UPSCALER_FSR_3 ? 1.0f : 0.0f;
    const INT2 backbufferResolution = e->GetBackbufferResolution();
    acb.AR_Pad1.z = outputResolution && backbufferResolution.x > 0
        ? static_cast<float>(e->GetResolution().x) / static_cast<float>(backbufferResolution.x)
        : 0.0f;

    auto advanceBuffer = advanceRainCS->GetBuffer( "AdvanceRainConstantBuffer" );
    if ( !advanceBuffer.Update( &acb ).Succeeded()
        || !advanceBuffer.GetRawBuffer()
        || !advanceBuffer.GetRawBuffer()->IsValid() ) {
        LogError() << "Rain: Failed to update the compute advance constant buffer.";
        return XR_FAILED;
    }
    advanceBuffer.GetRawBuffer()->BindToPixelShader( 1 );

    if ( state.RendererSettings.RainMoveParticles && !Engine::GAPI->IsGamePaused() ) {
        if ( !RainBufferStatic
            || !RainBufferStatic->GetShaderResourceView()
            || !RainBufferDrawFrom
            || !RainBufferDrawFrom->GetUnorderedAccessView()
            || !advanceBuffer.Bind().Succeeded()
            || advanceRainCS->Apply() != XR_SUCCESS ) {
            LogError() << "Rain: Failed to prepare the compute advance pass.";
            return XR_FAILED;
        }

        e->GetContext()->CSSetShaderResources( 0, 1, RainBufferStatic->GetShaderResourceView().GetAddressOf() );
        e->GetContext()->CSSetUnorderedAccessViews( 0, 1, RainBufferDrawFrom->GetUnorderedAccessView().GetAddressOf(), nullptr );
        e->GetContext()->Dispatch( (numParticles + 127) / 128, 1, 1 );

        ID3D11Buffer* noBuffer = nullptr;
        ID3D11UnorderedAccessView* noUAV = nullptr;
        ID3D11ShaderResourceView* noSRV = nullptr;
        e->GetContext()->CSSetConstantBuffers( 0, 1, &noBuffer );
        e->GetContext()->CSSetShaderResources( 0, 1, &noSRV );
        e->GetContext()->CSSetUnorderedAccessViews( 0, 1, &noUAV, nullptr );
        e->GetContext()->CSSetShader( nullptr, nullptr, 0 );
    }

    // Prepare all fallible draw resources before modifying the tracked render state.
    ParticleGSInfoConstantBuffer gcb = {};
    gcb.CameraPosition = Engine::GAPI->GetCameraPosition();
    gcb.PGS_RainFxWeight = Engine::GAPI->GetRainFXWeight();
    gcb.PGS_RainHeight = state.RendererSettings.RainHeightRange;
    gcb.PGS_RainScale = isSnow ? snowScale : rainScale;
    if ( !isSnow && acb.AR_Pad1.y > 0.5f ) {
        gcb.PGS_RainScale.x *= 1.6f;
        gcb.PGS_RainScale.y *= 1.1f;
    }

    ParticlePointShadingConstantBuffer scb = {};
    scb.View = GetRainShadowmapCameraRepl().ViewReplacement;
    scb.Projection = GetRainShadowmapCameraRepl().ProjectionReplacement;

    auto particleInfoBuffer = particleVS->GetBuffer( "ParticleGSInfo" );
    auto particleShadingBuffer = particleVS->GetBuffer( "ParticlePointShadingConstantBuffer" );
    if ( !RainShadowmap
        || !RainShadowmap->IsValid()
        || !RainBufferDrawFrom
        || !RainBufferDrawFrom->GetVertexBuffer()
        || !RainBufferStatic
        || !RainBufferStatic->GetShaderResourceView()
        || !m_RainDropShadowSamplerState
        || !(isSnow ? SnowTextureArraySRV.Get() : RainTextureArraySRV.Get())
        || !particleInfoBuffer.Update( &gcb ).Bind().Succeeded()
        || !particleShadingBuffer.Update( &scb ).Bind().Succeeded()
        || particleVS->Apply() != XR_SUCCESS
        || rainPS->Apply() != XR_SUCCESS ) {
        LogError() << "Rain: Failed to prepare compute-particle draw resources.";
        return XR_FAILED;
    }

    // ---- Draw the rain ----
    state.BlendState.SetAlphaBlending();
    state.BlendState.SetDirty();

    // Disable depth-write
    state.DepthState.DepthWriteEnabled = false;
    state.DepthState.SetDirty();

    // Disable culling
    state.RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_NONE;
    state.RasterizerState.SetDirty();

    // Rendering instances only
    e->GetContext()->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP );
    e->UpdateRenderStates();

    Microsoft::WRL::ComPtr<ID3D11BlendState> previousBlendState;
    FLOAT previousBlendFactor[4] = {};
    UINT previousSampleMask = 0xffffffff;
    e->GetContext()->OMGetBlendState( previousBlendState.GetAddressOf(), previousBlendFactor, &previousSampleMask );
    if ( ID3D11BlendState* rainBlendState = GetRainReactiveBlendState() ) {
        const FLOAT blendFactor[4] = {};
        e->GetContext()->OMSetBlendState( rainBlendState, blendFactor, 0xffffffff );
    }

    RainShadowmap->BindToVertexShader( e->GetContext().Get(), 0 );

    // Bind immutable particle data as StructuredBuffer SRV
    e->GetContext()->VSSetShaderResources( 1, 1, RainBufferStatic->GetShaderResourceView().GetAddressOf() );

    // Bind the shadow comparison sampler to the vertex shader at slot 2 (SS_Comp in shader)
    e->GetContext()->VSSetSamplers( 2, 1, m_RainDropShadowSamplerState.GetAddressOf() );

    // Bind view/proj
    e->SetupVS_ExConstantBuffer();

    // Bind droplets
    e->GetContext()->PSSetShaderResources( 0, 1, isSnow
        ? SnowTextureArraySRV.GetAddressOf()
        : RainTextureArraySRV.GetAddressOf() );

    // Draw the vertexbuffer
    {
        UINT stride = sizeof( RainParticleDynamic );
        UINT offset = 0;
        e->GetContext()->IASetVertexBuffers( 0, 1, RainBufferDrawFrom->GetVertexBuffer().GetAddressOf(), &stride, &offset );
        e->GetContext()->DrawInstanced( 4, numParticles, 0, 0 );
    }

    // Reset primitive topology
    e->GetContext()->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
    e->GetContext()->OMSetBlendState( previousBlendState.Get(), previousBlendFactor, previousSampleMask );
    return XR_SUCCESS;
}

XRESULT D3D11Effect::LoadRainResources() {
    auto* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);
    if ( !engine || !engine->GetDevice() || !engine->GetContext() ) {
        return XR_FAILED;
    }

    try {
        std::filesystem::path basePath;
        std::array<char, MAX_PATH> modulePath{};
        const DWORD pathLength = GetModuleFileNameA( nullptr, modulePath.data(), static_cast<DWORD>(modulePath.size()) );
        if ( pathLength > 0 && pathLength < modulePath.size() ) {
            basePath = std::filesystem::path( std::string( modulePath.data(), pathLength ) ).parent_path()
                / "GD3D11" / "Textures";
        }

        auto loadTextureSet = [&]( const char* label, const char* vdfsPrefix,
                                   const std::filesystem::path& diskPrefix, int textureCount,
                                   Microsoft::WRL::ComPtr<ID3D11Texture2D>& texture,
                                   Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& srv ) -> bool {
            if ( texture && srv ) {
                return true;
            }

            Microsoft::WRL::ComPtr<ID3D11Texture2D> newTexture;
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> newSrv;
            HRESULT hr = LoadTextureArray( engine->GetDevice(), engine->GetContext(), vdfsPrefix,
                textureCount, newTexture.GetAddressOf(), newSrv.GetAddressOf() );
            if ( FAILED( hr ) && !diskPrefix.empty() ) {
                newTexture.Reset();
                newSrv.Reset();
                const std::string diskPath = diskPrefix.string();
                hr = LoadTextureArray( engine->GetDevice(), engine->GetContext(), diskPath.c_str(),
                    textureCount, newTexture.GetAddressOf(), newSrv.GetAddressOf() );
            }
            if ( FAILED( hr ) || !newTexture || !newSrv ) {
                LogError() << "Rain: Failed to load " << label << " texture array (0x"
                    << std::hex << static_cast<unsigned long>(hr) << ").";
                return false;
            }

            texture = std::move( newTexture );
            srv = std::move( newSrv );
            return true;
        };

        if ( !loadTextureSet( "raindrop", "\\_work\\Data\\Textures\\GD3D11\\Raindrops\\cv0_vPositive_",
                basePath.empty() ? std::filesystem::path{} : basePath / "Raindrops" / "cv0_vPositive_",
                370, RainTextureArray, RainTextureArraySRV )
            || !loadTextureSet( "snowflake", "\\_work\\Data\\Textures\\GD3D11\\Snowflakes\\Snow_",
                basePath.empty() ? std::filesystem::path{} : basePath / "Snowflakes" / "Snow_",
                256, SnowTextureArray, SnowTextureArraySRV ) ) {
            return XR_FAILED;
        }

        if ( !RainShadowmap || !RainShadowmap->IsValid() ) {
            auto newShadowmap = std::make_unique<RenderToDepthStencilBuffer>( engine->GetDevice().Get(), 2048, 2048,
                DXGI_FORMAT_R16_TYPELESS, nullptr, DXGI_FORMAT_D16_UNORM, DXGI_FORMAT_R16_UNORM );
            if ( !newShadowmap->IsValid() ) {
                LogError() << "Rain: Failed to create the rain shadow map.";
                return XR_FAILED;
            }
            SetDebugName( newShadowmap->GetDepthStencilView().Get(), "RainShadowmap->DepthStencilView" );
            SetDebugName( newShadowmap->GetShaderResView().Get(), "RainShadowmap->ShaderResView" );
            SetDebugName( newShadowmap->GetTexture().Get(), "RainShadowmap->Texture" );
            RainShadowmap = std::move( newShadowmap );
        }

        if ( !m_RainDropShadowSamplerState ) {
            D3D11_SAMPLER_DESC samplerDesc{};
            samplerDesc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
            samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
            samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
            samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
            samplerDesc.MaxAnisotropy = 1;
            samplerDesc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
            samplerDesc.MinLOD = -FLT_MAX;
            samplerDesc.MaxLOD = FLT_MAX;

            Microsoft::WRL::ComPtr<ID3D11SamplerState> newSampler;
            const HRESULT hr = engine->GetDevice()->CreateSamplerState( &samplerDesc, newSampler.GetAddressOf() );
            if ( FAILED( hr ) ) {
                LogError() << "Rain: Failed to create shadow sampler (0x"
                    << std::hex << static_cast<unsigned long>(hr) << ").";
                return XR_FAILED;
            }
            SetDebugName( newSampler.Get(), "RainDropSamplerState" );
            m_RainDropShadowSamplerState = std::move( newSampler );
        }

        return XR_SUCCESS;
    } catch ( const std::bad_alloc& ) {
        LogError() << "Rain: Not enough memory while loading effect resources.";
        return XR_FAILED;
    } catch ( const std::filesystem::filesystem_error& error ) {
        LogError() << "Rain: Failed to resolve texture paths: " << error.what();
        return XR_FAILED;
    }
}

/** Renders the rain-shadowmap */
XRESULT D3D11Effect::DrawRainShadowmap() {
    D3D11GraphicsEngine* e = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine); // TODO: This has to be a cast to D3D11GraphicsEngineBase!
    //D3D11GraphicsEngineBase* e = (D3D11GraphicsEngineBase*)Engine::GraphicsEngine; //RenderShadowmaps to be moved then to D3D11GraphicsEngineBase

    if ( !e || !Engine::GAPI || !e->GetDevice() || !e->GetContext() ) {
        return XR_FAILED;
    }

    if ( !RainShadowmap || !RainShadowmap->IsValid() ) {
        const int s = 2048;
        auto newShadowmap = std::make_unique<RenderToDepthStencilBuffer>( e->GetDevice().Get(), s, s,
            DXGI_FORMAT_R16_TYPELESS, nullptr, DXGI_FORMAT_D16_UNORM, DXGI_FORMAT_R16_UNORM );
        if ( !newShadowmap->IsValid() ) {
            LogError() << "Rain: Failed to create the rain shadow map.";
            return XR_FAILED;
        }
        SetDebugName( newShadowmap->GetDepthStencilView().Get(), "RainShadowmap->DepthStencilView" );
        SetDebugName( newShadowmap->GetShaderResView().Get(), "RainShadowmap->ShaderResView" );
        SetDebugName( newShadowmap->GetTexture().Get(), "RainShadowmap->Texture" );
        RainShadowmap = std::move( newShadowmap );
    }

    CameraReplacement& cr = RainShadowmapCameraRepl;

    // Get the section we are currently in
    XMVECTOR p = Engine::GAPI->GetCameraPositionXM();
    const XMFLOAT3 safeVelocity = SanitizeRainVelocity(
        Engine::GAPI->GetRendererState().RendererSettings.RainGlobalVelocity );
    XMVECTOR rainVelocity = XMLoadFloat3( &safeVelocity );
    const float velocityLengthSq = XMVectorGetX( XMVector3LengthSq( rainVelocity ) );
    if ( !std::isfinite( velocityLengthSq ) || velocityLengthSq < 0.0001f ) {
        rainVelocity = XMVectorSet( 0, -1, 0, 0 );
    }
    XMVECTOR dir = XMVector3Normalize( rainVelocity * -1.0f );

    // Set the camera height to the highest point in this section
    p += dir * 6000.0f;

    XMVECTOR lookAt = p - dir;

    XMVECTOR forward = XMVector3Normalize( lookAt - p );
    XMVECTOR up = XMVectorSet( 0, 1, 0, 0 );
    if ( fabsf( XMVectorGetX( XMVector3Dot( forward, up ) ) ) > 0.95f ) {
        up = XMVectorSet( 0, 0, 1, 0 );
    }

    // Create shadowmap view-matrix
    XMMATRIX crViewReplacement = XMMatrixLookAtLH( p, lookAt, up );

    const auto size = RainShadowmap->GetSizeX();
    const auto legacySingleShadowMapScaleFactor = Toolbox::GetRecommendedWorldShadowRangeScaleForSize( size );

    XMMATRIX crProjectionReplacement = XMMatrixOrthographicLH(
        size * legacySingleShadowMapScaleFactor,
        size * legacySingleShadowMapScaleFactor,
        1,
        20000.0f
    );

    XMStoreFloat4x4( &cr.ViewReplacement, XMMatrixTranspose( crViewReplacement ) );
    XMStoreFloat4x4( &cr.ProjectionReplacement, XMMatrixTranspose( crProjectionReplacement ) );
    XMStoreFloat3( &cr.PositionReplacement, p );
    XMStoreFloat3( &cr.LookAtReplacement, lookAt );
    
    cr.frustum.BuildOrthographic( crViewReplacement,
        size * legacySingleShadowMapScaleFactor,
        size * legacySingleShadowMapScaleFactor,
        1.0f,
        20000.f );

    if ( !cr.frustum.IsValid() ) {
        // Keep rain occlusion alive even if a custom rain direction produced a degenerate camera basis.
        XMVECTOR fallbackUp = XMVectorSet( 1, 0, 0, 0 );
        crViewReplacement = XMMatrixLookAtLH( p, lookAt, fallbackUp );
        XMStoreFloat4x4( &cr.ViewReplacement, XMMatrixTranspose( crViewReplacement ) );
        cr.frustum.BuildOrthographic( crViewReplacement,
            size * legacySingleShadowMapScaleFactor,
            size * legacySingleShadowMapScaleFactor,
            1.0f,
            20000.f );
    }

    if ( !cr.frustum.IsValid() ) {
        return XR_SUCCESS;
    }

    // Replace gothics camera
    Engine::GAPI->SetCameraReplacementPtr( &cr );

    // Make alpharef a bit more aggressive, to make trees less rain-proof

    float oldAlphaRef = Engine::GAPI->GetRendererState().GraphicsState.FF_AlphaRef;

    Engine::GAPI->GetRendererState().GraphicsState.FF_AlphaRef = -1.0f;

    // Bind the FF-Info to the first PS slot
    auto PS_Diffuse = e->GetShaderManager().GetPShader( PShaderID::PS_Diffuse );
    if ( PS_Diffuse ) {
        PS_Diffuse->GetBuffer( "FFPipelineConstantBuffer" ).Update( &Engine::GAPI->GetRendererState().GraphicsState ).Bind();
    }

    // Disable stuff like NPCs and usable things as they don't need to cast rain-shadows
    bool oldDrawSkel = Engine::GAPI->GetRendererState().RendererSettings.DrawSkeletalMeshes;
    Engine::GAPI->GetRendererState().RendererSettings.DrawSkeletalMeshes = false;

    // Draw rain-shadowmap

    // Save old rendertargets
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> oldRTV;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> oldDSV;
    e->GetContext()->OMGetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.GetAddressOf() );

    e->RenderShadowmaps( p, RainShadowmap.get(), true, false );

    // Restore old settings
    e->GetContext()->OMSetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.Get() );
    Engine::GAPI->GetRendererState().RendererSettings.DrawSkeletalMeshes = oldDrawSkel;
    Engine::GAPI->GetRendererState().GraphicsState.FF_AlphaRef = oldAlphaRef;
    if ( PS_Diffuse ) {
        PS_Diffuse->GetBuffer( "FFPipelineConstantBuffer" ).Update( &Engine::GAPI->GetRendererState().GraphicsState );
    }

    e->SetDefaultStates();

    // Restore gothics camera
    Engine::GAPI->SetCameraReplacementPtr( nullptr );

    return XR_SUCCESS;
}

//--------------------------------------------------------------------------------------
// LoadTextureArray loads a texture array and associated view from a series
// of textures on disk.
//--------------------------------------------------------------------------------------
HRESULT LoadTextureArray( Microsoft::WRL::ComPtr<ID3D11Device1> pd3dDevice,
                          Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context,
                          const char* sTexturePrefix, int iNumTextures,
                          ID3D11Texture2D** ppTex2D, ID3D11ShaderResourceView** ppSRV ) {
    constexpr long MAX_PACKED_TEXTURE_BYTES = 512L * 1024L * 1024L;
    constexpr long MAX_SLICE_TEXTURE_BYTES = 256L * 1024L * 1024L;

    if ( !pd3dDevice || !context || !sTexturePrefix || !sTexturePrefix[0]
        || iNumTextures <= 0 || iNumTextures > D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION
        || !ppTex2D || !ppSRV || *ppTex2D || *ppSRV ) {
        LogError() << "Rain: Invalid texture-array load request.";
        return E_INVALIDARG;
    }

    try {
        std::string singleFilePath;
        if ( strstr( sTexturePrefix, "Raindrops" ) ) {
            singleFilePath = R"(\system\GD3D11\Textures\raindrops.dds)";
        } else if ( strstr( sTexturePrefix, "Snowflakes" ) ) {
            singleFilePath = R"(\system\GD3D11\Textures\snowflakes.dds)";
        }

        if ( !singleFilePath.empty() ) {
            auto file = zFILE_VDFS::Create( singleFilePath.c_str() );
            if ( file && file->Exists() && file->Open( false ) == zERROR_NONE ) {
                const long fileSize = file->Size();
                if ( fileSize > 0 && fileSize <= MAX_PACKED_TEXTURE_BYTES ) {
                    std::vector<uint8_t> storage( static_cast<size_t>(fileSize) );
                    const long bytesRead = file->Read( storage.data(), fileSize );
                    file->Close();

                    if ( bytesRead == fileSize ) {
                        Microsoft::WRL::ComPtr<ID3D11Resource> resource;
                        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
                        HRESULT hr = CreateDDSTextureFromMemoryEx(
                            pd3dDevice.Get(), storage.data(), storage.size(), 0,
                            D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0,
                            DDS_LOADER_DEFAULT, resource.GetAddressOf(), srv.GetAddressOf() );
                        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
                        if ( SUCCEEDED( hr ) ) {
                            hr = resource.As( &texture );
                        }
                        if ( SUCCEEDED( hr ) && texture && srv ) {
                            D3D11_TEXTURE2D_DESC desc{};
                            texture->GetDesc( &desc );
                            if ( desc.ArraySize >= static_cast<UINT>(iNumTextures)
                                && desc.Width > 0 && desc.Height > 0 && desc.MipLevels > 0 ) {
                                *ppTex2D = texture.Detach();
                                *ppSRV = srv.Detach();
                                LogInfo() << "Loaded texture array from VDFS: " << singleFilePath;
                                return S_OK;
                            }
                            hr = E_FAIL;
                        }
                        LogWarn() << "Rain: Packed DDS is invalid (0x"
                            << std::hex << static_cast<unsigned long>(hr) << "): " << singleFilePath;
                    } else {
                        LogWarn() << "Rain: Short read for packed DDS: " << singleFilePath;
                    }
                } else {
                    file->Close();
                    LogWarn() << "Rain: Packed DDS has an invalid size: " << singleFilePath;
                }
            }
        }

        Microsoft::WRL::ComPtr<ID3D11Texture2D> textureArray;
        D3D11_TEXTURE2D_DESC referenceDesc{};
        DXGI_FORMAT textureFormat = DXGI_FORMAT_UNKNOWN;
        std::vector<uint8_t> storage;

        for ( int textureIndex = 0; textureIndex < iNumTextures; ++textureIndex ) {
            std::array<char, MAX_PATH> path{};
            const int pathLength = sprintf_s( path.data(), path.size(), "%s%.4d.dds", sTexturePrefix, textureIndex );
            if ( pathLength <= 0 || static_cast<size_t>(pathLength) >= path.size() ) {
                LogError() << "Rain: Texture path is too long.";
                return HRESULT_FROM_WIN32( ERROR_FILENAME_EXCED_RANGE );
            }

            auto file = zFILE_VDFS::Create( path.data() );
            if ( !file || !file->Exists() || file->Open( false ) != zERROR_NONE ) {
                LogError() << "Rain: Failed to open texture slice: " << path.data();
                return HRESULT_FROM_WIN32( ERROR_FILE_NOT_FOUND );
            }

            const long fileSize = file->Size();
            if ( fileSize <= 0 || fileSize > MAX_SLICE_TEXTURE_BYTES ) {
                file->Close();
                LogError() << "Rain: Invalid texture-slice size: " << path.data();
                return E_FAIL;
            }

            storage.resize( static_cast<size_t>(fileSize) );
            const long bytesRead = file->Read( storage.data(), fileSize );
            file->Close();
            if ( bytesRead != fileSize ) {
                LogError() << "Rain: Short read for texture slice: " << path.data();
                return E_FAIL;
            }

            Microsoft::WRL::ComPtr<ID3D11Resource> resource;
            HRESULT hr = CreateDDSTextureFromMemoryEx(
                pd3dDevice.Get(), storage.data(), storage.size(), 0,
                D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_WRITE, 0,
                DDS_LOADER_DEFAULT, resource.GetAddressOf(), nullptr );
            if ( FAILED( hr ) || !resource ) {
                LogError() << "Rain: Failed to decode texture slice " << path.data()
                    << " (0x" << std::hex << static_cast<unsigned long>(hr) << ").";
                return FAILED( hr ) ? hr : E_FAIL;
            }

            Microsoft::WRL::ComPtr<ID3D11Texture2D> sourceTexture;
            hr = resource.As( &sourceTexture );
            if ( FAILED( hr ) || !sourceTexture ) {
                return FAILED( hr ) ? hr : E_NOINTERFACE;
            }

            D3D11_TEXTURE2D_DESC desc{};
            sourceTexture->GetDesc( &desc );
            if ( desc.Width == 0 || desc.Height == 0 || desc.MipLevels == 0 || desc.ArraySize != 1
                || (desc.Format != DXGI_FORMAT_BC4_UNORM && desc.Format != DXGI_FORMAT_R8_UNORM) ) {
                LogError() << "Rain: Unsupported texture-slice layout: " << path.data();
                return E_FAIL;
            }

            if ( !textureArray ) {
                referenceDesc = desc;
                textureFormat = desc.Format;
                D3D11_TEXTURE2D_DESC arrayDesc = desc;
                arrayDesc.Usage = D3D11_USAGE_DEFAULT;
                arrayDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                arrayDesc.CPUAccessFlags = 0;
                arrayDesc.MiscFlags = 0;
                arrayDesc.ArraySize = static_cast<UINT>(iNumTextures);
                hr = pd3dDevice->CreateTexture2D( &arrayDesc, nullptr, textureArray.GetAddressOf() );
                if ( FAILED( hr ) ) {
                    LogError() << "Rain: Failed to create texture array (0x"
                        << std::hex << static_cast<unsigned long>(hr) << ").";
                    return hr;
                }
            } else if ( desc.Width != referenceDesc.Width || desc.Height != referenceDesc.Height
                || desc.MipLevels != referenceDesc.MipLevels || desc.Format != textureFormat
                || desc.SampleDesc.Count != referenceDesc.SampleDesc.Count
                || desc.SampleDesc.Quality != referenceDesc.SampleDesc.Quality ) {
                LogError() << "Rain: Texture slices have incompatible layouts.";
                return E_FAIL;
            }

            for ( UINT mip = 0; mip < desc.MipLevels; ++mip ) {
                context->CopySubresourceRegion(
                    textureArray.Get(), D3D11CalcSubresource( mip, textureIndex, desc.MipLevels ),
                    0, 0, 0, sourceTexture.Get(), mip, nullptr );
            }
        }

        if ( !textureArray ) {
            return E_FAIL;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = textureFormat;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        srvDesc.Texture2DArray.MostDetailedMip = 0;
        srvDesc.Texture2DArray.MipLevels = referenceDesc.MipLevels;
        srvDesc.Texture2DArray.FirstArraySlice = 0;
        srvDesc.Texture2DArray.ArraySize = static_cast<UINT>(iNumTextures);

        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        const HRESULT hr = pd3dDevice->CreateShaderResourceView( textureArray.Get(), &srvDesc, srv.GetAddressOf() );
        if ( FAILED( hr ) ) {
            LogError() << "Rain: Failed to create texture-array SRV (0x"
                << std::hex << static_cast<unsigned long>(hr) << ").";
            return hr;
        }

        *ppTex2D = textureArray.Detach();
        *ppSRV = srv.Detach();
        return S_OK;
    } catch ( const std::bad_alloc& ) {
        LogError() << "Rain: Not enough memory to load texture array.";
        return E_OUTOFMEMORY;
    }
}
