#include "GSky.h"

#include "BaseGraphicsEngine.h"
#include "D3D11Texture.h"
#include "Engine.h"
#include "GMesh.h"
#include "oCGame.h"
#include "zCMaterial.h"
#include "zCTimer.h"
#include "zCTexture.h"
#include "zCSkyController_Outdoor.h"
#include "zCWorld.h"
#include "corecrt_io.h"
#include <cmath>

GSky::GSky() {
    Atmosphere.Kr = 0.0075f;
    Atmosphere.Km = 0.0010f;
    Atmosphere.ESun = 20.0f;
    Atmosphere.InnerRadius = 800000;
    Atmosphere.OuterRadius = 900000;
    Atmosphere.Samples = 3;
    Atmosphere.RayleightScaleDepth = 0.18f;
    Atmosphere.G = -0.995f;
    //Atmosphere.WaveLengths = float3(0.65f, 0.57f, 0.475f);
    Atmosphere.WaveLengths = float3( 0.63f, 0.57f, 0.50f );
    Atmosphere.SpherePosition = XMFLOAT3( 0, 0, 0 );
    Atmosphere.SphereOffsetY = -820000;
    Atmosphere.SkyTimeScale = 1.0f;
    XMStoreFloat3( &Atmosphere.LightDirection, XMVector3Normalize( XMVectorSplatOne() ) );

    ZeroMemory( &AtmosphereCB, sizeof( AtmosphereCB ) );
}

GSky::~GSky() {
    for ( unsigned int i = 0; i < SkyTextures.size(); i++ ) {
        SAFE_DELETE( SkyTextures[i] );
    }
}

/** Creates needed resources by the sky */
XRESULT GSky::InitSky() {
    const float sizeX = 500000;
    const float sizeY = 10000;

    SkyPlaneVertices[0].Position = float3( -sizeX, sizeY, -sizeX ); // 0
    SkyPlaneVertices[1].Position = float3( +sizeX, sizeY, -sizeX ); // 1
    SkyPlaneVertices[2].Position = float3( -sizeX, sizeY, +sizeX ); // 2

    SkyPlaneVertices[3].Position = float3( +sizeX, sizeY, -sizeX ); // 1
    SkyPlaneVertices[4].Position = float3( +sizeX, sizeY, +sizeX ); // 3
    SkyPlaneVertices[5].Position = float3( -sizeX, sizeY, +sizeX ); // 2

    const float scale = 20.0f;
    XMFLOAT2 displacement;
    float4 color = float4( 1, 1, 1, 1 );

    // Construct vertices
    // 0
    SkyPlaneVertices[0].TexCoord = float2( displacement );
    SkyPlaneVertices[0].Color = color.ToDWORD();

    // 1
    FXMVECTOR xm_displacement = XMLoadFloat2( &displacement );
    XMFLOAT2 SkyPlaneVertices1;
    XMStoreFloat2( &SkyPlaneVertices1, (XMVectorSet( scale, 0, 0, 0 ) + xm_displacement) );
    SkyPlaneVertices[1].TexCoord = SkyPlaneVertices1;
    SkyPlaneVertices[1].Color = color.ToDWORD();

    // 2
    XMFLOAT2 SkyPlaneVertices2;
    XMStoreFloat2( &SkyPlaneVertices2, (XMVectorSet( 0, scale, 0, 0 ) + xm_displacement) );
    SkyPlaneVertices[2].TexCoord = SkyPlaneVertices2;
    SkyPlaneVertices[2].Color = color.ToDWORD();

    // ---

    // 1
    SkyPlaneVertices[3].TexCoord = SkyPlaneVertices1;
    SkyPlaneVertices[3].Color = color.ToDWORD();

    // 3
    XMFLOAT2 SkyPlaneVertices4;
    XMStoreFloat2( &SkyPlaneVertices4, (XMVectorSet( scale, scale, 0, 0 ) + xm_displacement) );
    SkyPlaneVertices[4].TexCoord = SkyPlaneVertices4;
    SkyPlaneVertices[4].Color = color.ToDWORD();

    // 2
    SkyPlaneVertices[5].TexCoord = SkyPlaneVertices2;
    SkyPlaneVertices[5].Color = color.ToDWORD();

    return XR_SUCCESS;
}

/** Returns the skyplane */
MeshInfo* GSky::GetSkyPlane() {
    return SkyPlane.get();
}

/** Adds a sky texture. Sky textures must be in order to make the daytime work */
XRESULT GSky::AddSkyTexture( const std::string& file ) {
    D3D11Texture* t;
    XLE( Engine::GraphicsEngine->CreateTexture( &t ) );
    XLE( t->Init( file ) );

    SkyTextures.push_back( t );

    return XR_SUCCESS;
}

