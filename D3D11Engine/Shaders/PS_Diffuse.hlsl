//--------------------------------------------------------------------------------------
// World/VOB-Pixelshader for G2D3D11 by Degenerated
//--------------------------------------------------------------------------------------
#include <AtmosphericScattering.h>
#include <FFFog.h>
#include <DS_Defines.h>
#include <Toolbox.h>
// NW_MISC_OILLAMP_02 emission helpers. Kept local so shader deployment needs
// no additional include file.
float OilLampBrightnessMask(float3 diffuseColor)
{
    const float luminance = dot(diffuseColor, float3(0.2126f, 0.7152f, 0.0722f));
    return smoothstep(0.32f, 0.82f, luminance);
}

float3 ComputeOilLampEmission(float3 diffuseColor, float3 linkedLightColor)
{
    // INSTANCE_EMISSIVE_COLOR contains the authored/palette color in sRGB.
    // Convert it before adding it to the linear HDR lighting buffer. Treating
    // the sRGB bytes as linear and multiplying by 1.35 clipped warm white into
    // an apparently cold neutral white.
    const float3 linearLightColor = pow(saturate(linkedLightColor), 2.2f);
    return linearLightColor * OilLampBrightnessMask(diffuseColor) * 1.10f;
}

cbuffer MI_MaterialInfo : register( b2 )
{
	float MI_SpecularIntensity;
	float MI_SpecularPower;
	float MI_NormalmapStrength;
	float MI_ParallaxOcclusionStrength;
	float MI_WetGroundSSRStrength;
	float MI_MaterialPadding0;
	float MI_MaterialPadding1;
	float MI_MaterialPadding2;
	
	float4 MI_Color;
}

cbuffer DIST_Distance : register( b3 )
{
	float DIST_DrawDistance;
	float3 DIST_Pad;
}

//--------------------------------------------------------------------------------------
// Textures and Samplers
//--------------------------------------------------------------------------------------
SamplerState SS_Linear : register( s0 );
SamplerState SS_samMirror : register( s1 );
Texture2D	TX_Texture0 : register( t0 );
Texture2D	TX_Texture1 : register( t1 );
Texture2D	TX_Texture2 : register( t2 );
Texture2D	TX_Displacement : register( t13 );
TextureCube	TX_ReflectionCube : register( t4 );

struct WindowCutoutVolume
{
    float4 CenterExtentX;
    float4 AxisXExtentY;
    float4 AxisYExtentZ;
    float4 AxisZPadding;
};

cbuffer WindowCutoutConstants : register( b6 )
{
    matrix WindowCutoutInvView;
    WindowCutoutVolume WindowCutouts[32];
    uint WindowCutoutCount;
    float3 WindowCutoutPadding;
    uint4 WindowCutoutTileMasks[36];
    float2 WindowCutoutPixelToTile;
    float2 WindowCutoutTileOrigin;
    uint2 WindowCutoutTileCount;
    uint2 WindowCutoutTilePadding;
}

void ClipWindowCutouts(float3 worldPosition, float2 pixelPosition)
{
	// Startup, loading screens and worlds without visible validated windows keep
	// b6 unbound or publish Count == 0. Exit before any tile arithmetic/indexing.
	if (WindowCutoutCount == 0u)
		return;

    uint2 tile = min(uint2(max(pixelPosition - WindowCutoutTileOrigin, 0.0f)
        * WindowCutoutPixelToTile),
        max(WindowCutoutTileCount, 1u) - 1u);
    uint tileIndex = tile.y * WindowCutoutTileCount.x + tile.x;
    uint4 packedMasks = WindowCutoutTileMasks[tileIndex >> 2u];
    uint tileMask = packedMasks[tileIndex & 3u];
    if (tileMask == 0u)
        return;

    [loop]
    while (tileMask != 0u)
    {
        uint i = (uint)firstbitlow(tileMask);
        tileMask &= tileMask - 1u;
        WindowCutoutVolume cutout = WindowCutouts[i];
        float3 relativePosition = worldPosition - cutout.CenterExtentX.xyz;
        float distanceX = abs(dot(relativePosition, cutout.AxisXExtentY.xyz));
        float distanceY = abs(dot(relativePosition, cutout.AxisYExtentZ.xyz));
        float distanceZ = abs(dot(relativePosition, cutout.AxisZPadding.xyz));
        if (distanceX <= cutout.CenterExtentX.w
            && distanceY <= cutout.AxisXExtentY.w
            && distanceZ <= cutout.AxisYExtentZ.w)
        {
            discard;
        }
    }
}

