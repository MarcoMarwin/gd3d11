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
float EvaluateWindowSkyPath(int2 pixelPosition, int2 targetSize, float halfHeight,
	int horizontalOffset)
{
	const int laneX = clamp(pixelPosition.x + horizontalOffset, 0, targetSize.x - 1);

	// Keep the 24-sample coverage of the original safeguard, but do not unroll or
	// finish a path whose result is already known. Most lanes encounter either a
	// nearby wall blocker or upper-screen sky after only a few iterations. The
	// previous branchless/unrolled form always issued every depth/mask load for
	// all three lanes of every affected glass pixel.
	[loop]
	for (int stepIndex = 1; stepIndex <= 24; ++stepIndex)
	{
		const float t = float(stepIndex) * (1.0f / 24.0f);
		const int sampleY = clamp(int(lerp(float(pixelPosition.y), 0.0f, t)),
			0, targetSize.y - 1);
		const int2 samplePosition = int2(laneX, sampleY);

		// Only static world geometry closes a path. The mask is deliberately not
		// bound while VOBs and NPCs render, so they cannot interrupt it.
		const float worldBlocker = TX_WindowWorldMask.Load(
			int3(samplePosition, 0)).r;
		if (worldBlocker >= 0.5f)
			return 0.0f;

		if (sampleY < halfHeight)
		{
			const float sampleDepth = TX_WindowSceneDepth.Load(
				int3(samplePosition, 0)).r;
			if (sampleDepth <= 1e-7f)
				return 1.0f;
		}
	}

	return 0.0f;
}

float EvaluateCachedWindowSkyVisibility(
	int2 pixelPosition, int2 targetSize, float featherWidth)
{
	uint maskWidth;
	uint maskHeight;
	TX_WindowSkyVisibility.GetDimensions(maskWidth, maskHeight);
	if (maskWidth == 0u || maskHeight == 0u)
		return 0.0f;

	const float2 uv = (float2(pixelPosition) + 0.5f) / float2(targetSize);
	const float horizontalOffset = featherWidth / float(targetSize.x);
	const float2 halfTexel = 0.5f / float2(maskWidth, maskHeight);
	const float2 centerUV = clamp(uv, halfTexel, 1.0f - halfTexel);
	const float center = TX_WindowSkyVisibility.SampleLevel(
		SS_Linear, centerUV, 0.0f).r;
	const float left = TX_WindowSkyVisibility.SampleLevel(
		SS_Linear, clamp(centerUV - float2(horizontalOffset, 0.0f),
			halfTexel, 1.0f - halfTexel), 0.0f).r;
	const float right = TX_WindowSkyVisibility.SampleLevel(
		SS_Linear, clamp(centerUV + float2(horizontalOffset, 0.0f),
			halfTexel, 1.0f - halfTexel), 0.0f).r;
	return saturate((left + center * 2.0f + right) * 0.25f);
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

		if (cbFFData.windowParams.y > 0.5f)
		{
			const float halfHeight = cbFFData.windowParams.x;
			if (Input.vPosition.y >= halfHeight)
			{
				uint targetWidth;
				uint targetHeight;
				TX_WindowSceneDepth.GetDimensions(targetWidth, targetHeight);
				const int2 targetSize = int2(targetWidth, targetHeight);
				const int2 pixelPosition = clamp(int2(Input.vPosition.xy),
					int2(0, 0), targetSize - 1);
				const float sceneDepth = TX_WindowSceneDepth.Load(
					int3(pixelPosition, 0)).r;
				const float currentWorldBlocker = TX_WindowWorldMask.Load(
					int3(pixelPosition, 0)).r;
				// Keep a full-resolution final check around the reduced mask. It
				// prevents bilinear feathering from crossing an actual world pixel.
				if (sceneDepth <= 1e-7f && currentWorldBlocker < 0.5f)
				{
					const float featherWidth = clamp(
						float(min(targetWidth, targetHeight)) * 0.01f, 8.0f, 20.0f);
					float pathConfidence;
					[branch]
					if (cbFFData.windowParams.z > 0.5f)
					{
						pathConfidence = EvaluateCachedWindowSkyVisibility(
							pixelPosition, targetSize, featherWidth);
					}
					else
					{
						// Feature-level-10 fallback: retain the exact full-resolution
						// path test when compute/UAV support is unavailable.
						const float centerPath = EvaluateWindowSkyPath(
							pixelPosition, targetSize, halfHeight, 0);
						const float leftPath = EvaluateWindowSkyPath(
							pixelPosition, targetSize, halfHeight, -int(featherWidth));
						const float rightPath = EvaluateWindowSkyPath(
							pixelPosition, targetSize, halfHeight, int(featherWidth));
						pathConfidence =
							(leftPath + centerPath * 2.0f + rightPath) * 0.25f;
					}
					const float validTransparency = smoothstep(
						0.16f, 0.84f, pathConfidence);
					const float lowerHalfFade = smoothstep(
						halfHeight, halfHeight + featherWidth, Input.vPosition.y);
					color.a = lerp(color.a, 1.0f,
						(1.0f - validTransparency) * lowerHalfFade
							* saturate(cbFFData.windowParams.w));
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

