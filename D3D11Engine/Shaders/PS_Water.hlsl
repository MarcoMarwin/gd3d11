//--------------------------------------------------------------------------------------
// World/VOB-Pixelshader for G2D3D11 by Degenerated
//--------------------------------------------------------------------------------------
#include <AtmosphericScattering.h>
#include <FFFog.h>
#include <DS_Defines.h>
#include <DepthReconstruction.h>
#include <SSR.h>

static const float DIST_SMALL_SPEED = -0.01f;
static const float DIST_SMALL_AMOUNT = 0.01f;
static const float DIST_SMALL_SCALE = 0.3f;
static const float DIST_BIG_SCALE = 0.1f;
static const float DIST_BIG_SPEED = -0.005f;


// Cleans the refraction borders
#define CleanRefraction(uv, screen_uv, depthRef) (lerp(uv, screen_uv, saturate(Input.vTexcoord2.x-depthRef)))

cbuffer RefractionInfo : register( b2 )
{
	float4x4 RI_Projection;
	float2 RI_ViewportSize;
	float RI_Time;
	float RI_Pad1;

	float3 RI_CameraPosition;
	float RI_SSRIntensity; // reserved/unused (SSR quality is a compile-time permutation)

	float4x4 RI_View; // World->view, for screen-space reflections
};

//--------------------------------------------------------------------------------------
// Textures and Samplers
//--------------------------------------------------------------------------------------
SamplerState SS_Linear : register( s0 );
SamplerState SS_samMirror : register( s1 );
Texture2D	TX_Diffuse : register( t0 );

Texture2D	TX_Depth : register( t2 );
TextureCube	TX_ReflectionCube : register( t3 );
Texture2D	TX_Distortion : register( t4 );
Texture2D	TX_Scene : register( t5 );

//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
struct PS_INPUT
{
	float2 vTexcoord		: TEXCOORD0;
	float2 vTexcoord2		: TEXCOORD1;
	float4 vDiffuse			: TEXCOORD2;
	float3 vNormalWS		: TEXCOORD4;
	float3 vWorldPosition	: TEXCOORD5;
	float4 vPosition		: SV_POSITION;
};


//--------------------------------------------------------------------------------------
// Screen-space reflections
// Shared Nightly-style SSR hit logic is implemented in SSR.h and used by both water and
// WetGroundSSR. Water stays in this pixel shader and only delegates the hit search.
//--------------------------------------------------------------------------------------
#define WATER_SSR_MAX_STEPS 24
#define WATER_SSR_REFINE_STEPS 5
#define WATER_SSR_MAX_DISTANCE 30000.0f
#define WATER_SSR_THICKNESS 350.0f
#define WATER_SSR_START_BIAS 2.0f
#define WATER_SSR_EDGE_FADE 0.001f

float3 TraceWaterSSR(float3 worldPos, float3 reflectDirWS, out float confidence)
{
    confidence = 0.0f;

    float4x4 waterViewProj = mul(RI_View, RI_Projection);
    float3 originVS = mul(float4(worldPos, 1.0f), RI_View).xyz;
    float startBias = max(WATER_SSR_START_BIAS, originVS.z * 0.002f);

    SSRTraceResult trace = SSRCore_TraceWorldRay(
        TX_Depth,
        worldPos, reflectDirWS,
        waterViewProj, RI_ViewportSize,
        RI_Projection._43, RI_Projection._33,
        WATER_SSR_MAX_DISTANCE, WATER_SSR_MAX_STEPS, WATER_SSR_REFINE_STEPS,
        startBias, WATER_SSR_THICKNESS, WATER_SSR_EDGE_FADE);

    confidence = trace.confidence;

    if (trace.hit <= 0.0f || trace.confidence <= 0.0f)
        return float3(0.0f, 0.0f, 0.0f);

    return TX_Scene.Sample(SS_Linear, trace.hitUV).rgb;
}