#ifdef FORWARD_PLUS
#include <include/ForwardPlusLighting.hlsl>
// Pre-computed screen-space CSM shadow mask from the shadow mask pre-pass (bound at t12)
Texture2D FP_ShadowMask : register( t12 );
#endif

//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
struct PS_INPUT
{
	float2 vTexcoord		: TEXCOORD0;
	float2 vTexcoord2		: TEXCOORD1;
	float4 vDiffuse			: TEXCOORD2;
	float3 vNormalVS		: TEXCOORD4;
	float3 vViewPosition	: TEXCOORD5;
	float4 vCurrClipPos     : TEXCOORD6;  // Current clip position for velocity (from instanced VS)
	float4 vPrevClipPos     : TEXCOORD7;  // Previous clip position for velocity (from instanced VS)
	float4 vEmissiveColor   : TEXCOORD8;
	float4 vPosition		: SV_POSITION;
};

// Calculate screen-space velocity from clip positions
float2 CalculateVelocity(float4 currClipPos, float4 prevClipPos)
{
	// Handle edge case where clip positions are invalid (w == 0)
	if (currClipPos.w == 0.0 || prevClipPos.w == 0.0)
		return float2(0, 0);
	
	// Perspective divide to get NDC [-1,1]
	float2 currNDC = currClipPos.xy / currClipPos.w;
	float2 prevNDC = prevClipPos.xy / prevClipPos.w;
	
	// Convert NDC to UV space [0,1]
	// Note: Y is flipped between NDC (Y+ up) and UV (Y+ down)
	float2 currUV = float2(currNDC.x * 0.5 + 0.5, 1.0 - (currNDC.y * 0.5 + 0.5));
	float2 prevUV = float2(prevNDC.x * 0.5 + 0.5, 1.0 - (prevNDC.y * 0.5 + 0.5));
	
	// Velocity = current - previous (where the pixel came from)
	float2 velocity = prevUV - currUV;
	
	return velocity;
}


