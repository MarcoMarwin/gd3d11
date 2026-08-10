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

struct FFData {
	float4 textureFactor;
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
	float horizontalOffset)
{
	float pathOpen = 1.0f;
	float reachedUpperSky = 0.0f;
	const int laneX = clamp(pixelPosition.x + int(horizontalOffset), 0, targetSize.x - 1);

	// A bounded vertical probe is the deliberate compromise here: it is local to
	// City_Window glass pixels and avoids a full-screen connected-component pass.
	// Eight probes left gaps of more than a hundred pixels at 1080p and could
	// jump over a wall strip. Twenty-four is still bounded and restricted to
	// lower-half sky pixels of the four supported window VOBs.
	[unroll]
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
		pathOpen *= 1.0f - step(0.5f, worldBlocker);

		if (sampleY < halfHeight)
		{
			const float sampleDepth = TX_WindowSceneDepth.Load(
				int3(samplePosition, 0)).r;
			const float sampleIsSky = 1.0f - step(1e-7f, sampleDepth);
			reachedUpperSky = max(reachedUpperSky, pathOpen * sampleIsSky);
		}
	}

	return reachedUpperSky;
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

		// Preserve the authored glass layer even in panes whose DDS alpha reaches
		// zero. Without this floor the sky was visually unobstructed although the
		// frame/grid still rendered correctly.
		color.a = max(color.a, 0.18f);

		// A factor below -1 enables the City_Window sky safeguard and its
		// magnitude carries the fixed screen midpoint in render-target pixels.
		if (cbFFData.textureFactor.a < -1.5f)
		{
			const float halfHeight = -cbFFData.textureFactor.a;
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
				if (sceneDepth <= 1e-7f)
				{
					const float featherWidth = clamp(
						float(min(targetWidth, targetHeight)) * 0.01f, 8.0f, 20.0f);
					const float centerPath = EvaluateWindowSkyPath(
						pixelPosition, targetSize, halfHeight, 0.0f);
					const float leftPath = EvaluateWindowSkyPath(
						pixelPosition, targetSize, halfHeight, -featherWidth);
					const float rightPath = EvaluateWindowSkyPath(
						pixelPosition, targetSize, halfHeight, featherWidth);
					// The pixel's own vertical path is authoritative. Neighboring
					// lanes only soften the horizontal edge; they must never make an
					// isolated lower-half sky pixel transparent on their own.
					const float pathConfidence = centerPath * 0.75f
						+ (leftPath + rightPath) * 0.125f;
					const float validTransparency = smoothstep(
						0.20f, 0.80f, pathConfidence);
					const float lowerHalfFade = smoothstep(
						halfHeight, halfHeight + featherWidth, Input.vPosition.y);
					color.a = lerp(color.a, 1.0f,
						(1.0f - validTransparency) * lowerHalfFade);
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

