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

float IsBlockedWindowSky(int2 pixelPosition, int2 targetSize, float skyCutoff)
{
	pixelPosition = clamp(pixelPosition, int2(0, 0), targetSize - 1);
	if (pixelPosition.y < skyCutoff)
		return 0.0f;

	return TX_WindowSceneDepth.Load(int3(pixelPosition, 0)).r <= 1e-7f
		? 1.0f : 0.0f;
}

float2 WindowUvDeltaToScreenDelta(float2 uvDelta, float2 uvDx, float2 uvDy)
{
	const float determinant = uvDx.x * uvDy.y - uvDy.x * uvDx.y;
	if (abs(determinant) <= 1e-8f)
		return float2(0.0f, 0.0f);

	return float2(
		(uvDelta.x * uvDy.y - uvDelta.y * uvDy.x) / determinant,
		(uvDelta.y * uvDx.x - uvDelta.x * uvDx.y) / determinant);
}

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
		// Derivatives must be evaluated before the alpha discard so the opaque
		// lattice cannot invalidate neighboring pane-row projections.
		const float2 uvDx = ddx(Input.vTexcoord);
		const float2 uvDy = ddy(Input.vTexcoord);

		if (color.a > (170.0f / 255.0f))
			discard;

		// Soften overly opaque authored panes but retain a firm visibility floor.
		// This keeps glass present over sky, world geometry, decals and effects
		// without making it read as a bright opaque sheet.
		color.a = max(color.a * 0.82f, 0.16f);

		if (cbFFData.windowSkyParams.y > 0.5f)
		{
			const float skyCutoff = cbFFData.windowSkyParams.x;
			uint targetWidth;
			uint targetHeight;
			TX_WindowSceneDepth.GetDimensions(targetWidth, targetHeight);
			const int2 targetSize = int2(targetWidth, targetHeight);

			// City_Window.dds has five pane rows separated by an opaque lattice.
			// If a row sees sky in the lower screen third, that complete row and
			// every row below it become opaque. Three samples cover its three panes.
			const int currentRow = clamp((int)(saturate(Input.vTexcoord.y) * 5.0f), 0, 4);
			const float paneColumnCenters[3] = {
				1.0f / 6.0f, 3.0f / 6.0f, 5.0f / 6.0f
			};
			const float paneRowOffsets[2] = { 0.3f, 0.7f };
			float rowSkyVisible = 0.0f;
			for (int row = 0; row < 5; ++row)
			{
				if (row > currentRow || rowSkyVisible > 0.5f)
					break;

				for (int column = 0; column < 3; ++column)
				{
					for (int rowSample = 0; rowSample < 2; ++rowSample)
					{
						const float sampleRow = (row + paneRowOffsets[rowSample]) / 5.0f;
						const float2 sampleUv = float2(paneColumnCenters[column], sampleRow);
						const float2 samplePosition = Input.vPosition.xy
							+ WindowUvDeltaToScreenDelta(sampleUv - Input.vTexcoord, uvDx, uvDy);
						rowSkyVisible = max(rowSkyVisible, IsBlockedWindowSky(
							int2(samplePosition), targetSize, skyCutoff));
						if (rowSkyVisible > 0.5f)
							break;
					}
					if (rowSkyVisible > 0.5f)
						break;
				}
			}
			if (rowSkyVisible > 0.5f)
				color.a = 1.0f;
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