float GetFsr3DialogReactiveMask()
{
    return (FF_GSwitches & GSWITCH_FSR3_DIALOG_REACTIVE) != 0 ? 0.30f : 0.0f;
}
//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------
#ifdef FORWARD_PLUS
//--------------------------------------------------------------------------------------
// Forward+ Lit Output
//--------------------------------------------------------------------------------------
FORWARD_PLUS_PS_OUTPUT PSMain( PS_INPUT Input )
{
	FORWARD_PLUS_PS_OUTPUT output;
	// Match Kirides Nightly temporal masks for opaque world geometry.
	output.vTransparencyAndCompositionMask = 0.0f;
	output.vReactiveMask = GetFsr3DialogReactiveMask();

	float2 materialUV = Input.vTexcoord;
#if NORMALMAPPING == 1
	materialUV = parallax_occlusion_mapping(Input.vNormalVS, Input.vViewPosition,
		TX_Displacement, materialUV, SS_Linear, MI_ParallaxOcclusionStrength);
#endif
	float4 color = TX_Texture0.Sample(SS_Linear, materialUV);
	if (MI_MaterialPadding1 > 0.5f && color.a <= (170.0f / 255.0f))
		discard;
	float3 oilLampLightColor = 0.0f;
	float oilLampPaletteCode = 0.0f;
	if (MI_MaterialPadding0 > 0.5f) {
		oilLampLightColor = Input.vEmissiveColor.rgb;
		oilLampPaletteCode = Input.vEmissiveColor.a;
	}

	// clip but only use z approximation
	ClipDistanceEffect(abs(Input.vViewPosition.z), DIST_DrawDistance, color.r * 2 - 1, 500.0f);

#if ALPHATEST == 1
	DoAlphaTest(color.a);
	output.vReactiveMask = max(output.vReactiveMask, 0.10f); // Preserve dialog reactivity; Kirides alpha-test floor.
	output.vTransparencyAndCompositionMask = max(output.vTransparencyAndCompositionMask, 0.10f);
#endif

#if NORMALMAPPING == 1
	float3 nrm = perturb_normal(Input.vNormalVS, Input.vViewPosition, TX_Texture1, materialUV, SS_Linear, MI_NormalmapStrength);
#else
	float3 nrm = normalize(Input.vNormalVS);
#endif

	float4 fx;
#if FXMAP == 1
	fx = TX_Texture2.Sample(SS_Linear, materialUV);
#else
	fx = 1.0f;
#endif

	float specIntensity = MI_SpecularIntensity * fx.r;
	float specPower = MI_SpecularPower * fx.g;
	float vertLighting = Input.vDiffuse.y;
	float twoSidedBacklitMaterial = MI_Color.a < -1.5f ? 1.0f : 0.0f;
	float npcMaterial = (MI_Color.a < -0.5f && MI_Color.a > -2.0f) ? 1.0f : 0.0f;
	// MI_Color.a is also used as a private draw-class marker. Values below
	// -0.1 identify all VOB/NPC pixels for water shoreline handling without
	// changing the existing NPC and two-sided vegetation lighting classes.
	float waterObjectMaterial = MI_Color.a < -0.1f ? 1.0f : 0.0f;
	float alphaTestedMaterial = 0.0f;
#if ALPHATEST == 1
	alphaTestedMaterial = 1.0f;
#endif
	float vegetationBacklitMask = PLS_ComputeBacklitVegetationMask(color.rgb, alphaTestedMaterial, twoSidedBacklitMaterial);
	float vegetationReceiverMask = max(vegetationBacklitMask, alphaTestedMaterial * twoSidedBacklitMaterial);

	float3 vsPosition = Input.vViewPosition;
	float3 wsPosition = mul(float4(vsPosition, 1.0f), WindowCutoutInvView).xyz;
	ClipWindowCutouts(wsPosition, Input.vPosition.xy);
	
	float pixelDistZ = abs(vsPosition.z);

	// CSM shadow source is toggleable in Forward+: precomputed screen-space mask or direct CSM.
	float shadow = vertLighting;
#if SHD_ENABLE
	if (AC_SunVisibility > 0.001f || AC_MoonVisibility > 0.001f)
	{
		#if FP_USE_SHADOW_MASK && ALPHATEST != 1
			if (npcMaterial < 0.5f)
			{
				float2 screenUV = Input.vPosition.xy / FP_ViewportSize;
				shadow = FP_ShadowMask.SampleLevel( SS_Linear, screenUV, 0 ).r;
			}
			else
			{
				float3 wsNormal = normalize(mul(float4(nrm, 0.0f), SQ_InvView).xyz);
				shadow = ComputeCascadedShadowValueCharacter(
					wsPosition, wsNormal, vsPosition.z, vertLighting, Input.vPosition.xy);
			}
		#else
			float3 wsNormal = normalize(mul(float4(nrm, 0.0f), SQ_InvView).xyz);
			if (npcMaterial > 0.5f)
			{
				shadow = ComputeCascadedShadowValueCharacter(
					wsPosition, wsNormal, vsPosition.z, vertLighting, Input.vPosition.xy);
			}
			else
			{
				float3 wsLightDirection = normalize(mul(float4(SQ_LightDirectionVS, 0.0f), SQ_InvView).xyz);
				int cascadeIndex = GetPrimaryCascadeIndex(wsPosition);
				float texelWorldSize = GetCascadeWorldTexelSize(cascadeIndex);
				float3 biasedWsPosition = ApplyReceiverNormalBias(
					wsPosition, wsNormal, wsLightDirection, texelWorldSize, vegetationReceiverMask);
				shadow = ComputeCascadedShadowValueSoft(
					biasedWsPosition, vsPosition.z, vertLighting, 0.0f, Input.vPosition.xy, 0.0f, cascadeIndex);
			}
		#endif
	} else {
		float3 wsNormal = normalize(mul(float4(nrm, 0.0f), SQ_InvView).xyz);
        // Night-time sky ambient:
        // saturate(wsNormal.y) restricts the value to [0, 1].
        // Facing up = 1, Facing sides/down = 0.
        shadow = saturate(wsNormal.y) * vertLighting;
    }
#endif

	float3 litPixel = FP_ComputeSunLighting(wsPosition, vsPosition, nrm, color.rgb, specIntensity, specPower, shadow, vertLighting, twoSidedBacklitMaterial, vegetationBacklitMask);
	
	// Atmospheric scattering affects reflected surface lighting, not light
	// emitted by the lamp itself.
	litPixel = ApplyAtmosphericScatteringGround(wsPosition, litPixel);

	// Keep self-emission independent of day/night atmosphere, matching the
	// additive point-light stage below.
	[branch]
	if (oilLampPaletteCode > 0.0f)
		litPixel += ComputeOilLampEmission(color.rgb, oilLampLightColor);

	// Point lights, only when close enough
	if (pixelDistZ < 6000.0f) 
	{
		litPixel += FP_ComputePointLighting(wsPosition, vsPosition, nrm, float4(color.rgb, Input.vDiffuse.a), specIntensity, specPower, Input.vPosition.xy, twoSidedBacklitMaterial, vegetationBacklitMask);
	}

	output.vColor = float4(litPixel, 1);
	output.vNrm = EncodeNormalGBuffer(nrm);
	float encodedSpecIntensity = MI_Color.a < -1.5f ? -(specIntensity + 3.0f)
		: (MI_Color.a < -0.5f ? -(specIntensity + 1.0f) : specIntensity);
	// Preserve the oil-lamp palette while adding a sign-coded water object
	// marker. World geometry keeps the original non-negative palette encoding;
	// VOB/NPC pixels use -(1 + palette), so zero still means no lamp.
	float encodedWaterMaterial = waterObjectMaterial > 0.5f
		? -(1.0f + oilLampPaletteCode) : oilLampPaletteCode;
	output.vSI_SP = float4(encodedSpecIntensity, specPower, saturate(MI_WetGroundSSRStrength),
		encodedWaterMaterial);
	output.vVelocity = CalculateVelocity(Input.vCurrClipPos, Input.vPrevClipPos);

	return output;
}

