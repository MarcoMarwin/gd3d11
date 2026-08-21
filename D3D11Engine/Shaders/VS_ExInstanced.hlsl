//--------------------------------------------------------------------------------------
// Simple vertex shader
//--------------------------------------------------------------------------------------

#include "Globals_VS_ExConstants.h"

cbuffer Matrices_PerFrame : register( b0 )
{
	VS_ExConstantBuffer_PerFrame frame;
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
	float4x4 InstanceWorldViewMatrix : INSTANCE_WORLD_MATRIX;
	float4 vInstanceColor : INSTANCE_COLOR;
	float2 vInstanceScale : INSTANCE_SCALE;
	
};

struct VS_OUTPUT
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
// Vertex Shader
//--------------------------------------------------------------------------------------
VS_OUTPUT VSMain( VS_INPUT Input )
{
	VS_OUTPUT Output;
	float4 localPosition = float4(Input.vPosition * float3(Input.vInstanceScale, 1) * 2.0f, 1.0f);
	float4 viewPosition = mul(localPosition, Input.InstanceWorldViewMatrix);

	Output.vPosition = mul(viewPosition, frame.M_Proj);
	Output.vTexcoord2 = Input.vTex1;
	Output.vTexcoord = Input.vTex1;
	Output.vDiffuse  = float4(Input.vInstanceColor.gba, pow(Input.vInstanceColor.r, 2.2f));
	Output.vNormalVS = float3(0,0,0);//mul(Input.vNormal, (float3x3)mul(Input.InstanceWorldViewMatrix, frame.M_View));
	Output.vViewPosition = float3(0,0,0);//mul(float4(Input.vPosition,1), mul(Input.InstanceWorldViewMatrix, frame.M_View));
	
	// These instances have no previous object transform, but they are static in
	// world space. Recover that position from the supplied world-view matrix so
	// camera motion is still represented correctly for FSR3 reprojection.
	float4 worldPosition = mul(viewPosition, frame.M_InvView);
	Output.vCurrClipPos = mul(worldPosition, frame.M_UnjitteredViewProj);
	Output.vPrevClipPos = mul(worldPosition, frame.M_PrevViewProj);
	
	return Output;
}