/** Loads the sky resources */
XRESULT GSky::LoadSkyResources() {
    SkyDome = std::make_unique<GMesh>();
    SkyDome->LoadMesh( "system\\GD3D11\\meshes\\unitSphere.obj" );

    LogInfo() << "Loading sky textures...";

    D3D11Texture* cloudTex;
    XLE( Engine::GraphicsEngine->CreateTexture( &cloudTex ) );
    CloudTexture.reset( cloudTex );

    switch ( DaySkyTexture ) {
    case ESkyTexture::ST_OldWorld:
        XLE( CloudTexture->Init(
            "system\\GD3D11\\Textures\\SkyDay_G1.dds" ) );
        Atmosphere.WaveLengths = float3( 0.54f, 0.56f, 0.60f );
        break;

    case ESkyTexture::ST_NewWorld:
    default:
        XLE( CloudTexture->Init(
            "system\\GD3D11\\Textures\\SkyDay.dds" ) );
        Atmosphere.WaveLengths = float3( 0.63f, 0.57f, 0.50f );
        break;
    }

    ApplyDaySkyColorProfile();

    D3D11Texture* rainCloudTex;
    XLE( Engine::GraphicsEngine->CreateTexture( &rainCloudTex ) );
    RainCloudTexture.reset( rainCloudTex );
    XLE( RainCloudTexture->Init( "system\\GD3D11\\Textures\\RainCloud.dds" ) );

    D3D11Texture* nightTex;
    XLE( Engine::GraphicsEngine->CreateTexture( &nightTex ) );
    NightTexture.reset( nightTex );

    XLE( NightTexture->Init( "system\\GD3D11\\Textures\\starsh.dds" ) );
    D3D11Texture* moonTex;
    XLE( Engine::GraphicsEngine->CreateTexture( &moonTex ) );
    MoonTexture.reset( moonTex );
    XLE( MoonTexture->Init( "system\\GD3D11\\Textures\\Moon.dds" ) );

    VERTEX_INDEX indices[] = { 0, 1, 2, 3, 4, 5 };
    SkyPlane = std::make_unique<MeshInfo>();
    SkyPlane->Create( SkyPlaneVertices, 6, indices, 6 );

    return XR_SUCCESS;
}

void GSky::ApplyDaySkyColorProfile() {
    Engine::GAPI->GetRendererState().RendererSettings.ApplySkyColorValues( DaySkyTexture == ESkyTexture::ST_OldWorld );
}

/** Sets the current sky texture */
void GSky::SetSkyTexture( ESkyTexture texture ) {
    DaySkyTexture = texture;

    D3D11Texture* cloudTex;
    XLE( Engine::GraphicsEngine->CreateTexture( &cloudTex ) );
    CloudTexture.reset( cloudTex );

    // Load the specific new texture
    switch ( texture ) {
    case ESkyTexture::ST_NewWorld:
        XLE( CloudTexture->Init( "system\\GD3D11\\Textures\\SkyDay.dds" ) );
        Atmosphere.WaveLengths = float3( 0.63f, 0.57f, 0.50f );
        break;

    case ESkyTexture::ST_OldWorld:
        XLE( CloudTexture->Init( "system\\GD3D11\\Textures\\SkyDay_G1.dds" ) );
        Atmosphere.WaveLengths = float3( 0.54f, 0.56f, 0.60f );
        break;
    }

    ApplyDaySkyColorProfile();
}

/** Sets the custom cloud sky texture */
void GSky::SetCustomCloudAndNightTexture( int idx, bool isNightTexture, bool isOldWorld ) {
    if ( idx == -1 ) {
        if ( isNightTexture) {
            D3D11Texture* nightTex;
            XLE( Engine::GraphicsEngine->CreateTexture( &nightTex ) );
            NightTexture.reset( nightTex );
            XLE( NightTexture->Init( "system\\GD3D11\\Textures\\starsh.dds" ) );
        } else {
            SetSkyTexture( isOldWorld ? ESkyTexture::ST_OldWorld : ESkyTexture::ST_NewWorld );
        }
    } else {
        std::string textureFile;
        textureFile.append( "system\\GD3D11\\Textures\\CustomSky\\" )
            .append( isNightTexture ? "SkyNight_G" : "SkyDay_G" )
            .append( std::to_string( isOldWorld ? 1 : 2 ) )
            .append( "_" )
            .append( std::to_string( idx ) )
            .append( ".dds" );

        if ( _access( textureFile.c_str(), 0 ) != -1 ) {
            if ( isNightTexture ) {
                D3D11Texture* nightTex;
                XLE( Engine::GraphicsEngine->CreateTexture( &nightTex ) );
                NightTexture.reset( nightTex );
                XLE( NightTexture->Init( textureFile ) );
            } else {
                D3D11Texture* cloudTex;
                XLE( Engine::GraphicsEngine->CreateTexture( &cloudTex ) );
                CloudTexture.reset( cloudTex );
                XLE( CloudTexture->Init( textureFile ) );
                DaySkyTexture = isOldWorld ? ESkyTexture::ST_OldWorld : ESkyTexture::ST_NewWorld;
                Atmosphere.WaveLengths = isOldWorld ? float3( 0.54f, 0.56f, 0.60f ) : float3( 0.63f, 0.57f, 0.50f );
                ApplyDaySkyColorProfile();
            }
        }
    }
}

