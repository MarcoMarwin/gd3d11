#pragma once
#include "pch.h"
#include <DirectXMath.h>

#define FFX_CPU
#include "Shaders/FidelityFX/ffx_core.h"

/** Actual instance data for a vob */
struct VobInstanceInfo {
    XMFLOAT4X4 world;
    XMFLOAT4X4 prevWorld;  // Previous frame's world matrix for motion vectors
    DWORD color;
    float windStrenth;
    float canBeAffectedByPlayer;
    // General purpose slot. Used by instanced VOB rendering to store an index
    // into optional per-visual metadata buffers.
    DWORD GP_Slot;
    // RGB8 emission color plus an A8 palette code for explicitly emissive VOB materials.
    DWORD emissiveColor;
};
static_assert( offsetof( VobInstanceInfo, emissiveColor ) == 144 );
static_assert( sizeof( VobInstanceInfo ) == 148 );

struct VobWindMetadata {
    float MinHeight;
    float MaxHeight;
    float2 HorizontalExtent;
    // World-space plane of Gothic's actual ground polygon (normal.xyz, distance).
    // A zero normal selects the legacy local-bounds fallback.
    float4 GroundPlane;
    float GrassShear;
    float3 Padding;
};
static_assert( sizeof( VobWindMetadata ) == 48 );

/** Oriented local volume cut from the Gothic world mesh by an actual window VOB. */
struct WindowCutoutVolume {
    XMFLOAT4 CenterExtentX;
    XMFLOAT4 AxisXExtentY;
    XMFLOAT4 AxisYExtentZ;
    XMFLOAT4 AxisZPadding;
};
static_assert( sizeof( WindowCutoutVolume ) == 64 );

struct WindowCutoutConstants {
    XMFLOAT4X4 InvView;
    WindowCutoutVolume Volumes[32];
    uint32_t Count;
    float Padding[3];
    // 16 x 9 screen tiles, packed four uint masks per vector. Each bit selects
    // one of the 32 volumes that can affect that tile.
    XMUINT4 TileMasks[36];
    XMFLOAT2 PixelToTile;
    XMFLOAT2 TileOrigin;
    uint32_t TileCountX;
    uint32_t TileCountY;
    uint32_t TilePadding[2];
};
static_assert( sizeof( WindowCutoutConstants ) == 2736 );

/** Per-instance data for instanced node attachment rendering */
struct NodeAttachmentInstanceData {
    XMFLOAT4X4 World;
    XMFLOAT4X4 PrevWorld;
    float4 Color;
};

/** Remap-index for the static vobs */
struct VobInstanceRemapInfo {
    bool operator < ( const VobInstanceRemapInfo& b ) const {
        return InstanceRemapIndex < b.InstanceRemapIndex;
    }

    bool operator == ( const VobInstanceRemapInfo& o ) const {
        return InstanceRemapIndex == o.InstanceRemapIndex;
    }

    DWORD InstanceRemapIndex;
};

#pragma pack (push, 1)	
struct SkyConstantBuffer {
    float SC_TextureWeight;
    float3 SC_pad1;
};

struct GammaCorrectConstantBuffer {
    float G_Gamma;
    float G_Brightness;
    float G_OutputDitherStrength;
    float G_Pad;
};

struct PfxSharpenConstantBuffer {
    float2 G_TextureSize;
    float G_SharpenStrength;
    float G_pad;
};

struct BlurConstantBuffer {
    float2 B_PixelSize;
    float B_BlurSize;
    float B_Threshold;

    float4 B_ColorMod;
};

struct BloomCombineConstantBuffer {
    float BC_BaseWeight;
    float BC_WideWeight;
    float2 BC_Pad;
};

struct DepthOfFieldConstantBuffer {
    float DoF_FocusDistance;
    float DoF_FocusRange;
    float DoF_BokehRadius;
    float DoF_MaxBlur;

    float4 DoF_ProjParams;
    float DoF_NearPlane;
    float DoF_FarPlane;
    float DoF_NearBlurDistance;
    float DoF_NearBlurStrength;
};

struct PerObjectState {
    float3 OS_AmbientColor;
    float OS_Pad;
};

