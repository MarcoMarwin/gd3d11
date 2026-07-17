#include "pch.h"

#include "D3D11PFX_FSR3.h"

#include "D3D11PfxRenderer.h"
#include "D3D11GraphicsEngine.h"
#include "Engine.h"
#include "RenderToTextureBuffer.h"
#include <FidelityFX/host/backends/dx11/ffx_dx11.h>
#include <algorithm>
#include <cstdlib>

#pragma comment(lib, "ffx_backend_dx11_x86.lib")
#pragma comment(lib, "ffx_fsr3upscaler_x86.lib")

namespace {
    FfxResource WrapResource(
        ID3D11Resource* resource,
        const wchar_t* name,
        FfxResourceStates state = FFX_RESOURCE_STATE_COMPUTE_READ )
    {
        if ( !resource ) {
            return {};
        }

        FfxResource ffxResource = {};
        ffxResource.resource = resource;
        ffxResource.description = GetFfxResourceDescriptionDX11( resource );
        ffxResource.state = state;
        if ( name ) {
            wcscpy_s( ffxResource.name, name );
        }
        return ffxResource;
    }

    ID3D11Resource* GetResourceFromView( ID3D11View* view ) {
        if ( !view ) {
            return nullptr;
        }
        ID3D11Resource* resource = nullptr;
        view->GetResource( &resource );
        if ( resource ) {
            resource->Release();
        }
        return resource;
    }

    FfxResource WrapView(
        ID3D11View* view,
        const wchar_t* name,
        FfxResourceStates state = FFX_RESOURCE_STATE_COMPUTE_READ )
    {
        return WrapResource( GetResourceFromView( view ), name, state );
    }

    void UnbindComputeResources( ID3D11DeviceContext* context ) {
        ID3D11ShaderResourceView* nullSrvs[16] = {};
        ID3D11UnorderedAccessView* nullUavs[8] = {};
        UINT initialCounts[8] = {};
        context->CSSetShaderResources( 0, _countof( nullSrvs ), nullSrvs );
        context->CSSetUnorderedAccessViews( 0, _countof( nullUavs ), nullUavs, initialCounts );
        context->CSSetShader( nullptr, nullptr, 0 );
    }

    void FfxLog( FfxMsgType type, const wchar_t* message ) {
        LogError() << "FidelityFX FSR3 Upscaler (" << type << "): " << message;
    }

    FfxErrorCode GuardedFsr3UpscalerContextCreate(
        FfxFsr3UpscalerContext* context,
        const FfxFsr3UpscalerContextDescription* description )
    {
        __try {
            return ffxFsr3UpscalerContextCreate( context, description );
        }
        __except ( EXCEPTION_EXECUTE_HANDLER ) {
            return FFX_ERROR_BACKEND_API_ERROR;
        }
    }
}

D3D11PFX_FSR3::D3D11PFX_FSR3( D3D11PfxRenderer* renderer )
    : Renderer( renderer )
    , Backend( {} )
    , Context( nullptr )
    , ScratchMemory( nullptr )
    , MaxInputSize( 0, 0 )
    , MaxOutputSize( 0, 0 )
    , Initialized( false )
{
}

D3D11PFX_FSR3::~D3D11PFX_FSR3() {
    Destroy();
}

