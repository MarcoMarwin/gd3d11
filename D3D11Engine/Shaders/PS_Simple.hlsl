//--------------------------------------------------------------------------------------
// World/VOB-Pixelshader for G2D3D11 by Degenerated
//--------------------------------------------------------------------------------------

//--------------------------------------------------------------------------------------
// Textures and Samplers
//--------------------------------------------------------------------------------------
SamplerState SS_Linear : register( s0 );
Texture2D	TX_Texture0 : register( t0 );
Texture2D	TX_WindowSceneDepth : register( t14 );
Texture2D	TX_WindowWorldMask : register( t15 );
Texture2D	TX_WindowSkyVisibility : register( t16 );

struct FFData {
	float4 textureFactor;
	float4 windowParams;
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

#ifdef USE_FFDATA
float EvaluateCachedWindowSkyVisibility(
	int2 pixelPosition, int2 targetSize, float featherWidth)
{
	uint maskWidth;
	uint maskHeight;
	TX_WindowSkyVisibility.GetDimensions(maskWidth, maskHeight);
	if (maskWidth == 0u || maskHeight == 0u)
		return 0.0f;

	const float2 uv = (float2(pixelPosition) + 0.5f) / float2(targetSize);
	const float2 featherOffset = featherWidth / float2(targetSize);
	const float2 halfTexel = 0.5f / float2(maskWidth, maskHeight);
	const float2 centerUV = clamp(uv, halfTexel, 1.0f - halfTexel);
	const float center = TX_WindowSkyVisibility.SampleLevel(
		SS_Linear, centerUV, 0.0f).r;
	const float left = TX_WindowSkyVisibility.SampleLevel(
		SS_Linear, clamp(centerUV - float2(featherOffset.x, 0.0f),
			halfTexel, 1.0f - halfTexel), 0.0f).r;
	const float right = TX_WindowSkyVisibility.SampleLevel(
		SS_Linear, clamp(centerUV + float2(featherOffset.x, 0.0f),
			halfTexel, 1.0f - halfTexel), 0.0f).r;
	const float up = TX_WindowSkyVisibility.SampleLevel(
		SS_Linear, clamp(centerUV - float2(0.0f, featherOffset.y),
			halfTexel, 1.0f - halfTexel), 0.0f).r;
	const float down = TX_WindowSkyVisibility.SampleLevel(
		SS_Linear, clamp(centerUV + float2(0.0f, featherOffset.y),
			halfTexel, 1.0f - halfTexel), 0.0f).r;
	// A compact five-tap cross softens and slightly expands the protected
	// disconnected region without returning to the old per-pixel path walk.
	return saturate((left + right + up + down + center * 2.0f) * (1.0f / 6.0f));
}
#endif


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

		// Soften overly opaque authored panes but retain a firm visibility floor.
		// This keeps glass present over sky, world geometry, decals and effects
		// without making it read as a bright opaque sheet.
		color.a = max(color.a * 0.82f, 0.16f);

		if (cbFFData.windowParams.y > 0.5f
			&& cbFFData.windowParams.z > 0.5f)
		{
			const float halfHeight = cbFFData.windowParams.x;
			uint targetWidth;
			uint targetHeight;
			TX_WindowSceneDepth.GetDimensions(targetWidth, targetHeight);
			// Feather only the cached connected/disconnected boundary. The fixed
			// screen centre remains the classification boundary, but clear sky just
			// below it stays transparent when it is connected to upper-screen sky.
			const float featherWidth = clamp(
				float(min(targetWidth, targetHeight)) * 0.04f, 32.0f, 72.0f);
			if (Input.vPosition.y >= halfHeight)
			{
				const int2 targetSize = int2(targetWidth, targetHeight);
				const int2 pixelPosition = clamp(int2(Input.vPosition.xy),
					int2(0, 0), targetSize - 1);
				const float sceneDepth = TX_WindowSceneDepth.Load(
					int3(pixelPosition, 0)).r;
				if (sceneDepth <= 1e-7f)
				{
					const float connectedSky = EvaluateCachedWindowSkyVisibility(
						pixelPosition, targetSize, featherWidth);
					const float disconnectedSky = 1.0f - connectedSky;
					color.a = lerp(color.a, 1.0f,
						disconnectedSky * saturate(cbFFData.windowParams.w));
				}
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
	//return float4(1,0,0,1);
	
	return color;
}