struct PFXVS_ConstantBuffer {
    float4 PFXVS_ProjParams; // x = 1/P._11, y = 1/P._22, z = P._43, w = P._33
};

struct HeightfogConstantBuffer {
    float4 HF_ProjParams; // x = 1/P._11, y = 1/P._22, z = P._43, w = P._33
    XMFLOAT4X4 InvView;
    float3 CameraPosition;
    float HF_FogHeight;
    float HF_HeightFalloff;
    float HF_GlobalDensity;
    float HF_WeightZNear;
    float HF_WeightZFar;
    float3 HF_FogColorMod;
    float HF_FogOverride;
    float2 HF_ProjAB;
    float2 HF_Pad3;
    float3 HF_RainFogColor;
    float HF_RainGlobalDensity;
    float HF_RainFogHeight;
    float HF_RainHeightFalloff;
    float HF_RainWeightZNear;
    float HF_RainWeightZFar;
};
static_assert( sizeof(HeightfogConstantBuffer) == 176, "HeightfogConstantBuffer must be exactly 176 bytes" );


struct CompositionControlConstantBuffer {
    float CC_HeightFogEnabled;
    float CC_GodRaysEnabled;
    XMFLOAT2 CC_Pad;
};

struct LumAdaptConstantBuffer {
    float LC_DeltaTime;
    float3 LC_Pad;
};

struct GodRayZoomConstantBuffer {
    float GR_Decay;
    float GR_Weight;
    float2 GR_Center;

    float GR_Density;
    float3 GR_ColorMod;
};

struct HDRSettingsConstantBuffer {
    float HDR_MiddleGray;
    float HDR_LumWhite;
    float HDR_Threshold;
    float HDR_BloomStrength;
};

struct LPMConstantsBuffer {
    uint32_t LPM_Ctl[24][4];
};

struct ViewportInfoConstantBuffer {
    float2 VPI_ViewportSize;
    float2 VPI_pad;
};

struct DS_PointLightConstantBuffer {
    float4 PL_Color;

    float PL_Range;
    float3 Pl_PositionWorld;

    float PL_Outdoor;
    float3 Pl_PositionView;

    float2 PL_ViewportSize;
    float PL_IgnoreIndoorOutdoorLimit;
    float PL_ShadowSoftness;

    float4 PL_ProjParams; // x = 1/P._11, y = 1/P._22, z = P._43, w = P._33
    XMFLOAT4X4 PL_InvView;

    float3 PL_LightScreenPos;
    float PL_ShadowStrength;

    // Runtime pointlight filter settings; kept in a separate 16-byte block.
    uint32_t PL_ShadowFilterMode;
    uint32_t PL_ShadowFilterPad[3];
};

constexpr int MAX_CSM_CASCADES = 4;
struct GodRayVolumetricConstantBuffer {
    float4 GRV_ProjParams;
    XMFLOAT4X4 GRV_InvView;
    float3 GRV_CameraPosition;
    float GRV_MaxDistance;
    XMFLOAT4X4 GRV_ShadowViewProj[MAX_CSM_CASCADES];
    float4 GRV_LightColor;
    float3 GRV_LightDirectionWS;
    float GRV_ShadowmapSize;
    float GRV_FogHeight;
    float GRV_HeightFalloff;
    float GRV_GlobalDensity;
    float GRV_WeightZNear;
    float GRV_WeightZFar;
    float GRV_RainFogHeight;
    float GRV_RainHeightFalloff;
    float GRV_RainGlobalDensity;
    float GRV_RainWeightZNear;
    float GRV_RainWeightZFar;
    float GRV_FogOverride;
    float GRV_RainWeight;
    float GRV_SunVisibility;
    float GRV_Strength;
    uint32_t GRV_FrameIndex;
    uint32_t GRV_NumCascades;
    XMFLOAT4X4 GRV_PreviousViewProjection;
    float2 GRV_InvOutputSize;
    float GRV_HistoryValid;
    float GRV_HistoryWeight;
};
static_assert( sizeof(GodRayVolumetricConstantBuffer) == 528, "GodRayVolumetricConstantBuffer must be exactly 528 bytes" );
struct DS_ScreenQuadConstantBuffer {
    float4 SQ_ProjParams; // x = 1/P._11, y = 1/P._22, z = P._43, w = P._33
    XMFLOAT4X4 SQ_InvView;
    XMFLOAT4X4 SQ_View;