bool D3D11PFX_FSR3::Init( const INT2& maxInputSize, const INT2& maxOutputSize ) {
    if ( Initialized
        && MaxInputSize == maxInputSize
        && MaxOutputSize == maxOutputSize ) {
        return true;
    }

    Destroy();

    auto* engine = static_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    ID3D11Device* device = engine->GetDevice().Get();
    if ( !device || device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0 ) {
        return false;
    }

    MaxInputSize = maxInputSize;
    MaxOutputSize = maxOutputSize;

    const int effectCount = FFX_FSR3UPSCALER_CONTEXT_COUNT;
    const size_t scratchSize = ffxGetScratchMemorySizeDX11( effectCount );
    ScratchMemory = calloc( 1, scratchSize );
    if ( !ScratchMemory ) {
        LogError() << "FSR3: Failed to allocate backend scratch memory.";
        Destroy();
        return false;
    }

    const FfxErrorCode interfaceResult = ffxGetInterfaceDX11(
        &Backend,
        device,
        ScratchMemory,
        scratchSize,
        effectCount );
    if ( interfaceResult != FFX_OK ) {
        LogError() << "FSR3: Failed to create DX11 backend interface.";
        Destroy();
        return false;
    }

    FfxFsr3UpscalerContextDescription desc = {};
    desc.flags = FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE
        | FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE
        | FFX_FSR3UPSCALER_ENABLE_DEPTH_INVERTED
        | FFX_FSR3UPSCALER_ENABLE_DEPTH_INFINITE
        | FFX_FSR3UPSCALER_ENABLE_DYNAMIC_RESOLUTION;
#ifdef DEBUG_D3D11
    desc.flags |= FFX_FSR3UPSCALER_ENABLE_DEBUG_CHECKING;
    desc.fpMessage = &FfxLog;
#endif

    desc.maxRenderSize = {
        static_cast<uint32_t>(maxInputSize.x),
        static_cast<uint32_t>(maxInputSize.y)
    };
    desc.maxUpscaleSize = {
        static_cast<uint32_t>(maxOutputSize.x),
        static_cast<uint32_t>(maxOutputSize.y)
    };
    desc.backendInterface = Backend;

    Context = new FfxFsr3UpscalerContext{};
    const FfxErrorCode createResult = GuardedFsr3UpscalerContextCreate( Context, &desc );
    if ( createResult != FFX_OK ) {
        LogError() << "FSR3: Failed to create upscaler context (" << createResult << ").";
        SAFE_DELETE( Context );
        Destroy();
        return false;
    }

    Initialized = true;
    return true;
}

void D3D11PFX_FSR3::Destroy() {
    if ( Context ) {
        try {
            ffxFsr3UpscalerContextDestroy( Context );
        } catch ( ... ) {
            LogError() << "FSR3: DX11 backend exception while destroying the upscaler context.";
        }
        SAFE_DELETE( Context );
    }

    if ( ScratchMemory ) {
        free( ScratchMemory );
        ScratchMemory = nullptr;
    }
    Backend = {};
    Initialized = false;
    MaxInputSize = INT2( 0, 0 );
    MaxOutputSize = INT2( 0, 0 );
}