/** Sets the custom sky texture */
void GSky::SetCustomSkyTexture_ZenGin( bool isNightTexture, zCTexture* texture, bool isOldWorld ) {
    if ( !texture ) {
        if ( isNightTexture ) {
            D3D11Texture* nightTex;
            XLE( Engine::GraphicsEngine->CreateTexture( &nightTex ) );
            NightTexture.reset( nightTex );
            XLE( NightTexture->Init( "system\\GD3D11\\Textures\\starsh.dds" ) );
            NightTexture_Zen = nullptr;
        } else {
            SetSkyTexture( isOldWorld ? ESkyTexture::ST_OldWorld : ESkyTexture::ST_NewWorld );
            CloudTexture_Zen = nullptr;
        }
    } else {
        (isNightTexture ? NightTexture_Zen : CloudTexture_Zen) = texture;
        if ( !isNightTexture ) {
            DaySkyTexture = isOldWorld ? ESkyTexture::ST_OldWorld : ESkyTexture::ST_NewWorld;
            Atmosphere.WaveLengths = isOldWorld ? float3( 0.54f, 0.56f, 0.60f ) : float3( 0.63f, 0.57f, 0.50f );
            ApplyDaySkyColorProfile();
        }
    }
}

void GSky::SetCustomSkyWavelengths( float X, float Y, float Z ) {
    Atmosphere.WaveLengths = float3( X, Y, Z );
}

/** Returns the sky-texture for the passed daytime (0..1) */
void GSky::GetTextureOfDaytime( float time, D3D11Texture** t1, D3D11Texture** t2, float* factor ) {
    if ( !SkyTextures.size() )
        return;

    time -= floor( time ); // Fractionalize, put into 0..1 range

    // Get the index of the current texture
    float index = time * (SkyTextures.size() - 0.5f);

    // Get indices of the current and the next texture
    int i0 = static_cast<int>(index);
    int i1 = static_cast<unsigned int>(index + 1) < SkyTextures.size() ? static_cast<int>(index) + 1 : 0;

    // Calculate weight
    float weight = index - i0;

    *t1 = SkyTextures[i0];
    *t2 = SkyTextures[i1];
    *factor = weight;
}