    XMFLOAT4X4 SQ_RainViewProj;

    float3 SQ_LightDirectionVS;
    float SQ_ShadowmapSize;

    float4 SQ_LightColor;
    
    // CSM: Cascade 0 (for compatibility with existing shaders)
    XMFLOAT4X4 SQ_ShadowViewProj[MAX_CSM_CASCADES];

    float SQ_ShadowStrength;
    float SQ_ShadowAOStrength;
    float SQ_WorldAOStrength;
    float SQ_ShadowSoftness;
    uint32_t SQ_FrameIndex;
    float2 SQ_JitterOffset;
    float SQ_LightSize;

    // Cascade atlas UV rect (xy = offset, zw = scale); unused for texture arrays.
    float4 SQ_CascadeAtlasRect[MAX_CSM_CASCADES];

    // Exact world-space light direction used to build each stabilized cascade.
    // NPC receivers use this to keep their normal bias synchronized with the
    // shadow projection instead of the continuously moving sky direction.
    float4 SQ_CascadeLightDirectionWS[MAX_CSM_CASCADES];

    // x = temporal reconstruction; y = PCF/PCSS quality (0 / 1 / 2);
    // z = runtime world-shadow enable. These remain runtime-controlled so
    // changing the associated settings never requires shader reloads.
    float4 SQ_ShadowRuntimeParams;

    // x = active CSM cascade count; y = number of near cascades using the
    // high-quality filter.
    // These are runtime-controlled for the same reason as SQ_ShadowRuntimeParams.
    float4 SQ_ShadowCascadeRuntimeParams;

    // Far distances of cascades 0..3 in camera/view space. Retained as an
    // extension of the runtime layout; the active Build-221 sampling path
    // selects cascades from their light-space projections.
    float4 SQ_ShadowCascadeSplits;

    // Actual pixel resolution of cascade 0..3. In atlas mode the far
    // cascades are half-size and therefore cannot use SQ_ShadowmapSize.
    float4 SQ_CascadeShadowResolution;

    // World-space units per texel for each cascade, precomputed on the CPU.
    float4 SQ_CascadeTexelSize;

    // Atlas dimensions in pixels (x = width, y = height); unused for arrays.
    float4 SQ_ShadowAtlasSize;
};

struct CloudConstantBuffer {
    float3 C_LightDirection;
    float C_Pad;

    float3 C_CloudPosition;
    float C_Pad2;
};

struct AdvanceRainConstantBuffer {
    float3 AR_LightPosition;
    float AR_FPS;

    float3 AR_CameraPosition;
    float AR_Radius;

    float AR_Height;
    float3 AR_GlobalVelocity;

    int AR_MoveRainParticles;
    float3 AR_Pad1;
};

struct VS_ExConstantBuffer_PerFrame {
    XMFLOAT4X4 View;
    XMFLOAT4X4 InvView;
    XMFLOAT4X4 Projection;
    XMFLOAT4X4 ViewProj;           // Jittered for rendering
    XMFLOAT4X4 PrevViewProj;       // Previous frame's unjittered view-projection for motion vectors
    XMFLOAT4X4 UnjitteredViewProj; // Current frame's unjittered view-projection for motion vectors
};

constexpr int MAX_CHARACTER_INTERACTION_NPCS = 5;
constexpr int MAX_CHARACTER_INTERACTION_INFLUENCERS = 1 + MAX_CHARACTER_INTERACTION_NPCS;

struct VS_ExConstantBuffer_Wind {
    float3 windDir;
    float globalTime;

    float minHeight;
    float maxHeight;
    float prevGlobalTime;
    float accurateWindVelocity;
    float4 cameraWorldPosition;

