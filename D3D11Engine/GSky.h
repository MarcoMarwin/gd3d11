#pragma once
#include "pch.h"
#include "ConstantBufferStructs.h"

struct AtmosphereSettings {
    float G;
    float Kr;
    float Km;
    float ESun;
    float InnerRadius;
    float OuterRadius;
    int Samples;
    float RayleightScaleDepth;
    float3 WaveLengths;
    XMFLOAT3 SpherePosition;
    float SphereOffsetY;
    XMFLOAT3 LightDirection;
    float SkyTimeScale;
};

enum ESkyTexture {
    ST_NewWorld,
    ST_OldWorld
};

class zCTexture;
class zCSkyLayer;
class zCSkyState;
class GMesh;
class D3D11Texture;

class GSky {
public:
    GSky();
    ~GSky();

    /** Renders the sky */
    XRESULT RenderSky();

    /** Returns the loaded sky-Dome */
    GMesh* GetSkyDome();

    /** Sets the current sky texture */
    void SetSkyTexture( ESkyTexture texture );
    ESkyTexture GetDaySkyTexture() const { return DaySkyTexture; }

    void SetCustomCloudAndNightTexture( int idx, bool isNightTexture, bool isOldWorld );
    void SetCustomSkyTexture_ZenGin( bool isNightTexture, zCTexture* texture, bool isOldWorld );
    void SetCustomSkyWavelengths(float X, float Y, float Z);
    
    /** Returns the atmospheric parameters */
    AtmosphereConstantBuffer& GetAtmosphereCB() { return AtmosphereCB; }

    /** returns atmosphere settings */
    AtmosphereSettings& GetAtmosphereSettings() { return Atmosphere; }

    /** Returns the cloud texture */
    D3D11Texture* GetCloudTexture();

    /** Returns the rain-cloud density texture. */
    D3D11Texture* GetRainCloudTexture();

    /** Re-seeds the atmospheric transition after loading a world or savegame. */
    void ResetWeatherState();

    /** Returns the night texture */
    D3D11Texture* GetNightTexture();

    /** Returns the high-resolution renderer moon texture. */
    D3D11Texture* GetMoonTexture();

    /** Returns the active sun/moon direction used for outdoor directional lighting. */
    XMFLOAT3 GetMainLightDirection() const;
    float GetMainLightVisibility() const;
    bool IsMoonLightActive() const;

    /** Returns the current sun color */
    float3 GetSunColor();

protected:
    void ApplyDaySkyColorProfile();

    /** Loads the sky textures */
    XRESULT LoadSkyResources();

    /** Sky mesh */
    std::unique_ptr<GMesh> SkyDome;

    std::unique_ptr<D3D11Texture> CloudTexture;
    std::unique_ptr<D3D11Texture> RainCloudTexture;
    std::unique_ptr<D3D11Texture> NightTexture;
    std::unique_ptr<D3D11Texture> MoonTexture;

    zCTexture* CloudTexture_Zen = nullptr;
    zCTexture* NightTexture_Zen = nullptr;
    ESkyTexture DaySkyTexture = ESkyTexture::ST_NewWorld;

    /** Atmospheric variables */
    AtmosphereConstantBuffer AtmosphereCB;
    AtmosphereSettings Atmosphere;
    float AtmosphericRainWeight = 0.0f;
    DWORD AtmosphericRainLastUpdateMs = 0;
    DWORD AtmosphericRainDropStartMs = 0;
    DWORD AtmosphericRainSettledStartMs = 0;
    bool AtmosphericRainInitialized = false;
    bool AtmosphericRainReleasing = false;
    bool AtmosphericRainSettingInitialized = false;
    bool AtmosphericRainSettingEnabled = true;
    DWORD SkyResourceLastAttemptMs = 0;
};
