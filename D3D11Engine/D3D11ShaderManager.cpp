#include "pch.h"
#include "D3D11ShaderManager.h"
#include "D3D11VShader.h"
#include "D3D11PShader.h"
#include "D3D11GShader.h"
#include "D3D11CShader.h"
#include "D3D11ConstantBuffer.h"
#include "GothicGraphicsState.h"
#include "ConstantBufferStructs.h"
#include "GothicAPI.h"
#include "Engine.h"
#include "ThreadPool.h"

#include "D3D11GraphicsEngineBase.h"
#include <d3dcompiler.h>
#include "D3D11FileRelativeInclude.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxguid.lib")

// Patch HLSL-Compiler for http://support.microsoft.com/kb/2448404
#if D3DX_VERSION == 0xa2b
#pragma ruledisable 0x0802405f
#endif

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <unordered_map>

namespace {
    std::wstring NormalizeShaderPath( const std::filesystem::path& path ) {
        std::wstring value = path.native();
        std::transform( value.begin(), value.end(), value.begin(),
            []( wchar_t c ) { return static_cast<wchar_t>(std::towlower( c )); } );
        std::replace( value.begin(), value.end(), L'/', L'\\' );
        return value;
    }

    bool IsShaderPathWithinRoot( const std::filesystem::path& root,
        const std::filesystem::path& candidate ) {
        std::wstring normalizedRoot = NormalizeShaderPath( root );
        const std::wstring normalizedCandidate = NormalizeShaderPath( candidate );
        if ( normalizedRoot.empty() || normalizedCandidate.empty() ) return false;
        if ( normalizedCandidate == normalizedRoot ) return true;
        if ( normalizedRoot.back() != L'\\' ) normalizedRoot.push_back( L'\\' );
        return normalizedCandidate.size() > normalizedRoot.size()
            && normalizedCandidate.compare( 0, normalizedRoot.size(), normalizedRoot ) == 0;
    }
}

const int NUM_MAX_BONES = 96;

extern bool FeatureLevel10Compatibility;
extern bool FeatureRTArrayIndexFromAnyShader;
#if !defined(BUILD_GOTHIC_2_6_fix) && !defined(BUILD_1_12F)
extern bool haveWindAnimations;
#endif

D3D11ShaderManager::D3D11ShaderManager()
    : VShaders( static_cast<size_t>(VShaderID::COUNT) )
    , PShaders( static_cast<size_t>(PShaderID::COUNT) )
    , GShaders( static_cast<size_t>(GShaderID::COUNT) )
    , CShaders( static_cast<size_t>(CShaderID::COUNT) )
    , ShaderCategoriesToReloadNextFrame( ShaderCategory::None )
{
}

D3D11ShaderManager::~D3D11ShaderManager() {
    DeleteShaders();
}

//--------------------------------------------------------------------------------------
// Find and compile the specified shader
//--------------------------------------------------------------------------------------
HRESULT D3D11ShaderManager::CompileShaderFromFile( const CHAR* szFileName,
    LPCSTR szEntryPoint, LPCSTR szShaderModel, ID3DBlob** ppBlobOut,
    const std::vector<D3D_SHADER_MACRO>& makros ) {
    if ( !szFileName || !*szFileName ) return E_INVALIDARG;

    try {
        const std::wstring shaderFile = Toolbox::ToWideChar( szFileName );
        if ( shaderFile.empty() ) return E_INVALIDARG;
        return CompileShaderFromFile( shaderFile.c_str(), szEntryPoint,
            szShaderModel, ppBlobOut, makros );
    } catch ( const std::bad_alloc& ) {
        return E_OUTOFMEMORY;
    } catch ( ... ) {
        return E_FAIL;
    }
}

