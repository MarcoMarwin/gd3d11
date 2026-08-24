//--------------------------------------------------------------------------------------
// World/VOB-Pixelshader for G2D3D11 by Degenerated
//--------------------------------------------------------------------------------------

//--------------------------------------------------------------------------------------
// Textures and Samplers
//--------------------------------------------------------------------------------------
SamplerState SS_Linear : register( s0 );
Texture2D	TX_Texture0 : register( t0 );
Texture2D	TX_WindowSceneDepth : register( t14 );

struct FFData {
	float4 textureFactor;
	float2 windowSkyParams;
	float2 padding;
};

cbuffer cbFFData : register( b0 ) {
	FFData cbFFData;
};

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
	float4 vCurrClipPos     : TEXCOORD6;
	float4 vPrevClipPos     : TEXCOORD7;
	float4 vPosition		: SV_POSITION;
};

//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------
float4 PSMain( PS_INPUT Input ) : SV_TARGET
{
	float4 color = TX_Texture0.Sample(SS_Linear, Input.vTexcoord);

#ifdef USE_FFDATA
	// A negative factor alpha selects the transparent portion of the scoped
	// City_Window replacement. Its solid texels already ran through the lit pass.
	if (cbFFData.textureFactor.a < 0.0f)
	{
		if (color.a > (170.0f / 255.0f))
			discard;

		// The normal post-processed path uses the replacement texture as a
		// deliberately subtle glass overlay. With both HDR tone mapping and AA
		// disabled, however, there is no scene-space post-processing pass to
		// balance that dark overlay. Keep the pane genuinely transparent in that
		// direct scene-to-gamma path; the opaque frame was already rendered by the
		// main alpha-test pass and is not affected here.
		const bool directSceneToGammaPath = cbFFData.padding.x > 0.5f;
		if (directSceneToGammaPath)
		{
			// There is no transparent glass contribution in the direct
			// scene-to-gamma path. Discard instead of merely returning alpha 0,
			// so this pass cannot darken the pane even if a stale/non-alpha blend
			// state is present. The opaque frame was already rendered by the main
			// alpha-test pass.
			discard;
		}
		else
		{
			const float softenedAlpha = color.a * 0.82f;
			color.a = max(softenedAlpha, 0.16f);
		}

		if (!directSceneToGammaPath && cbFFData.windowSkyParams.y > 0.5f)
		{
			const float skyCutoff = cbFFData.windowSkyParams.x;

			// Only the actual pane pixel becomes opaque when it covers sky in the
			// lower screen third. This is screen-space by design and therefore does
			// not depend on camera pitch or on the window texture's pane layout.
			if (Input.vPosition.y >= skyCutoff)
			{
				const int2 pixelPosition = int2(Input.vPosition.xy);
				const bool coversSky = TX_WindowSceneDepth.Load(
					int3(pixelPosition, 0)).r <= 1e-7f;
				if (coversSky)
					color.a = 1.0f;
			}
		}

		// The IN and OUT meshes carry unrelated vertex lighting; IN variants can
		// even be black. Use the renderer's day/night glass factor so the same
		// authored pane remains visible from either side.
		color.rgb *= cbFFData.textureFactor.rgb;
		return color;
	}
#endif

	color *= Input.vDiffuse;
#ifdef USE_FFDATA
	color *= cbFFData.textureFactor;
#endif
	return color;
}