#else // !FORWARD_PLUS
//--------------------------------------------------------------------------------------
// Deferred GBuffer Output
//--------------------------------------------------------------------------------------
#if WINDOW_DEPTH_ONLY == 1
void PSMain( PS_INPUT Input )
{
	float3 wsPosition = mul(float4(Input.vViewPosition, 1.0f), WindowCutoutInvView).xyz;
	ClipWindowCutouts(wsPosition, Input.vPosition.xy);
}
DEFERRED_PS_OUTPUT PSMainDISABLED( PS_INPUT Input ) : SV_TARGET
#elif ALPHATEST_SHADOWS == 1
void PSMain( PS_INPUT Input )
{
	float3 wsPosition = mul(float4(Input.vViewPosition, 1.0f), WindowCutoutInvView).xyz;
	ClipWindowCutouts(wsPosition, Input.vPosition.xy);
	float4 color = TX_Texture0.Sample(SS_Linear, Input.vTexcoord);
	if (MI_MaterialPadding1 > 0.5f && color.a <= (170.0f / 255.0f))
		discard;

	// clip but only use z approximation
	ClipDistanceEffect(abs(Input.vViewPosition.z), DIST_DrawDistance, color.r * 2 - 1, 500.0f);

	DoAlphaTest(color.a);
}