HRESULT D3D11ShaderManager::CompileShaderFromFile( const WCHAR* szFileName,
    LPCSTR szEntryPoint, LPCSTR szShaderModel, ID3DBlob** ppBlobOut,
    const std::vector<D3D_SHADER_MACRO>& makros ) {
    if ( !szFileName || !*szFileName || !szEntryPoint || !*szEntryPoint
        || !szShaderModel || !*szShaderModel || !ppBlobOut || !Engine::GAPI ) {
        return E_INVALIDARG;
    }
    *ppBlobOut = nullptr;

    try {
        DWORD shaderFlags = D3DCOMPILE_ENABLE_STRICTNESS
            | D3DCOMPILE_OPTIMIZATION_LEVEL3;
#if defined(DEBUG_D3D11)
        shaderFlags |= D3DCOMPILE_DEBUG;
#endif

        std::vector<D3D_SHADER_MACRO> macros = makros;
        macros.push_back( { nullptr, nullptr } );

        std::error_code error;
        const std::filesystem::path startDirectory =
            std::filesystem::weakly_canonical( Engine::GAPI->GetStartDirectory(), error );
        if ( error || startDirectory.empty() ) {
            return HRESULT_FROM_WIN32( ERROR_PATH_NOT_FOUND );
        }

        error.clear();
        const std::filesystem::path shaderRoot = std::filesystem::weakly_canonical(
            startDirectory / L"system" / L"GD3D11" / L"Shaders", error );
        if ( error || shaderRoot.empty() ) {
            return HRESULT_FROM_WIN32( ERROR_PATH_NOT_FOUND );
        }

        const std::filesystem::path requestedPath( szFileName );
        error.clear();
        const std::filesystem::path shaderPath = std::filesystem::weakly_canonical(
            requestedPath.is_absolute() ? requestedPath : startDirectory / requestedPath,
            error );
        if ( error || shaderPath.empty() ) {
            return HRESULT_FROM_WIN32( ERROR_FILE_NOT_FOUND );
        }
        if ( !IsShaderPathWithinRoot( shaderRoot, shaderPath ) ) {
            LogError() << "Rejected shader path outside the shader directory.";
            return E_ACCESSDENIED;
        }

        error.clear();
        if ( !std::filesystem::is_regular_file( shaderPath, error ) || error ) {
            return HRESULT_FROM_WIN32( ERROR_FILE_NOT_FOUND );
        }

        D3D11FileRelativeInclude includeHandler( shaderRoot );
        Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;
        const HRESULT result = D3DCompileFromFile(
            shaderPath.c_str(), macros.data(), &includeHandler,
            szEntryPoint, szShaderModel, shaderFlags, 0, ppBlobOut, &errorBlob );
        if ( FAILED( result ) ) {
            LogError() << "Shader compilation failed: " << shaderPath.filename().string();
            if ( errorBlob && errorBlob->GetBufferPointer() && errorBlob->GetBufferSize() != 0 ) {
                const char* bytes = static_cast<const char*>(errorBlob->GetBufferPointer());
                std::string message( bytes, bytes + errorBlob->GetBufferSize() );
                while ( !message.empty() && message.back() == '\0' ) message.pop_back();
                LogErrorBox() << message;
            }
        }
        return result;
    } catch ( const std::bad_alloc& ) {
        return E_OUTOFMEMORY;
    } catch ( const std::filesystem::filesystem_error& error ) {
        LogError() << "Shader path resolution failed: " << error.what();
        return E_FAIL;
    } catch ( ... ) {
        return E_FAIL;
    }
}
/** Creates list with ShaderInfos */
XRESULT D3D11ShaderManager::Init() {
    Shaders = std::vector<ShaderInfo>();
    static const char* sNums[] = { "0","1","2","3","4","5","6","7","8","9","10","11","12","13","14","15" };

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_Ex>( "VS_Ex.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_1 )  );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_ExNode>( "VS_ExNode.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_1 ) );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_Decal>( "VS_Decal.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_1 )  );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_DecalInstanced>( "VS_DecalInstanced.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_15_VS_DecalInstanced )  );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_ExWater>( "VS_ExWater.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_1 )
        .with_category( ShaderCategory::Water )
        .with_macros( [](std::vector<D3D_SHADER_MACRO>& list) {
#ifdef BUILD_GOTHIC_2_6_fix
            list.push_back( {"SHD_WATERANI", "1"} );
#else
            list.push_back( {"SHD_WATERANI", "0"} );
#endif
        }) );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_ParticlePoint>( "VS_ParticlePoint.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_11_VS_ParticlePoint ) );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_ParticlePointShaded>( "VS_ParticlePointShaded.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_13 )  );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_ExWS>( "VS_ExWS.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_1 ) );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_ExSkeletal>( "VS_ExSkeletal.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_3_VS_ExSkeletal )
        .with_macros( [](std::vector<D3D_SHADER_MACRO>& list) {
            list.push_back( { "SKINNING_STRUCTURED", FeatureLevel10Compatibility ? "0" : "1" } );
        } ) );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_ExSkeletalVN>( "VS_ExSkeletalVN.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_3_VS_ExSkeletal )
        .with_macros( [](std::vector<D3D_SHADER_MACRO>& list) {
            list.push_back( { "SKINNING_STRUCTURED", FeatureLevel10Compatibility ? "0" : "1" } );
        } ) );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_TransformedEx>( "VS_TransformedEx.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_1 ) );
    
    Shaders.push_back( ShaderInfo::make<VShaderID::VS_TransformedEx_MAX_Z>( "VS_TransformedEx.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_1 )
        .with_macros( { {"OVERRIDE_MAX_Z", "1"} } ) );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_ExPointLight>( "VS_ExPointLight.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_1 ) );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_XYZRHW_DIF_T1>( "VS_XYZRHW_DIF_T1.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_7_VS_XYZRHW_DIF_T1 ) );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_XYZRHW_DIF_T1_MAX_Z>( "VS_XYZRHW_DIF_T1.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_7_VS_XYZRHW_DIF_T1 )
        .with_macros( { {"OVERRIDE_MAX_Z", "1"} }));

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_ExInstancedObj>( "VS_ExInstancedObj.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_10_VS_ExInstancedObj )
        .with_macros( [](std::vector<D3D_SHADER_MACRO>& list) {
            const auto& s = Engine::GAPI->GetRendererState().RendererSettings;
#ifdef BUILD_GOTHIC_2_6_fix
            const bool windEnabled = s.WindQuality == GothicRendererSettings::EWindQuality::WIND_QUALITY_ADVANCED;
            list.push_back( {"SHD_WIND",      windEnabled ? "1" : "0"} );
            list.push_back( {"SHD_INFLUENCE", windEnabled ? "1" : "0"} );
            list.push_back( {"WIND_META_SRV", (!FeatureLevel10Compatibility && windEnabled) ? "1" : "0"} );
#elif defined(BUILD_1_12F)
            list.push_back( {"SHD_WIND",      "0"} );
            list.push_back( {"SHD_INFLUENCE", "0"} );
            list.push_back( {"WIND_META_SRV", "0"} );
#else
            const bool windEnabled = haveWindAnimations
                && s.WindQuality == GothicRendererSettings::EWindQuality::WIND_QUALITY_ADVANCED;
            list.push_back( {"SHD_WIND",      windEnabled ? "1" : "0"} );
            list.push_back( {"SHD_INFLUENCE", windEnabled ? "1" : "0"} );
            list.push_back( {"WIND_META_SRV", (!FeatureLevel10Compatibility && windEnabled) ? "1" : "0"} );
#endif
        }) );


    Shaders.push_back( ShaderInfo::make<VShaderID::VS_ExInstanced>( "VS_ExInstanced.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_4_VS_ExInstanced ) );
    Shaders.push_back( ShaderInfo::make<VShaderID::VS_Lines>( "VS_Lines.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_6_Lines )  );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_Lines_XYZRHW>( "VS_Lines_XYZRHW.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_6_Lines )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_Lines>( "PS_Lines.hlsl" ) );
    Shaders.push_back( ShaderInfo::make<PShaderID::PS_LinesSel>( "PS_LinesSel.hlsl" ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_Simple>( "PS_Simple.hlsl" ) );
    Shaders.push_back( ShaderInfo::make<PShaderID::PS_Simple_FF>( "PS_Simple.hlsl" )
        .with_macros( { { "USE_FFDATA", "1" } } ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_Rain>( "PS_Rain.hlsl" ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_Rain_Snow>( "PS_Rain.hlsl" )
        .with_macros( { { "SNOW_FEATURE", "1" } }));

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_Transparency>( "PS_Transparency.hlsl" )  );
    Shaders.push_back( ShaderInfo::make<PShaderID::PS_TransparencySkel>( "PS_TransparencySkel.hlsl" )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_World>( "PS_World.hlsl" ).with_macros({ {"MOTION_VECTORS", "1"}})  );
    Shaders.push_back( ShaderInfo::make<PShaderID::PS_World_NoMV>( "PS_World.hlsl" )  );


    Shaders.push_back( ShaderInfo::make<PShaderID::PS_Water>( "PS_Water.hlsl" )
        .with_category( ShaderCategory::Water )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_WetGroundSSR>( "PS_PFX_WetGroundSSR.hlsl" )
        .with_category( ShaderCategory::Water ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_ParticleDistortion>( "PS_ParticleDistortion.hlsl" )  );
    Shaders.push_back( ShaderInfo::make<PShaderID::PS_ParticleSimple>( "PS_ParticleSimple.hlsl" )  );
    Shaders.push_back( ShaderInfo::make<PShaderID::PS_ParticleSimple_FF>( "PS_ParticleSimple.hlsl" )
        .with_macros( { { "USE_FFDATA", "1" } } ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_ApplyParticleDistortion>( "PS_PFX_ApplyParticleDistortion.hlsl" ) );



    Shaders.push_back( ShaderInfo::make<VShaderID::VS_PFX>( "VS_PFX.hlsl" ) );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_CinemaScope>( "VS_CinemaScope.hlsl" )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_Simple>( "PS_PFX_Simple.hlsl" ) );
    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_Simple_R8>( "PS_PFX_Simple_R8.hlsl" ) );
    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_AOComposite>( "PS_PFX_AOComposite.hlsl" ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_VelocityDebug>( "PS_PFX_VelocityDebug.hlsl" )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_GaussBlur>( "PS_PFX_GaussBlur.hlsl" )  );
    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_BloomCombine>( "PS_PFX_BloomCombine.hlsl" )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_Heightfog>( "PS_PFX_Heightfog.hlsl" )
        .with_category( ShaderCategory::SkyEffects ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_LowClouds>( "PS_PFX_LowClouds.hlsl" )
        .with_category( ShaderCategory::SkyEffects ) );
    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_LowCloudComposite>( "PS_PFX_LowCloudComposite.hlsl" )
        .with_category( ShaderCategory::SkyEffects ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_UnderwaterFinal>( "PS_PFX_UnderwaterFinal.hlsl" )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_Alpha_Blend>( "PS_PFX_Alpha_Blend.hlsl" )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_CinemaScope>( "PS_PFX_CinemaScope.hlsl" )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_LumConvert>( "PS_PFX_LumConvert.hlsl" ) );
    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_LumAdapt>( "PS_PFX_LumAdapt.hlsl" )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_HDR>( "PS_PFX_HDR.hlsl" )
        .with_category(ShaderCategory::Tonemapping) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_GodRayMask>( "PS_PFX_GodRayMask.hlsl" ) );
    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_GodRayZoom>( "PS_PFX_GodRayZoom.hlsl" ) );

    // PostFX Composition uber shader
    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_ScreenSpaceLightingTrace>( "PS_PFX_ScreenSpaceLightingTrace.hlsl" ) );
    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_ScreenSpaceLightingTemporal>( "PS_PFX_ScreenSpaceLightingTemporal.hlsl" ) );
    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_FSR3TransparencyMask>( "PS_PFX_FSR3TransparencyMask.hlsl" ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_Composition>( "PS_PFX_Composition.hlsl" )
        .with_category( ShaderCategory::Other )
        .with_macros( [](std::vector<D3D_SHADER_MACRO>& list) {
            const auto& s = Engine::GAPI->GetRendererState().RendererSettings;
            list.push_back( { "COMPOSE_GODRAYS", s.EnableGodRays ? "1" : "0" } );
            list.push_back( { "COMPOSE_HEIGHTFOG", s.DrawFog ? "1" : "0" } );
            list.push_back( { "COMPOSE_CONTACT_SHADOWS", s.EnableContactShadows ? "1" : "0" } );
            list.push_back( { "COMPOSE_SSGI", (s.EnableScreenSpaceGI && s.ScreenSpaceGIStrength > 0.0f) ? "1" : "0" } );
        } ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_Tonemap>( "PS_PFX_Tonemap.hlsl" )
        .with_category(ShaderCategory::Tonemapping) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_AtmosphereGround>( "PS_AtmosphereGround.hlsl" )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_Atmosphere>( "PS_Atmosphere.hlsl" )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_AtmosphereOuter>( "PS_AtmosphereOuter.hlsl" ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_FixedFunctionPipe>( "PS_FixedFunctionPipe.hlsl" )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_Video>( "PS_Video.hlsl" )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_DS_PointLight>( "PS_DS_PointLight.hlsl" )
        .with_category( ShaderCategory::LightsAndShadows ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_DS_PointLightDynShadow>( "PS_DS_PointLightDynShadow.hlsl" )
        .with_category( ShaderCategory::LightsAndShadows ) );

    // Shadow macro builder shared by both atmospheric scattering shader variants
    ShaderInfo::MacroBuilder shadowMacroBuilder = [](std::vector<D3D_SHADER_MACRO>& list) {
        const auto& s = Engine::GAPI->GetRendererState().RendererSettings;

        const bool useSimpleShadowFallback =
            FeatureLevel10Compatibility || s.DebugSettings.FeatureSet.UseShadowAtlas;
        list.push_back( {"SHD_ENABLE",           s.EnableShadows ? "1" : "0"} );
        list.push_back( {"SHD_FILTER_16TAP_PCF", useSimpleShadowFallback ? "1" : "0"} );
        list.push_back( {"SHD_FILTER_PCSS",      useSimpleShadowFallback ? "0" : "1"} );
        list.push_back( {"MAX_CSM_CASCADES",     TO_LITERAL(MAX_CSM_CASCADES)} );
        list.push_back( {"NUM_CSM_CASCADES",     sNums[std::clamp<size_t>(s.NumShadowCascades, 1, MAX_CSM_CASCADES)]} );
        list.push_back( {"CSM_PCF_LIMIT",        sNums[std::clamp<size_t>(s.ShadowCascadePCFLimit, 0, MAX_CSM_CASCADES)]} );
        list.push_back( {"SHADOW_ATLAS",         (FeatureLevel10Compatibility || s.DebugSettings.FeatureSet.UseShadowAtlas) ? "1" : "0"} );
        list.push_back( {"FP_USE_SHADOW_MASK",   s.DebugSettings.FeatureSet.UseScreenSpaceShadowMask ? "1" : "0"} );
        // Stable full-quality kernels avoid visible stippling on animated characters.
        // Do not rely on temporal AA to reconstruct deliberately undersampled shadows.
        list.push_back( {"SHD_BLUE_NOISE",        "0"} );
        list.push_back( {"PCSS_BLOCKER_TAPS",     "16"} );
        list.push_back( {"PCSS_FILTER_TAPS_NEAR", "32"} );
        list.push_back( {"PCSS_FILTER_TAPS_FAR",  "16"} );
        list.push_back( {"PCF_FILTER_TAPS_NEAR",  "16"} );
        list.push_back( {"PCF_FILTER_TAPS_FAR",   "8"} );
    };

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_DS_AtmosphericScattering>( "PS_DS_AtmosphericScattering.hlsl" )
        .with_macros( shadowMacroBuilder )
        .with_category( ShaderCategory::LightsAndShadows ) );

    Shaders.push_back( ShaderInfo::make<GShaderID::GS_VertexNormals>( "GS_VertexNormals.hlsl" ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_Diffuse>( "PS_Diffuse.hlsl" )
        .with_macros( {
            {"NORMALMAPPING", "0"},
            {"ALPHATEST", "0"}
        } ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PortalDiffuse>( "PS_PortalDiffuse.hlsl" ) ); //forest portals, doors, etc.
    Shaders.push_back( ShaderInfo::make<PShaderID::PS_WaterfallFoam>( "PS_WaterfallFoam.hlsl" ) );     //foam on at the base of waterfalls
    Shaders.push_back( ShaderInfo::make<PShaderID::PS_WaterMask>( "PS_WaterMask.hlsl" ) );
    Shaders.push_back( ShaderInfo::make<PShaderID::PS_TransparencyWetMask>( "PS_TransparencyWetMask.hlsl" ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_DS_AtmosphericScattering_Rain>( "PS_DS_AtmosphericScattering.hlsl" )
        .with_macros( { { "APPLY_RAIN_EFFECTS", "1" } })
        .with_macros( shadowMacroBuilder )
        .with_category( ShaderCategory::LightsAndShadows ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_LinDepth>( "PS_LinDepth.hlsl" )  );


    Shaders.push_back( ShaderInfo::make<PShaderID::PS_DiffuseNormalmapped>( "PS_Diffuse.hlsl" )
        .with_macros( {
            {"NORMALMAPPING", "1"},
            {"ALPHATEST", "0"},
        } ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_DiffuseNormalmappedFxMap>( "PS_Diffuse.hlsl" )
        .with_macros( {
            {"NORMALMAPPING", "1"},
            {"ALPHATEST", "0"},
            {"FXMAP", "1"}
        } ) );


    Shaders.push_back( ShaderInfo::make<PShaderID::PS_DiffuseAlphaTest>( "PS_Diffuse.hlsl" )
        .with_macros( {
            {"NORMALMAPPING", "0"},
            {"ALPHATEST", "1"},
        } ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_DiffuseAlphaTestShadows>( "PS_Diffuse.hlsl" )
        .with_macros( {
            {"NORMALMAPPING", "0"},
            {"ALPHATEST_SHADOWS", "1"},
            } ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_DiffuseNormalmappedAlphaTest>( "PS_Diffuse.hlsl" )
        .with_macros( {
            {"NORMALMAPPING", "1"},
            {"ALPHATEST", "1"},
        } ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_DiffuseNormalmappedAlphaTestFxMap>( "PS_Diffuse.hlsl" )
        .with_macros( {
            {"NORMALMAPPING", "1"},
            {"ALPHATEST", "1"},
            {"FXMAP", "1"}
        } ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_Preview_White>( "PS_Preview.hlsl" )
        .with_macros( { {"RENDERMODE", "0"} }));

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_Preview_Textured>( "PS_Preview.hlsl" )
        .with_macros( { {"RENDERMODE", "1"} } ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_Preview_TexturedLit>( "PS_Preview.hlsl" )
        .with_macros( { {"RENDERMODE", "2"} } ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_Sharpen>( "PS_PFX_Sharpen.hlsl" ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_GammaCorrectInv>( "PS_PFX_GammaCorrectInv.hlsl" ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_DoF_FocusResolve>( "PS_PFX_DoF_FocusResolve.hlsl" ) );
    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_DoF>( "PS_PFX_DoF.hlsl" ) );
    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_DoF_Gauss>( "PS_PFX_DoF.hlsl" )
        .with_macros( {{ "DOF_GAUSS_BLUR", "1" }} ) );
    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_DoF_Composite>( "PS_PFX_DoF_Composite.hlsl" ) );

    if ( FeatureRTArrayIndexFromAnyShader ) {
        Shaders.push_back( ShaderInfo::make<VShaderID::VS_ExLayered>( "VS_ExLayered.hlsl" )
            .with_layout( VERTEX_INPUT_LAYOUT_1 ) );

        Shaders.push_back( ShaderInfo::make<VShaderID::VS_ExNodeLayered>( "VS_ExNodeLayered.hlsl" )
            .with_layout( VERTEX_INPUT_LAYOUT_1 )  );

        Shaders.push_back( ShaderInfo::make<VShaderID::VS_ExSkeletalLayered>( "VS_ExSkeletalLayered.hlsl" )
            .with_layout( VERTEX_INPUT_LAYOUT_3_VS_ExSkeletal )
            .with_macros( [](std::vector<D3D_SHADER_MACRO>& list) {
                list.push_back( { "SKINNING_STRUCTURED", FeatureLevel10Compatibility ? "0" : "1" } );
            } ) ); // cbPerCubeRender for layered rendering
    }
    /*else: always compile fallback shaders*/
    {
        Shaders.push_back( ShaderInfo::make<GShaderID::GS_Cubemap>( "GS_Cubemap.hlsl" )  );

        Shaders.push_back( ShaderInfo::make<VShaderID::VS_ExCube>( "VS_ExCube.hlsl" )
            .with_layout( VERTEX_INPUT_LAYOUT_1 )  );

        Shaders.push_back( ShaderInfo::make<VShaderID::VS_ExNodeCube>( "VS_ExNodeCube.hlsl" )
            .with_layout( VERTEX_INPUT_LAYOUT_1 )  );

        Shaders.push_back( ShaderInfo::make<VShaderID::VS_ExSkeletalCube>( "VS_ExSkeletalCube.hlsl" )
            .with_layout( VERTEX_INPUT_LAYOUT_3_VS_ExSkeletal )
            .with_macros( [](std::vector<D3D_SHADER_MACRO>& list) {
                list.push_back( { "SKINNING_STRUCTURED", FeatureLevel10Compatibility ? "0" : "1" } );
            } ) );
    }

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_ExNodeInstanced>( "VS_ExNodeInstanced.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_14_VS_ExNodeInstanced ) );

    Shaders.push_back( ShaderInfo::make<GShaderID::GS_ParticleStreamOut>( "VS_AdvanceRain.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_13 ) );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_AdvanceRain>( "VS_AdvanceRain.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_13 ) );

    // Velocity Buffer Shader
    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_Velocity>( "PS_PFX_Velocity.hlsl" ) );
    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_SkyVelocity>( "PS_PFX_SkyVelocity.hlsl" ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_CAS>( "PS_PFX_CAS.hlsl" ));


    if ( !FeatureLevel10Compatibility ) {

        Shaders.push_back( ShaderInfo::make<CShaderID::CS_AdvanceRain>( "CS_AdvanceRain.hlsl" ));

        Shaders.push_back( ShaderInfo::make<CShaderID::CS_LightCulling>( "CS_LightCulling.hlsl" )
        .with_macros( {
            { "TILE_SIZE", TO_LITERAL( TILE_SIZE ) },
            { "MAX_LIGHTS_PER_TILE", TO_LITERAL( MAX_LIGHTS_PER_TILE ) },
        }));

        Shaders.push_back( ShaderInfo::make<CShaderID::CS_TiledShading>( "CS_TiledShading.hlsl" ));

        Shaders.push_back( ShaderInfo::make<CShaderID::CS_PFX_GodRayMask>( "CS_PFX_GodRayMask.hlsl" ));

        Shaders.push_back( ShaderInfo::make<CShaderID::CS_PFX_GodRayZoom>( "CS_PFX_GodRayZoom.hlsl" ));

        Shaders.push_back( ShaderInfo::make<CShaderID::CS_PFX_DoF_FocusResolve>( "CS_PFX_DoF_FocusResolve.hlsl" ));
        Shaders.push_back( ShaderInfo::make<CShaderID::CS_PFX_DoF>( "CS_PFX_DoF.hlsl" ));
        Shaders.push_back( ShaderInfo::make<CShaderID::CS_PFX_DoF_Gauss>( "CS_PFX_DoF.hlsl" )
            .with_macros( {{ "DOF_GAUSS_BLUR", "1" }} ) );
        Shaders.push_back( ShaderInfo::make<CShaderID::CS_PFX_DoF_Composite>( "CS_PFX_DoF_Composite.hlsl" ));

        Shaders.push_back( ShaderInfo::make<CShaderID::CS_PFX_XeGTAO_Prefilter>( "CS_PFX_XeGTAO.hlsl" ).with_entrypoint( "CSPrefilterDepths16x16" ) );
        Shaders.push_back( ShaderInfo::make<CShaderID::CS_PFX_XeGTAO_Low>( "CS_PFX_XeGTAO.hlsl" ).with_entrypoint( "CSGTAOLow" ) );
        Shaders.push_back( ShaderInfo::make<CShaderID::CS_PFX_XeGTAO_Medium>( "CS_PFX_XeGTAO.hlsl" ).with_entrypoint( "CSGTAOMedium" ) );
        Shaders.push_back( ShaderInfo::make<CShaderID::CS_PFX_XeGTAO_High>( "CS_PFX_XeGTAO.hlsl" ).with_entrypoint( "CSGTAOHigh" ) );
        Shaders.push_back( ShaderInfo::make<CShaderID::CS_PFX_XeGTAO_Ultra>( "CS_PFX_XeGTAO.hlsl" ).with_entrypoint( "CSGTAOUltra" ) );
        Shaders.push_back( ShaderInfo::make<CShaderID::CS_PFX_XeGTAO_Denoise>( "CS_PFX_XeGTAO.hlsl" ).with_entrypoint( "CSDenoisePass" ) );
        Shaders.push_back( ShaderInfo::make<CShaderID::CS_PFX_XeGTAO_DenoiseLast>( "CS_PFX_XeGTAO.hlsl" ).with_entrypoint( "CSDenoiseLastPass" ) );

        Shaders.push_back( ShaderInfo::make<CShaderID::CS_PFX_Sharpen>( "CS_PFX_Sharpen.hlsl" ));


        // Forward+ pixel shader variants
        Shaders.push_back( ShaderInfo::make<PShaderID::PS_FP_Diffuse>( "PS_Diffuse.hlsl" )
            .with_macros(shadowMacroBuilder)
            .with_macros( {
                { "FORWARD_PLUS", "1" },
                { "NORMALMAPPING", "0" },
                { "ALPHATEST", "0" },
            }).with_category(ShaderCategory::LightsAndShadows));

        Shaders.push_back( ShaderInfo::make<PShaderID::PS_FP_DiffuseNormalmapped>( "PS_Diffuse.hlsl" )
            .with_macros(shadowMacroBuilder)
            .with_macros( {
                { "FORWARD_PLUS", "1" },
                { "NORMALMAPPING", "1" },
                { "ALPHATEST", "0" },
            } ).with_category( ShaderCategory::LightsAndShadows ) );

        Shaders.push_back( ShaderInfo::make<PShaderID::PS_FP_DiffuseNormalmappedFxMap>( "PS_Diffuse.hlsl" )
            .with_macros(shadowMacroBuilder)
            .with_macros( {
                { "FORWARD_PLUS", "1" },
                { "NORMALMAPPING", "1" },
                { "ALPHATEST", "0" },
                { "FXMAP", "1" },
            } ).with_category( ShaderCategory::LightsAndShadows ) );

        Shaders.push_back( ShaderInfo::make<PShaderID::PS_FP_DiffuseAlphaTest>( "PS_Diffuse.hlsl" )
            .with_macros(shadowMacroBuilder)
            .with_macros( {
                { "FORWARD_PLUS", "1" },
                { "NORMALMAPPING", "0" },
                { "ALPHATEST", "1" },
            } ).with_category( ShaderCategory::LightsAndShadows ) );

        Shaders.push_back( ShaderInfo::make<PShaderID::PS_FP_DiffuseNormalmappedAlphaTest>( "PS_Diffuse.hlsl" )
            .with_macros(shadowMacroBuilder)
            .with_macros( {
                { "FORWARD_PLUS", "1" },
                { "NORMALMAPPING", "1" },
                { "ALPHATEST", "1" },
            } ).with_category( ShaderCategory::LightsAndShadows ) );

        Shaders.push_back( ShaderInfo::make<PShaderID::PS_FP_DiffuseNormalmappedAlphaTestFxMap>( "PS_Diffuse.hlsl" )
            .with_macros(shadowMacroBuilder)
            .with_macros( {
                { "FORWARD_PLUS", "1" },
                { "NORMALMAPPING", "1" },
                { "ALPHATEST", "1" },
                { "FXMAP", "1" },
            } ).with_category( ShaderCategory::LightsAndShadows ) );

        Shaders.push_back( ShaderInfo::make<PShaderID::PS_FP_ShadowMask>( "PS_FP_ShadowMask.hlsl" )
            .with_macros(shadowMacroBuilder)
            .with_category( ShaderCategory::LightsAndShadows ) );
    }

    return XR_SUCCESS;
}

static size_t HashCombine( size_t seed, size_t val ) noexcept {
    return seed ^ (val + 0x9e3779b9 + (seed << 6) + (seed >> 2));
}

static size_t ComputeShaderHash( const ShaderInfo& si ) {
    size_t h = 0;

    // Hash file last-modified timestamp
    std::string fullPath = Engine::GAPI->GetStartDirectory() + "\\system\\GD3D11\\Shaders\\" + si.fileName;
    std::error_code ec;
    auto lwt = std::filesystem::last_write_time( std::filesystem::path( fullPath ), ec );
    if ( !ec ) {
        h = HashCombine( h, static_cast<size_t>(lwt.time_since_epoch().count()) );
    }

    // Hash per-shader macros
    for ( const auto& macro : si.shaderMakros ) {
        if ( macro.Name )       h = HashCombine( h, std::hash<std::string_view>{}( macro.Name ) );
        if ( macro.Definition ) h = HashCombine( h, std::hash<std::string_view>{}( macro.Definition ) );
    }

    // Hash dynamic macros via the per-shader builder (only macros this shader actually uses).
    // Shaders without a builder have no renderer-setting-dependent macros to hash.
    if ( si.macroBuilder ) {
        std::vector<D3D_SHADER_MACRO> dynamicMakros;
        si.macroBuilder( dynamicMakros );
        for ( const auto& macro : dynamicMakros ) {
            if ( macro.Name )       h = HashCombine( h, std::hash<std::string_view>{}( macro.Name ) );
            if ( macro.Definition ) h = HashCombine( h, std::hash<std::string_view>{}( macro.Definition ) );
        }
    }

    return h;
}

XRESULT D3D11ShaderManager::CompileShader( ShaderInfo& si ) {
    const size_t newHash = ComputeShaderHash( si );

    auto IsIndexValid = [&]() -> bool {
        switch ( si.type ) {
        case ShaderType::Vertex:   return si.shaderIndex < VShaders.size();
        case ShaderType::Pixel:    return si.shaderIndex < PShaders.size();
        case ShaderType::Geometry: return si.shaderIndex < GShaders.size();
        case ShaderType::Compute:  return si.shaderIndex < CShaders.size();
        default:                   return false;
        }
    };
    if ( !IsIndexValid() ) {
        LogError() << "Shader index out of range: " << si.name;
        return XR_INVALID_ARG;
    }

    auto IsKnown = [&]() -> bool {
        switch ( si.type ) {
        case ShaderType::Vertex:   return IsVShaderKnown( si.shaderIndex );
        case ShaderType::Pixel:    return IsPShaderKnown( si.shaderIndex );
        case ShaderType::Geometry: return IsGShaderKnown( si.shaderIndex );
        case ShaderType::Compute:  return IsCShaderKnown( si.shaderIndex );
        default:                   return false;
        }
    };

    const bool isReload = IsKnown();
    if ( isReload && newHash != 0 && si.compiledHash == newHash ) {
        return XR_SUCCESS;
    }

    std::vector<D3D_SHADER_MACRO> compileMakros = si.shaderMakros;
    if ( si.macroBuilder ) {
        si.macroBuilder( compileMakros );
    }

    const std::string relativePath = "system\\GD3D11\\Shaders\\" + si.fileName;
    const std::filesystem::path fullPath = std::filesystem::path( Engine::GAPI->GetStartDirectory() ) / relativePath;
    std::error_code ec;
    if ( !std::filesystem::is_regular_file( fullPath, ec ) || ec ) {
        LogError() << "Required shader file is missing: " << fullPath.string();
        return XR_FAILED;
    }

    if ( Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog ) {
        LogInfo() << (isReload ? "Reloading shader: " : "Loading shader: ") << si.name;
    }

    XRESULT result = XR_FAILED;
    switch ( si.type ) {
    case ShaderType::Vertex: {
        auto shader = std::make_unique<D3D11VShader>();
        result = shader->LoadShader( si, compileMakros, relativePath.c_str() );
        if ( result == XR_SUCCESS ) {
            UpdateVShader( si.shaderIndex, shader.release() );
        }
        break;
    }
    case ShaderType::Pixel: {
        auto shader = std::make_unique<D3D11PShader>();
        result = shader->LoadShader( si, compileMakros, relativePath.c_str() );
        if ( result == XR_SUCCESS ) {
            UpdatePShader( si.shaderIndex, shader.release() );
        }
        break;
    }
    case ShaderType::Geometry: {
        auto shader = std::make_unique<D3D11GShader>();
        result = shader->LoadShader( relativePath.c_str(), compileMakros, si.layout != 0, si.layout );
        if ( result == XR_SUCCESS ) {
            UpdateGShader( si.shaderIndex, shader.release() );
        }
        break;
    }
    case ShaderType::Compute: {
        auto shader = std::make_unique<D3D11CShader>();
        result = shader->LoadShader(
            relativePath.c_str(), !si.entryPoint.empty() ? si.entryPoint.c_str() : nullptr, compileMakros );
        if ( result == XR_SUCCESS ) {
            UpdateCShader( si.shaderIndex, shader.release() );
        }
        break;
    }
    default:
        result = XR_INVALID_ARG;
        break;
    }

    if ( result != XR_SUCCESS ) {
        LogError() << "Failed to " << (isReload ? "reload" : "load") << " shader: " << si.fileName;
        return result;
    }

    si.compiledHash = newHash;
    return XR_SUCCESS;
}
/** Loads/Compiles Shaderes from list */
XRESULT D3D11ShaderManager::LoadShaders( ShaderCategory categories ) {
    // Temporarily disable multi-core shader compilation

    /*size_t numThreads = std::thread::hardware_concurrency();
    if ( numThreads > 1 ) {
        numThreads = numThreads - 1;
    }
    auto compilationTP = std::make_unique<ThreadPool>( numThreads );
    LogInfo() << "Compiling/Reloading shaders with " << compilationTP->getNumThreads() << " threads";
    */
    LogInfo() << "Compiling/Reloading shaders";
    XRESULT overallResult = XR_SUCCESS;
    for ( ShaderInfo& si : Shaders ) {
        // Determine shader type category
        ShaderCategory shaderTypeCategory = ShaderCategory::None;
        if ( si.type == ShaderType::Vertex ) {
            shaderTypeCategory = ShaderCategory::Vertex;
        } else if ( si.type == ShaderType::Pixel ) {
            shaderTypeCategory = ShaderCategory::Pixel;
        } else if ( si.type == ShaderType::Geometry ) {
            shaderTypeCategory = ShaderCategory::Geometry;
        } else if ( si.type == ShaderType::Compute ) {
            shaderTypeCategory = ShaderCategory::Compute;
        }

        // Check if shader type matches requested categories
        bool typeMatches = HasCategory( categories, shaderTypeCategory );

        // Check if shader content category matches requested categories
        bool contentMatches = HasCategory( categories, si.contentCategory );

        if ( !typeMatches && !contentMatches ) {
            // Skip if neither type nor content category matches
            continue;
        }

        if ( CompileShader( si ) != XR_SUCCESS ) {
            overallResult = XR_FAILED;
        }
        // compilationTP->enqueue( [this, si]() { CompileShader( si ); } );
    }

    // Join all threads (call Threadpool destructor)
    // compilationTP.reset();

    return overallResult;
}

/** Deletes all shaders and loads them again */
XRESULT D3D11ShaderManager::ReloadShaders( ShaderCategory categories ) {
    if ( categories == ShaderCategory::None ) return XR_SUCCESS;

    std::lock_guard<std::mutex> lock( _ReloadMutex );
    ShaderCategoriesToReloadNextFrame |= categories;
    return XR_SUCCESS;
}

/** Called on frame start */
XRESULT D3D11ShaderManager::OnFrameStart() {
    ShaderCategory categories = ShaderCategory::None;
    {
        std::lock_guard<std::mutex> lock( _ReloadMutex );
        categories = ShaderCategoriesToReloadNextFrame;
        ShaderCategoriesToReloadNextFrame = ShaderCategory::None;
    }

    return categories != ShaderCategory::None
        ? LoadShaders( categories )
        : XR_SUCCESS;
}

/** Deletes all shaders */
XRESULT D3D11ShaderManager::DeleteShaders() {
    std::scoped_lock lock(
        _VShaderMutex, _PShaderMutex, _GShaderMutex, _CShaderMutex );

    for ( auto& shader : VShaders ) {
        shader.reset();
    }
    for ( auto& shader : PShaders ) {
        shader.reset();
    }
    for ( auto& shader : GShaders ) {
        shader.reset();
    }
    for ( auto& shader : CShaders ) {
        shader.reset();
    }

    return XR_SUCCESS;
}

void D3D11ShaderManager::UpdateShaderInfo( ShaderInfo& shader ) {
    for ( size_t i = 0; i < Shaders.size(); i++ ) {
        if ( Shaders[i].type == shader.type && Shaders[i].shaderIndex == shader.shaderIndex ) {
            Shaders[i] = shader;
            CompileShader( Shaders[i] );
            return;
        }
    }
    Shaders.push_back( shader );
    CompileShader( Shaders.back() );
}