/** Renders the sky */
XRESULT GSky::RenderSky() {
    if ( !SkyDome ) {
        XLE( LoadSkyResources() );
    }

    XMFLOAT3 camPos = Engine::GAPI->GetCameraPosition();

    const bool rainEnabled = Engine::GAPI->GetRendererState().RendererSettings.EnableRain;
    float atmosphericRainWeight = rainEnabled ? Engine::GAPI->GetRainFXWeight() : 0.0f;
    if ( !std::isfinite( atmosphericRainWeight ) ) {
        atmosphericRainWeight = 0.0f;
    }
    atmosphericRainWeight = std::clamp( atmosphericRainWeight, 0.0f, 1.0f );

    XMFLOAT3 LightDir = {};
    XMFLOAT3 MoonDir = {};
    float masterTime = -1.0f;

    if ( Engine::GAPI->GetRendererState().RendererSettings.ReplaceSunDirection ) {
        LightDir = Atmosphere.LightDirection;
        XMStoreFloat3( &MoonDir, XMVectorNegate( XMLoadFloat3( &LightDir ) ) );
    } else {
        zCSkyController_Outdoor* sc = oCGame::GetGame()->_zCSession_world->GetSkyControllerOutdoor();
        if ( sc ) {
            masterTime = sc->GetMasterTime();
            LightDir = sc->GetSunWorldPosition( Atmosphere.SkyTimeScale );
            MoonDir = sc->GetMoonWorldPosition( Atmosphere.SkyTimeScale );
            Atmosphere.LightDirection = LightDir;
        }
    }
    XMStoreFloat3( &LightDir, XMVector3Normalize( XMLoadFloat3( &LightDir ) ) );
    if ( XMVectorGetX( XMVector3LengthSq( XMLoadFloat3( &MoonDir ) ) ) < 0.001f ) {
        XMStoreFloat3( &MoonDir, XMVectorNegate( XMLoadFloat3( &LightDir ) ) );
    } else {
        XMStoreFloat3( &MoonDir, XMVector3Normalize( XMLoadFloat3( &MoonDir ) ) );
    }
    //Atmosphere.SpherePosition.y = -Atmosphere.InnerRadius;

    Atmosphere.SpherePosition.x = 0;//Engine::GAPI->GetLoadedWorldInfo()->MidPoint.x;
    Atmosphere.SpherePosition.z = 0;//Engine::GAPI->GetLoadedWorldInfo()->MidPoint.y;
    Atmosphere.SpherePosition.y = 0;//Engine::GAPI->GetLoadedWorldInfo()->LowestVertex - Atmosphere.InnerRadius;

    XMFLOAT3 sp = camPos;
    sp.y += Atmosphere.SphereOffsetY;

    // Fill atmosphere buffer for this frame
    AtmosphereCB.AC_CameraPos = XMFLOAT3( 0, -Atmosphere.SphereOffsetY, 0 );
    AtmosphereCB.AC_Time = Engine::GAPI->GetTimeSeconds();
    AtmosphereCB.AC_LightPos = LightDir;
    AtmosphereCB.AC_MoonPos = MoonDir;
    auto smoothFade = []( float value ) {
        value = std::clamp( value, 0.0f, 1.0f );
        return value * value * (3.0f - 2.0f * value);
    };

    float sunTimeFade = smoothFade( LightDir.y / 0.12f );
    float moonTimeFade = smoothFade( MoonDir.y / 0.12f );
    if ( masterTime >= 0.0f ) {
        constexpr float dawnMoonFadeStart = 4.25f; // 04:15
        constexpr float dawnSunFadeStart = 4.50f; // 04:30
        constexpr float duskSunFadeStart = 19.25f; // 19:15
        constexpr float duskMoonFadeStart = 19.50f; // 19:30
        constexpr float transitionHours = 15.0f / 60.0f;
        const float dawnMoonFadeEnd = dawnMoonFadeStart + transitionHours;
        const float dawnSunFadeEnd = dawnSunFadeStart + transitionHours;
        const float duskSunFadeEnd = duskSunFadeStart + transitionHours;
        const float duskMoonFadeEnd = duskMoonFadeStart + transitionHours;
        const float gameHour = fmodf( masterTime * 24.0f + 12.0f, 24.0f );

        sunTimeFade = 0.0f;
        moonTimeFade = 0.0f;
        if ( gameHour >= dawnSunFadeStart && gameHour < duskSunFadeEnd ) {
            sunTimeFade = 1.0f;
            if ( gameHour < dawnSunFadeEnd ) {
                sunTimeFade = smoothFade( (gameHour - dawnSunFadeStart) / transitionHours );
            } else if ( gameHour >= duskSunFadeStart ) {
                sunTimeFade = 1.0f - smoothFade( (gameHour - duskSunFadeStart) / transitionHours );
            }
        } else if ( gameHour >= duskMoonFadeStart || gameHour < dawnMoonFadeEnd ) {
            moonTimeFade = 1.0f;
            if ( gameHour >= duskMoonFadeStart && gameHour < duskMoonFadeEnd ) {
                moonTimeFade = smoothFade( (gameHour - duskMoonFadeStart) / transitionHours );
            } else if ( gameHour >= dawnMoonFadeStart && gameHour < dawnMoonFadeEnd ) {
                moonTimeFade = 1.0f - smoothFade( (gameHour - dawnMoonFadeStart) / transitionHours );
            }
        }
    }

    const float rainLightFade = 1.0f - std::clamp( atmosphericRainWeight * 2.0f, 0.0f, 1.0f );
    AtmosphereCB.AC_SunVisibility = sunTimeFade * rainLightFade;
    AtmosphereCB.AC_MoonVisibility = moonTimeFade * rainLightFade;

    const float celestialDistance = std::max( 10000.0f, Engine::GAPI->GetFarPlane() );
    XMFLOAT4X4 celestialProjection = Engine::GAPI->GetProjectionMatrix();
    // Screen-space atmosphere effects must not follow the temporal sub-pixel jitter.
    celestialProjection._13 = 0.0f;
    celestialProjection._23 = 0.0f;
    const XMMATRIX celestialView = Engine::GAPI->GetViewMatrixXM();
    const XMMATRIX celestialProjectionMatrix = XMLoadFloat4x4( &celestialProjection );
    auto projectCelestialDirection = [&]( const XMFLOAT3& direction ) {
        XMVECTOR directionVector = XMVector3Normalize( XMLoadFloat3( &direction ) );
        XMVECTOR viewDirection = XMVector3Normalize(
            XMVector3TransformNormal( directionVector, celestialView ) );
        XMVECTOR viewPosition = XMVectorSet(
            XMVectorGetX( viewDirection ) * celestialDistance,
            XMVectorGetY( viewDirection ) * celestialDistance,
            XMVectorGetZ( viewDirection ) * celestialDistance,
            1.0f );
        XMFLOAT4 clipPosition;
        XMStoreFloat4(
            &clipPosition,
            XMVector4Transform( viewPosition, celestialProjectionMatrix ) );
        float visible = 0.0f;
        float screenX = 0.5f;
        float screenY = 0.5f;
        if ( fabsf( clipPosition.w ) > 0.0001f ) {
            const float invW = 1.0f / clipPosition.w;
            const float ndcX = clipPosition.x * invW;
            const float ndcY = clipPosition.y * invW;
            const float ndcZ = clipPosition.z * invW;
            screenX = ndcX * 0.5f + 0.5f;
            screenY = -ndcY * 0.5f + 0.5f;
            if ( clipPosition.w > 0.0f && ndcZ >= 0.0f && ndcZ <= 1.0f ) {
                const float edgeDistance = std::min(
                    std::min( screenX + 0.25f, 1.25f - screenX ),
                    std::min( screenY + 0.25f, 1.25f - screenY ) );
                const float fadeWidth = 0.35f;
                const float fade = std::clamp(
                    (edgeDistance + fadeWidth) / fadeWidth,
                    0.0f,
                    1.0f );
                visible = fade * fade * (3.0f - 2.0f * fade);
            }
        }
        return XMFLOAT4( screenX, screenY, visible, 0.0f );
    };
    AtmosphereCB.AC_LightScreenPos =
        projectCelestialDirection( LightDir );
    AtmosphereCB.AC_MoonScreenPos =
        projectCelestialDirection( MoonDir );
    AtmosphereCB.AC_CameraHeight = -Atmosphere.SphereOffsetY;
    AtmosphereCB.AC_InnerRadius = Atmosphere.InnerRadius;
    AtmosphereCB.AC_OuterRadius = Atmosphere.OuterRadius;
    AtmosphereCB.AC_nSamples = Atmosphere.Samples;
    AtmosphereCB.AC_fSamples = static_cast<float>(AtmosphereCB.AC_nSamples);

    AtmosphereCB.AC_Kr4PI = Atmosphere.Kr * 4 * XM_PI;
    AtmosphereCB.AC_Km4PI = Atmosphere.Km * 4 * XM_PI;
    AtmosphereCB.AC_KrESun = Atmosphere.Kr * Atmosphere.ESun;
    AtmosphereCB.AC_KmESun = Atmosphere.Km * Atmosphere.ESun;

    AtmosphereCB.AC_Scale = 1.0f / (AtmosphereCB.AC_OuterRadius - AtmosphereCB.AC_InnerRadius);
    AtmosphereCB.AC_RayleighScaleDepth = Atmosphere.RayleightScaleDepth;
    AtmosphereCB.AC_RayleighOverScaleDepth = AtmosphereCB.AC_Scale / AtmosphereCB.AC_RayleighScaleDepth;
    AtmosphereCB.AC_g = Atmosphere.G;
    AtmosphereCB.AC_Wavelength = Atmosphere.WaveLengths;
    AtmosphereCB.AC_SpherePosition = sp;

    if ( !Engine::GAPI->GetRendererState().RendererSettings.EnableRain ) {
        AtmosphereCB.AC_SceneWettness = 0.f;
    } else {
        AtmosphereCB.AC_SceneWettness =
            Engine::GAPI->GetSceneWetness();
    }
    AtmosphereCB.AC_RainFXWeight = atmosphericRainWeight;
    AtmosphereCB.AC_EnableSSR = Engine::GAPI->GetRendererState().RendererSettings.EnableSSR ? 1.0f : 0.0f;
    AtmosphereCB.AC_EnableSSS = 1.0f;
    AtmosphereCB.AC_SSRStrength = Engine::GAPI->GetRendererState().RendererSettings.SSRStrength * 0.84f;
    AtmosphereCB.AC_SSSIntensity = 1.0f;
    const auto& rendererSettings = Engine::GAPI->GetRendererState().RendererSettings;
    AtmosphereCB.AC_WaterCubemapStrength = rendererSettings.WaterCubemapStrength;
    AtmosphereCB.AC_EnableNightAtmosphere = 1.0f;
    AtmosphereCB.AC_NearNightBrightness = rendererSettings.NightNearBrightness;
    AtmosphereCB.AC_NightFogBrightness = rendererSettings.NightFogBrightness;
    AtmosphereCB.AC_NightDarkeningStart = rendererSettings.NightDarkeningStart;
    AtmosphereCB.AC_NightDarkeningRange = rendererSettings.NightDarkeningRange;
    AtmosphereCB.AC_NightDarkeningMax = rendererSettings.NightDarkeningMax;
    // AC_SunVisibility was filled together with the moon visibility above.
    AtmosphereCB.AC_WorldCameraPos = camPos;
    AtmosphereCB.AC_EnableContactShadows = rendererSettings.EnableContactShadows ? 1.0f : 0.0f;
    AtmosphereCB.AC_EnableScreenSpaceGI = (rendererSettings.EnableScreenSpaceGI && rendererSettings.ScreenSpaceGIStrength > 0.0f) ? 1.0f : 0.0f;
    AtmosphereCB.AC_SkyEffectsEnabled = rendererSettings.EnableRain ? 1.0f : 0.0f;
    AtmosphereCB.AC_ContactShadowStrength = (rendererSettings.EnableContactShadows ? rendererSettings.GetContactShadowFixedStrength() : 0.0f) * GetMainLightVisibility();
    AtmosphereCB.AC_ScreenSpaceGIStrength = rendererSettings.ScreenSpaceGIStrength;
    AtmosphereCB.AC_EnableParticleLighting = rendererSettings.EnableParticleLighting ? 1.0f : 0.0f;
    AtmosphereCB.AC_ParticleLightingStrength = rendererSettings.ParticleLightingStrength * 1.5f;
    AtmosphereCB.AC_Pad3 = 0.0f;
    AtmosphereCB.AC_Pad4 = 0.0f;
    AtmosphereCB.AC_Pad5 = XMFLOAT3( 0.0f, 0.0f, 0.0f );
    AtmosphereCB.AC_Pad6 = 0.0f;
    AtmosphereCB.AC_Pad7 = XMFLOAT3( 0.0f, 0.0f, 0.0f );
    AtmosphereCB.AC_Pad8 = 0.0f;
    AtmosphereCB.AC_Pad9 = XMFLOAT3( 0.0f, 0.0f, 0.0f );
    AtmosphereCB.AC_Pad10 = 0.0f;
    AtmosphereCB.AC_Pad11 = 0.0f;
    AtmosphereCB.AC_Pad12 = 0.0f;
    AtmosphereCB.AC_Pad13 = 0.0f;
    AtmosphereCB.AC_DayRainAtmosphereStrength = rendererSettings.DayRainAtmosphereStrength;
    AtmosphereCB.AC_LowCloudDayColor = rendererSettings.DynamicCloudDayColor;
    AtmosphereCB.AC_LowCloudDensity = rendererSettings.DynamicCloudDensity;
    AtmosphereCB.AC_LowCloudRainColor = rendererSettings.DynamicCloudRainColor;
    AtmosphereCB.AC_LowCloudScale = rendererSettings.DynamicCloudScale;
    AtmosphereCB.AC_LowCloudNightColor = rendererSettings.DynamicCloudNightColor;
    AtmosphereCB.AC_LowCloudSpeed = rendererSettings.DynamicCloudSpeed;
    AtmosphereCB.AC_LowCloudHeightScale = rendererSettings.DynamicCloudHeight;
    AtmosphereCB.AC_LowCloudDistanceScale = rendererSettings.DynamicCloudDistance;
    AtmosphereCB.AC_LowCloudSunLight = rendererSettings.DynamicCloudSunLight;
    AtmosphereCB.AC_LowCloudPad0 = 0.0f;

    //Engine::GraphicsEngine->DrawSky();

    // Extract fog settings
    /*zCSkyController_Outdoor* sky = oCGame::GetGame()->_zCSession_world->GetSkyControllerOutdoor();
    Engine::GAPI->GetRendererState().GraphicsState.FF_FogColor = float3(sky->GetMasterState()->FogColor / 255.0f);
    Engine::GAPI->GetRendererState().GraphicsState.FF_FogNear = 0.3f * sky->GetMasterState()->FogDist; // That 0.3f is hardcoded in gothic
    Engine::GAPI->GetRendererState().GraphicsState.FF_FogFar = sky->GetMasterState()->FogDist;
    */
    return XR_SUCCESS;
}