    // xyz = actor world position, w = active flag. Slot 0 is the hero, following slots are nearby NPCs.
    float4 interactionPositions[MAX_CHARACTER_INTERACTION_INFLUENCERS];
    float characterInteractionStrength;
    float characterInteractionRange;
    float windEnabled;
    float influenceEnabled;
    float windMetadataEnabled;
    float padding1;
};

struct ParticlePointShadingConstantBuffer {
    XMFLOAT4X4 View;
    XMFLOAT4X4 Projection;
};

struct VS_ExConstantBuffer_PerInstance {
    XMFLOAT4X4 World;
    float4 Color;
};

struct VS_ExConstantBuffer_PerInstanceNode {
    XMFLOAT4X4 World;
    XMFLOAT4X4 PrevWorld; // Added for motion vectors
    float4 Color;
    float Fatness;
    float Scaling;
    float2 Pad1;
};

struct VS_ExConstantBuffer_PerInstanceSkeletal {
    XMFLOAT4X4 World;
    XMFLOAT4X4 PrevWorld; // Added for motion vectors
    float4 PI_ModelColor;
    float PI_ModelFatness;
    float3 PI_Pad1;
};

struct VS_ExConstantBuffer_SkeletalBoneRange {
    unsigned int BoneOffset;
    unsigned int PrevBoneOffset;
    unsigned int BoneCount;
    unsigned int UseStructuredBones;
};

struct ScreenFadeConstantBuffer {
    float GA_Alpha;
    float3 GA_Pad;
};

struct GhostAlphaConstantBuffer {
    float2 GA_ViewportSize;
    float GA_Alpha;
    float GA_LightingScale;
    float3 GA_LightingTint;
    float GA_Pad;
};

struct GrassConstantBuffer {
    float3 G_NormalVS;
    float G_Time;
    float G_WindStrength;
    float3 G_Pad1;
};

struct CubemapGSConstantBuffer {
    XMFLOAT4X4 PCR_View[6]; // View matrices for cube map rendering
    XMFLOAT4X4 PCR_ViewProj[6];
};

struct ParticleGSInfoConstantBuffer {
    float3 CameraPosition;
    float PGS_RainFxWeight;
    float PGS_RainHeight;
    float PGS_Pad;
    float2 PGS_RainScale;
};

struct RefractionInfoConstantBuffer {
    XMFLOAT4X4 RI_Projection;
    float2 RI_ViewportSize;
    float RI_Time;
    float RI_Far;

    float3 RI_CameraPosition;
    float RI_Pad2;

    XMFLOAT4X4 RI_ViewProj;
    XMFLOAT4X4 RI_InvViewProj; // world-space water-depth reconstruction
};
static_assert( sizeof( RefractionInfoConstantBuffer ) == 224 );

struct WetGroundSSRConstantBuffer {
    float4 WG_ProjParams;
    XMFLOAT4X4 WG_InvView;
    XMFLOAT4X4 WG_ViewProj;
    XMFLOAT4X4 WG_RainViewProj;

    float3 WG_CameraPosition;
    float WG_Wetness;

    float2 WG_InvResolution;
    float WG_Strength;
    float WG_Time;

    float WG_RainFXWeight;
    float3 WG_RainFogColor;

    float WG_RainFogDensity;
    float WG_FogRange;
    float WG_WetMaterialReflectionsStrength;
    float WG_ProceduralPuddlesStrength;

    float WG_PuddleReflectionsStrength;
    float WG_WetGroundRainImpactsStrength;
    float WG_PuddleAccumulation;
    float WG_ReflectionsEnabled;
};

static_assert( sizeof(WetGroundSSRConstantBuffer) == 288, "WetGroundSSRConstantBuffer must be exactly 288 bytes" );

struct AtmosphereConstantBuffer {
    float AC_Kr4PI;
    float AC_Km4PI;
    float AC_g;
    float AC_KrESun;

    float AC_KmESun;
    float AC_InnerRadius;
    float AC_OuterRadius;
    float AC_Scale;

    float3 AC_Wavelength;
    float AC_RayleighScaleDepth;


    float AC_RayleighOverScaleDepth;
    int AC_nSamples;
    float AC_fSamples;
    float AC_CameraHeight;