// Disable regular shader
DEFERRED_PS_OUTPUT PSMainDISABLED( PS_INPUT Input ) : SV_TARGET
#else
DEFERRED_PS_OUTPUT PSMain( PS_INPUT Input ) : SV_TARGET
#endif
{
	DEFERRED_PS_OUTPUT output;
	// Match Kirides Nightly temporal masks for opaque world geometry.
	output.vTransparencyAndCompositionMask = 0.0f;
	output.vReactiveMask = GetFsr3DialogReactiveMask();

	float3 wsPosition = mul(float4(Input.vViewPosition, 1.0f), WindowCutoutInvView).xyz;
	ClipWindowCutouts(wsPosition, Input.vPosition.xy);

	float2 materialUV = Input.vTexcoord;
#if NORMALMAPPING == 1
	materialUV = parallax_occlusion_mapping(Input.vNormalVS, Input.vViewPosition,
		TX_Displacement, materialUV, SS_Linear, MI_ParallaxOcclusionStrength);
#endif
	float4 color = TX_Texture0.Sample(SS_Linear, materialUV);
	if (MI_MaterialPadding1 > 0.5f && color.a <= (170.0f / 255.0f))
		discard;
	float3 oilLampLightColor = 0.0f;
	float oilLampPaletteCode = 0.0f;
	if (MI_MaterialPadding0 > 0.5f) {
		oilLampLightColor = Input.vEmissiveColor.rgb;
		oilLampPaletteCode = Input.vEmissiveColor.a;
	}
	
	// Do alphatest if wanted
#if ALPHATEST == 1
	// clip but only use z approximation
	ClipDistanceEffect(abs(Input.vViewPosition.z), DIST_DrawDistance, color.r * 2 - 1, 500.0f);
	
	// WorldMesh can always do the alphatest
	DoAlphaTest(color.a);
	output.vReactiveMask = max(output.vReactiveMask, 0.10f); // Preserve dialog reactivity; Kirides alpha-test floor.
	output.vTransparencyAndCompositionMask = max(output.vTransparencyAndCompositionMask, 0.10f);
#endif
	
	// Apply normalmapping if wanted
#if NORMALMAPPING == 1
	float3 nrm = perturb_normal(Input.vNormalVS, Input.vViewPosition, TX_Texture1, materialUV, SS_Linear, MI_NormalmapStrength);
#else
	float3 nrm = normalize(Input.vNormalVS);
#endif
	
	float4 fx;
#if FXMAP == 1
	fx = TX_Texture2.Sample(SS_Linear, materialUV);
#else
	fx = 1.0f;
#endif
	
	output.vDiffuse = float4(color.rgb, Input.vDiffuse.a);
	//output.vDiffuse = float4(Input.vTexcoord2, 0, 1);
	//output.vDiffuse = float4(Input.vNormalVS, 1);
	
	output.vNrm = EncodeNormalGBuffer(nrm);
	
	float deferredSpecIntensity = MI_SpecularIntensity * fx.r;
	float deferredSpecPower = MI_SpecularPower * fx.g;
	// Encode two-sided vegetation and NPC draw classes in the otherwise non-negative
	// specular-intensity channel while preserving the existing vegetation marker.
	output.vSI_SP.x = MI_Color.a < -1.5f ? -(deferredSpecIntensity + 3.0f)
		: (MI_Color.a < -0.5f ? -(deferredSpecIntensity + 1.0f) : deferredSpecIntensity);
#if ALPHATEST == 1
	output.vSI_SP.y = -(deferredSpecPower + 1.0f);
#else
	output.vSI_SP.y = deferredSpecPower;
#endif
	output.vSI_SP.z = saturate(MI_WetGroundSSRStrength);
	float waterObjectMaterial = MI_Color.a < -0.1f ? 1.0f : 0.0f;
	// Keep the linked oil-lamp palette available to deferred lighting while
	// reserving the negative range for direct VOB/NPC exclusion in water.
	output.vSI_SP.w = waterObjectMaterial > 0.5f
		? -(1.0f + oilLampPaletteCode) : oilLampPaletteCode;

	// Calculate velocity for motion vectors
	// For instanced objects (VOBs, skeletal meshes), vCurrClipPos/vPrevClipPos come from VS
	// For world mesh, these will be (0,0,0,0) resulting in zero velocity
	output.vVelocity = CalculateVelocity(Input.vCurrClipPos, Input.vPrevClipPos);
	
	return output;
}
#endif // FORWARD_PLUS