/** Returns the loaded sky-Dome */
GMesh* GSky::GetSkyDome() {
    return SkyDome.get();
}

/** Returns the current sky-light color */
float4 GSky::GetSkylightColor() {
    zCSkyController_Outdoor* sc = oCGame::GetGame()->_zCSession_world->GetSkyControllerOutdoor();
    return float4( 1, 1, 1, 1 );
}

/** Returns the cloud texture */
D3D11Texture* GSky::GetCloudTexture() {
    if ( CloudTexture_Zen ) {
        if ( CloudTexture_Zen->CacheIn( -1 ) == zRES_CACHED_IN ) {
            if ( MyDirectDrawSurface7* dds7 = CloudTexture_Zen->GetSurface() ) {
                return dds7->GetEngineTexture();
            }
        }
    }
    return CloudTexture.get();
}

/** Returns the cloud texture */
D3D11Texture* GSky::GetNightTexture() {
    if ( NightTexture_Zen ) {
        if ( NightTexture_Zen->CacheIn( -1 ) == zRES_CACHED_IN ) {
            if ( MyDirectDrawSurface7* dds7 = NightTexture_Zen->GetSurface() ) {
                return dds7->GetEngineTexture();
            }
        }
    }
    return NightTexture.get();
}

/** Returns the renderer-owned copy of Gothic's moon texture. */
D3D11Texture* GSky::GetMoonTexture() {
    return MoonTexture.get();
}