    float3 AC_CameraPos;
    float AC_Time;
    float3 AC_LightPos;
    float AC_SceneWettness;

    float3 AC_MoonPos;
    float AC_MoonVisibility;

    float3 AC_SpherePosition;
    float AC_RainFXWeight;

    float AC_EnableSSR;
    float AC_EnableSSS;
    float AC_SSRStrength;
    float AC_SSSIntensity;

    float AC_WaterCubemapStrength;
    float AC_EnableNightAtmosphere;
    float AC_NearNightBrightness;
    float AC_NightFogBrightness;

    float AC_NightDarkeningStart;
    float AC_NightDarkeningMax;
    float AC_NightDarkeningRange;
    float AC_SunVisibility;

    float3 AC_WorldCameraPos;
    float AC_SkyEffectsEnabled;

    float AC_EnableParticleLighting;
    float AC_ParticleLightingStrength;
    float AC_Pad3;
    // 1 = normal night fog, 0 = suppress the night-fog blend.
    float AC_NightFogEnabled;

    XMFLOAT3 AC_Pad5;
    float AC_Pad6;
    XMFLOAT3 AC_Pad7;
    float AC_Pad8;
    XMFLOAT3 AC_Pad9;
    float AC_Pad10;
    // 1 = keep the legacy atmosphere dither, 0 = suppress it for FSR3.
    float AC_AtmosphereDitherEnabled;
    float AC_Pad12;
    float AC_Pad13;
    float AC_DayRainAtmosphereStrength;

    XMFLOAT3 AC_LowCloudDayColor;
    float AC_LowCloudDensity;
    XMFLOAT3 AC_LowCloudRainColor;
    float AC_LowCloudScale;
    XMFLOAT3 AC_LowCloudNightColor;
    float AC_LowCloudSpeed;
    float AC_LowCloudHeightScale;
    float AC_LowCloudDistanceScale;
    float AC_LowCloudSunLight;
    float AC_LowCloudPad0;

    float4 AC_LightScreenPos;
    float4 AC_MoonScreenPos;
};

struct CASConstantBuffer {
    FfxUInt32x4 const0;  // CasSetup output
    FfxUInt32x4 const1;  // CasSetup output
};


struct VelocityDebugConstantBuffer {
    float Amplification;    // Multiplier for velocity values (e.g., 10-100)
    float ShowMagnitude;    // 0 = show direction as RG, 1 = show magnitude as grayscale
    float AbsoluteMode;     // 0 = signed (-1 to 1), 1 = absolute values
    float Padding;
};

#define TILE_SIZE 16

struct LightCullingConstantBuffer {
    float ProjScaleX;
    float ProjScaleY;
    uint32_t ScreenWidth;
    uint32_t ScreenHeight;
    uint32_t TotalLights;
    uint32_t NumTilesX;
    float NearZ;
    float FarZ;
};
static_assert( sizeof(LightCullingConstantBuffer) == 32, "LightCullingConstantBuffer must match CS_LightCulling" );

struct TiledShadingConstantBuffer {
    float2 ViewportSize;
    float2 JitterOffset;
    float4 ProjParams; // x = 1/P._11, y = 1/P._22, z = P._43, w = P._33
    uint32_t LimitLightIntensity;
    uint32_t NumTilesX;
    float ClusterNearZ;
    float ClusterFarZ;
    XMFLOAT4X4 InvView; // For world-space reconstruction (shadow sampling)
};
static_assert( sizeof(TiledShadingConstantBuffer) == 112, "TiledShadingConstantBuffer must match CS_TiledShading" );

struct ForwardPlusTileConstantBuffer {
    float2 ViewportSize;
    uint32_t NumTilesX;
    uint32_t LimitLightIntensity;
    float ClusterNearZ;
    float ClusterFarZ;
    float TilePad[2];
};
static_assert( sizeof(ForwardPlusTileConstantBuffer) == 32, "ForwardPlusTileConstantBuffer must match Forward+" );

struct PsSimpleFFdata {
    float4 textureFactor;
    float2 windowSkyParams;
    float2 padding;
};

#pragma pack (pop)