//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------
float4 PSMain( PS_INPUT Input ) : SV_TARGET
{
	float2 screenUV = Input.vPosition.xy / RI_ViewportSize;

	// Linear depth
	float depth = TX_Depth.Sample(SS_Linear, screenUV).r;
	depth = RI_Projection._43 / (depth - RI_Projection._33);

	// Clip here so we don't have to bind the depthbuffer
	//if(depth < Input.vTexcoord2.x) // vTexcoord2-x stores viewspace.z
	//	discard;

	float shallowDepth = saturate((depth - Input.vTexcoord2.x) * 0.01f);

	// Camera direction
	float3 viewDirection = normalize(Input.vWorldPosition - RI_CameraPosition);

	// Calculate distortion vectors
	float2 worldTexCoord = Input.vWorldPosition.xz / 1000.0f;
	float3 distortionSmall = TX_Distortion.Sample(SS_Linear, worldTexCoord * DIST_SMALL_SCALE + RI_Time * DIST_SMALL_SPEED).xyz * 2 - 1;
	distortionSmall += TX_Distortion.Sample(SS_Linear, worldTexCoord * float2(-1,0.7) * DIST_SMALL_SCALE + RI_Time * DIST_SMALL_SPEED * 2).xyz * 2 - 1;
	distortionSmall *= 0.5f;

	float3 distortionBig = TX_Distortion.Sample(SS_Linear, worldTexCoord * DIST_BIG_SCALE + RI_Time * DIST_BIG_SPEED).xyz * 2 - 1;
	distortionBig += TX_Distortion.Sample(SS_Linear, worldTexCoord * float2(-1,0.7) * DIST_BIG_SCALE + RI_Time * DIST_BIG_SPEED * 1.2).xyz * 2 - 1;
	distortionBig *= 0.5f;

	float2 distUV = screenUV + distortionSmall.xy * DIST_SMALL_AMOUNT + distortionBig.xy * DIST_SMALL_AMOUNT;

	// Distorted diffuse
	float3 diffuse = TX_Diffuse.Sample(SS_Linear, Input.vTexcoord + distortionSmall.xy * DIST_SMALL_AMOUNT * 0.5f).rgb;

	// Refracted depth
	float depthRefracted = TX_Depth.Sample(SS_Linear, distUV).r;
	depthRefracted = RI_Projection._43 / (depthRefracted - RI_Projection._33);

	distUV = CleanRefraction(distUV, screenUV, depthRefracted);
	distUV = saturate(distUV);

	// Wave vector
	float3 wavesDist = normalize(distortionSmall.xzy * float3(1,100,1));
	float3 wavesFres = normalize(distortionBig.xzy * float3(1,10,1));

	// Scene color
	float3 scene = TX_Scene.Sample(SS_Linear, distUV).rgb;
	float3 sceneClean = TX_Scene.Sample(SS_Linear, lerp(distUV, screenUV, pow(1-shallowDepth, 20.0f))).rgb;

	// Fresnel from waves
	float fresnel = min(0.5f, saturate(pow(1.0f - saturate(dot(-viewDirection, wavesFres)), 10.0f)));

	// Reflection
	float3 reflect_vec = reflect(-viewDirection, wavesFres);

	// sample reflection cube (fallback for off-screen / missed rays)
	float3 reflection = TX_ReflectionCube.Sample(SS_Linear, reflect_vec).xyz;
	float ssrConfidence = 0.0f;
	float3 ssrColor = float3(0.0f, 0.0f, 0.0f);
	{
		// reflect_vec above is negated (reflect(-viewDirection,N)) for the cube lookup.
		// The true eye-reflection direction, which marches UP into the scene, is
		// reflect(viewDirection, N). Flatten the wave normal so reflection rays stay
		// coherent (mirror-like) instead of scattering into many off-screen misses.
		float3 ssrNormal = normalize(lerp(float3(0.0f, 1.0f, 0.0f), wavesFres, 0.5f));
		float3 ssrDir = reflect(viewDirection, ssrNormal);
		// Screen-space reflection of nearby on-screen geometry; falls back to the cube on a miss.
		ssrColor = TraceWaterSSR(Input.vWorldPosition, ssrDir, ssrConfidence);
		reflection = lerp(reflection, ssrColor, saturate(ssrConfidence));
	}

	// Darken the scene, to make a wet surface
	float f = 1-saturate(pow(1-shallowDepth, 8.0f) + clamp(pow(distortionSmall.y, 2), 0.5f, 1.0f));

	float3 sceneWet = lerp(sceneClean, sceneClean * 0.01f, f); // Darken border-scene
	scene = lerp(scene, scene * float3(4, 0.2f, 0.1f) * 0.05f, f); // Darken distorted scene

	float pxDistance = Input.vTexcoord2.y;
	scene = lerp(scene, diffuse, 0.73f * max(pow(fresnel,8.0f), 0.5f));
	float3 color = lerp(scene, sceneClean, pow(saturate(pxDistance / 35000.0f), 4.0f));
	color = lerp(color, sceneWet, (1-shallowDepth));

	// Reflection compositing.
	// Fresnel (view angle) is the primary driver of how much reflection shows, same as
	// real water: looking straight down mostly shows the water body's own color, looking
	// at a grazing angle mostly shows the reflection. ssrConfidence only picks *which*
	// reflection source to use (real on-screen geometry vs the static cube, chosen above
	// at line 284) and gives it a modest boost - it must not override the angle-based
	// blend entirely, or the water reads as a flat mirror regardless of how you're
	// looking at it.
	float NdotV = saturate(dot(-viewDirection, wavesFres));
	float reflectFresnel = pow(1.0f - NdotV, 3.0f);

	// Waterfalls (surface normal pointing mostly sideways rather than up) get a
	// strong, distracting reflection because the geometry is nearly vertical while
	// the shader still treats it like flat, horizontal water. Use the true geometric
	// normal (not the wave-perturbed one) to detect this and fade the reflection out.
	float waterfallFactor = 1.0f - saturate(abs(normalize(Input.vNormalWS).y));
	float reflectSuppress = lerp(1.0f, 0.12f, waterfallFactor);

	float reflectAmount = saturate(lerp(0.35f, 1.0f, reflectFresnel) * lerp(0.5f, 1.0f, saturate(ssrConfidence)) * reflectFresnel) * reflectSuppress;
	color = lerp(color, reflection * lerp(1.0f, diffuse, 0.6f), reflectAmount);

	color.rgb = ApplyAtmosphericScatteringGround(Input.vWorldPosition, color.rgb);

	// Do spec lighting
	float3 sunOrange = float3(0.6,0.3,0.1) * 2.0f;
	float3 sunColor = lerp(sunOrange, 1.0f, AC_LightPos.y) * 5.0f;

	float3 reflect_vecSmall = reflect(-viewDirection, normalize(distortionSmall.xzy * float3(1,10,1)));

	float cos_spec = clamp(dot(reflect_vecSmall, -AC_LightPos.xyz * float3(1,1,1)), 0, 1);
	float sun_spot = pow(cos_spec, 500.0f) * 0.5f;
	color.rgb += lerp(sunColor * sun_spot, float3(0.0f, 0.0f, 0.0f), step(step(0.0f, AC_LightPos.y) * Input.vDiffuse.y, 0.5f));

	//darken / lighten water based on the day / night cycle
	float darknessFactor = 2.0f;
	darknessFactor -= AC_LightPos.y;

	return float4(color / darknessFactor, 1);
}