bool GSky::IsMoonLightActive() const {
    return AtmosphereCB.AC_MoonVisibility > AtmosphereCB.AC_SunVisibility;
}

XMFLOAT3 GSky::GetMainLightDirection() const {
    const float3& direction =
        IsMoonLightActive() ? AtmosphereCB.AC_MoonPos : AtmosphereCB.AC_LightPos;
    XMFLOAT3 result( direction.x, direction.y, direction.z );
    if ( XMVectorGetX( XMVector3LengthSq( XMLoadFloat3( &result ) ) ) < 0.001f ) {
        result = Atmosphere.LightDirection;
    }
    return result;
}

float GSky::GetMainLightVisibility() const {
    // The weights are mutually exclusive, so only one directional shadow map is active.
    return std::max( AtmosphereCB.AC_SunVisibility, AtmosphereCB.AC_MoonVisibility );
}

// The scale equation calculated by Vernier's Graphical Analysis
float AC_Escale( float fCos, float rayleighScaleDepth ) {
    float x = 1.0f - fCos;
    return rayleighScaleDepth * exp( -0.00287f + x * (0.459f + x * (3.83f + x * (-6.80f + x * 5.25f))) );
}

// Calculates the Mie phase function
float AC_getMiePhase( float fCos, float fCos2, float g, float g2 ) {
    return 1.5f * ((1.0f - g2) / (2.0f + g2)) * (1.0f + fCos2) / pow( abs( 1.0f + g2 - 2.0f * g * fCos ), 1.5f );
}

