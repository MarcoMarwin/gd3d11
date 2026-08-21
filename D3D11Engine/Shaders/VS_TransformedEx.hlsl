//--------------------------------------------------------------------------------------
// Simple vertex shader
//--------------------------------------------------------------------------------------

#ifndef OVERRIDE_MAX_Z
#define OVERRIDE_MAX_Z 0
#endif

cbuffer Viewport : register( b0 )
{
	float2 V_ViewportPos;
	float2 V_ViewportSize;
};


//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
struct VS_INPUT
{
	float3 vPosition	: POSITION;
	float3 vNormal		: NORMAL;
	float2 vTex1		: TEXCOORD0;
	float2 vTex2		: TEXCOORD1;
	float4 vDiffuse		: DIFFUSE;
};

struct VS_OUTPUT
{
	float2 vTexcoord		: TEXCOORD0;
	float2 vTexcoord2		: TEXCOORD1;
	float4 vDiffuse			: TEXCOORD2;
	float3 vViewPosition 	: TEXCOORD3;
	float3 vNormalVS		: TEXCOORD4;
	float3 vWorldPosition	: TEXCOORD5;
	float4 vPosition		: SV_POSITION;
};

/** Transforms a pre-transformed xyzrhw-coordinate into d3d11-space */
float4 TransformXYZRHW(float4 xyzrhw)
{
	// Convert from viewport-coordinates to normalized device coordinates
	float3 ndc;
	ndc.x = ((2 * (xyzrhw.x - V_ViewportPos.x)) / V_ViewportSize.x) - 1;
	ndc.y = 1 - ((2 * (xyzrhw.y - V_ViewportPos.y)) / V_ViewportSize.y);
	
#ifdef OVERRIDE_MAX_Z
	ndc.z = 0; // for sky we need to override this, so that the sky dome is properly depth clipped if behind geometry.
#else
	ndc.z = xyzrhw.z;
#endif
	
	// Convert to clip space. RHW is the reciprocal of W.
	float actualW = 1.0f / xyzrhw.w;
	float3 clipSpace = ndc.xyz * actualW;
	
	return float4(clipSpace, actualW);
}

//--------------------------------------------------------------------------------------
// Vertex Shader
//--------------------------------------------------------------------------------------
VS_OUTPUT VSMain( VS_INPUT Input )
{
	VS_OUTPUT Output;
	
	Output.vPosition = TransformXYZRHW(float4(Input.vPosition, Input.vNormal.x)); // rhw is stored in normal.x
	
	Output.vTexcoord2 = Input.vTex1;
	Output.vTexcoord = Input.vTex1;
	Output.vDiffuse  = Input.vDiffuse;
	Output.vNormalVS = float3(0,0,0);//mul(Input.vNormal, (float3x3)M_WorldView);
	Output.vViewPosition = float3(0,0,0);//mul(float4(Input.vPosition,1), M_WorldView);
	Output.vWorldPosition = Input.vPosition;
	
	return Output;
}

