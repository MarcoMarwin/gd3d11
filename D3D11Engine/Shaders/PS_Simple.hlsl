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

float2 WindowUvToScreenPosition(
	float2 targetUv, float2 uvOverW, float reciprocalW,
	float2 uvOverWDx, float2 uvOverWDy,
	float reciprocalWDx, float reciprocalWDy,
	float2 screenPosition)
{
	// Perspective-correct UV is A/B, where A = UV/W and B = 1/W are
	// screen-linear. Reconstruct that homography so every pixel of the planar
	// window projects a row sample to exactly the same screen position.
	const float2 equationX = uvOverWDx - targetUv * reciprocalWDx;
	const float2 equationY = uvOverWDy - targetUv * reciprocalWDy;
	const float2 rightHandSide = targetUv * reciprocalW - uvOverW;
	const float determinant = equationX.x * equationY.y
		- equationX.y * equationY.x;
	if (abs(determinant) <= 1e-8f)
		return screenPosition;

	const float2 screenDelta = float2(
		(rightHandSide.x * equationY.y - equationX.y * rightHandSide.y)
			/ determinant,
		(equationX.x * rightHandSide.y - rightHandSide.x * equationY.x)
			/ determinant);
	return screenPosition + screenDelta;
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
		// Evaluate derivatives while all lanes are active. The following discard
		// removes the opaque lattice and must not invalidate pane-row projection.
		const float reciprocalW = Input.vPosition.w;
		const float2 uvOverW = Input.vTexcoord * reciprocalW;
		const float2 uvOverWDx = ddx(uvOverW);
		const float2 uvOverWDy = ddy(uvOverW);
		const float reciprocalWDx = ddx(reciprocalW);
		const float reciprocalWDy = ddy(reciprocalW);

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
			// every row below it become opaque. Two fixed samples cover each pane.
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
						const float2 samplePosition = WindowUvToScreenPosition(
							sampleUv, uvOverW, reciprocalW,
							uvOverWDx, uvOverWDy, reciprocalWDx, reciprocalWDy,
							Input.vPosition.xy);
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