XRESULT D3D11PFX_FSR3::Apply(
    ID3D11ShaderResourceView* color,
    ID3D11ShaderResourceView* depth,
    ID3D11ShaderResourceView* motionVectors,
    ID3D11ShaderResourceView* reactiveMask,
    ID3D11ShaderResourceView* transparencyAndCompositionMask,
    ID3D11RenderTargetView* output,
    const INT2& inputSize,
    const INT2& outputSize,
    float deltaTimeMs,
    const float2& jitterOffset,
    const float2& motionVectorScale,
    bool resetAccumulation,
    float cameraFovAngleVertical,
    float cameraNear,
    float cameraFar,
    bool enableSharpening,
    float sharpness )
{
    if ( !Init( inputSize, outputSize ) ) {
        LogError() << "FSR3: Failed to initialize.";
        return XR_FAILED;
    }

    auto* engine = static_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    ID3D11DeviceContext* context = engine->GetContext().Get();

    engine->SetDefaultStates();
    engine->UpdateRenderStates();

    ID3D11RenderTargetView* nullRtvs[8] = {};
    ID3D11ShaderResourceView* nullSrvs[16] = {};
    context->OMSetRenderTargets( _countof( nullRtvs ), nullRtvs, nullptr );
    context->VSSetShaderResources( 0, _countof( nullSrvs ), nullSrvs );
    context->PSSetShaderResources( 0, _countof( nullSrvs ), nullSrvs );
    context->CSSetShaderResources( 0, _countof( nullSrvs ), nullSrvs );

    FfxFsr3UpscalerSharedResourceDescriptions sharedResources = {};
    ffxFsr3UpscalerGetSharedResourceDescriptions( Context, &sharedResources );

    auto dilatedMotionVectors = Renderer->GetTexturePool()->Acquire( {
        static_cast<int>(sharedResources.dilatedMotionVectors.resourceDescription.width),
        static_cast<int>(sharedResources.dilatedMotionVectors.resourceDescription.height),
        DXGI_FORMAT_R16G16_FLOAT,
        D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE
    } );
    auto dilatedDepth = Renderer->GetTexturePool()->Acquire( {
        static_cast<int>(sharedResources.dilatedDepth.resourceDescription.width),
        static_cast<int>(sharedResources.dilatedDepth.resourceDescription.height),
        DXGI_FORMAT_R32_FLOAT,
        D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE
    } );
    auto reconstructedPrevNearestDepth = Renderer->GetTexturePool()->Acquire( {
        static_cast<int>(sharedResources.reconstructedPrevNearestDepth.resourceDescription.width),
        static_cast<int>(sharedResources.reconstructedPrevNearestDepth.resourceDescription.height),
        DXGI_FORMAT_R32_UINT,
        D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE
    } );

    FfxFsr3UpscalerDispatchDescription dispatch = {};
    dispatch.commandList = ffxGetCommandListDX11( context );
    dispatch.color = WrapView( color, L"FSR3 Input Color" );
    dispatch.depth = WrapView( depth, L"FSR3 Input Depth" );
    dispatch.motionVectors = WrapView( motionVectors, L"FSR3 Input Motion Vectors" );
    dispatch.output = WrapView( output, L"FSR3 Upscale Output", FFX_RESOURCE_STATE_UNORDERED_ACCESS );
    if ( reactiveMask ) {
        dispatch.reactive = WrapView( reactiveMask, L"FSR3 Reactive Mask" );
    }
    if ( transparencyAndCompositionMask ) {
        dispatch.transparencyAndComposition = WrapView( transparencyAndCompositionMask, L"FSR3 Transparency and Composition" );
    }
    dispatch.dilatedMotionVectors = WrapResource(
        dilatedMotionVectors->GetTexture().Get(),
        sharedResources.dilatedMotionVectors.name,
        FFX_RESOURCE_STATE_UNORDERED_ACCESS );
    dispatch.dilatedDepth = WrapResource(
        dilatedDepth->GetTexture().Get(),
        sharedResources.dilatedDepth.name,
        FFX_RESOURCE_STATE_UNORDERED_ACCESS );
    dispatch.reconstructedPrevNearestDepth = WrapResource(
        reconstructedPrevNearestDepth->GetTexture().Get(),
        sharedResources.reconstructedPrevNearestDepth.name,
        FFX_RESOURCE_STATE_UNORDERED_ACCESS );

    dispatch.renderSize = {
        static_cast<uint32_t>(inputSize.x),
        static_cast<uint32_t>(inputSize.y)
    };
    dispatch.upscaleSize = {
        static_cast<uint32_t>(outputSize.x),
        static_cast<uint32_t>(outputSize.y)
    };
    dispatch.jitterOffset = { jitterOffset.x, jitterOffset.y };
    dispatch.motionVectorScale = { motionVectorScale.x, motionVectorScale.y };
    dispatch.reset = resetAccumulation;
    dispatch.enableSharpening = enableSharpening;
    dispatch.sharpness = std::clamp( sharpness, 0.0f, 1.0f );
    dispatch.frameTimeDelta = std::max( deltaTimeMs, 1.0f );
    dispatch.preExposure = 1.0f;
    dispatch.viewSpaceToMetersFactor = 0.01f;
    dispatch.cameraFovAngleVertical = XMConvertToRadians( cameraFovAngleVertical );
    dispatch.cameraNear = cameraNear;
    dispatch.cameraFar = cameraFar;

    FfxErrorCode result = FFX_ERROR_BACKEND_API_ERROR;
    try {
        result = ffxFsr3UpscalerContextDispatch( Context, &dispatch );
    } catch ( ... ) {
        LogError() << "FSR3: DX11 backend exception during upscaling dispatch.";
    }

    UnbindComputeResources( context );
    if ( result != FFX_OK ) {
        LogError() << "FSR3: Upscaling dispatch failed (" << result << ").";
        return XR_FAILED;
    }

    return XR_SUCCESS;
}