// Calculates the Rayleigh phase function
float AC_getRayleighPhase( float fCos2 ) {
    //return 1.0;
    return 0.75f + 0.75f * fCos2;
}
// Returns the near intersection point of a line and a sphere
float AC_getNearIntersection( FXMVECTOR v3Pos, FXMVECTOR v3Ray, float fDistance2, float fRadius2 ) {
    float B;
    XMStoreFloat( &B, XMVector3Dot( v3Pos, v3Ray ) * 2.0f );
    float C = fDistance2 - fRadius2;
    float fDet = std::max( 0.0f, B * B - 4.0f * C );
    return 0.5f * (-B - sqrt( fDet ));
}
// Returns the far intersection point of a line and a sphere
float AC_getFarIntersection( FXMVECTOR v3Pos, FXMVECTOR v3Ray, float fDistance2, float fRadius2 ) {
    float B;
    XMStoreFloat( &B, XMVector3Dot( v3Pos, v3Ray ) * 2.0f );
    float C = fDistance2 - fRadius2;
    float fDet = std::max( 0.0f, B * B - 4.0f * C );
    return 0.5f * (-B + sqrt( fDet ));
}

/** Returns the current sun color */
float3 GSky::GetSunColor() {
    XMVECTOR LightPos = XMVectorSet( AtmosphereCB.AC_LightPos.x, AtmosphereCB.AC_LightPos.y, AtmosphereCB.AC_LightPos.z, 0.0f );
    XMVECTOR camPos = XMVectorSet( AtmosphereCB.AC_CameraPos.x, AtmosphereCB.AC_CameraPos.y, AtmosphereCB.AC_CameraPos.z, 0.0f );

    XMVECTOR wPos = (LightPos * AtmosphereCB.AC_OuterRadius) + XMLoadFloat3( &Atmosphere.SpherePosition );
    XMVECTOR vPos = wPos - XMVectorSet( AtmosphereCB.AC_SpherePosition.x, AtmosphereCB.AC_SpherePosition.y, AtmosphereCB.AC_SpherePosition.z, 0 );
    XMVECTOR vRay = vPos - camPos;

    float fFar;
    XMStoreFloat( &fFar, XMVector3Length( vRay ) );
    vRay /= fFar;

    // Calculate the ray's starting position, then calculate its scattering offset
    float fDepth = exp( AtmosphereCB.AC_RayleighOverScaleDepth * (AtmosphereCB.AC_InnerRadius - AtmosphereCB.AC_CameraHeight) );
    float fStartAngle;
    XMStoreFloat( &fStartAngle, XMVector3Dot( vRay, camPos ) / XMVector3Length( camPos ) );
    float fStartOffset = fDepth * AC_Escale( fStartAngle, AtmosphereCB.AC_RayleighScaleDepth );

    // Initialize the scattering loop variables
    float fSampleLength = fFar / AtmosphereCB.AC_fSamples;
    float fScaledLength = fSampleLength * AtmosphereCB.AC_Scale;
    XMVECTOR vSampleRay = vRay * fSampleLength;
    XMVECTOR vSamplePoint = camPos + vSampleRay * 0.5f;

    constexpr XMVECTORF32 Four_XMV = { 4, 4, 4, 0 };
    XMVECTOR vInvWavelength = XMQuaternionInverse( XMVectorPow( XMVectorSet( AtmosphereCB.AC_Wavelength.x, AtmosphereCB.AC_Wavelength.y, AtmosphereCB.AC_Wavelength.z, 0 ), Four_XMV ) );

    // Now loop through the sample rays
    XMVECTOR vFrontColor = XMVectorZero();
    float fHeight_float;
    float fLightAngle;
    float fCameraAngle;
    for ( int i = 0; i < AtmosphereCB.AC_nSamples; i++ ) {
        XMVECTOR fHeight = XMVector3Length( vSamplePoint );
        XMStoreFloat( &fHeight_float, fHeight );
        float fDepth = exp( AtmosphereCB.AC_RayleighOverScaleDepth * (AtmosphereCB.AC_InnerRadius - fHeight_float) );
        XMStoreFloat( &fLightAngle, XMVector3Dot( LightPos, vSamplePoint ) / fHeight );
        XMStoreFloat( &fCameraAngle, XMVector3Dot( vRay, vSamplePoint ) / fHeight );
        float fScatter = (fStartOffset + fDepth * (AC_Escale( fLightAngle, AtmosphereCB.AC_RayleighScaleDepth ) - AC_Escale( fCameraAngle, AtmosphereCB.AC_RayleighScaleDepth )));

        XMVECTOR vAttenuate = XMVectorExp( -fScatter * vInvWavelength * AtmosphereCB.AC_Kr4PI + XMVectorSet( AtmosphereCB.AC_Km4PI, AtmosphereCB.AC_Km4PI, AtmosphereCB.AC_Km4PI, 0 ) );
        vFrontColor += vAttenuate * fDepth * fScaledLength * 2;
        vSamplePoint += vSampleRay;
    }

    // Finally, scale the Mie and Rayleigh colors and set up the varying variables for the pixel shader
    XMVECTOR c0 = vFrontColor * vInvWavelength * AtmosphereCB.AC_KrESun;
    XMVECTOR c1 = vFrontColor * AtmosphereCB.AC_KmESun;
    XMVECTOR vDirection = camPos - vPos;

    float fCos;
    XMStoreFloat( &fCos, XMVector3Dot( LightPos, vDirection ) / XMVector3Length( vDirection ) );

    XMFLOAT3 suncolor_convert;
    float fCos2 = fCos * fCos;
    XMStoreFloat3( &suncolor_convert, AC_getRayleighPhase( fCos2 ) * c0 + AC_getMiePhase( fCos, fCos2, AtmosphereCB.AC_g, AtmosphereCB.AC_g * AtmosphereCB.AC_g ) * c1 );

    float3 suncolor_return;
    suncolor_return = suncolor_convert;
    return suncolor_return;
}
