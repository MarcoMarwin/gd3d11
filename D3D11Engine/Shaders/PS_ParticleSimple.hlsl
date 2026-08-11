//--------------------------------------------------------------------------------------
// Non-additive particle pixel shader with scene lighting adaptation.
//--------------------------------------------------------------------------------------
#include <AtmosphericScattering.h>
SamplerState SS_Linear : register( s0 );
Texture2D TX_Texture0 : register( t0 );
#ifdef USE_FFDATA
struct FFData {
    float4 textureFactor;
};
cbuffer cbFFData : register( b0 ) {
    FFData cbFFData;
};
#endif
struct PS_INPUT
{
    float2 vTexcoord        : TEXCOORD0;
    float2 vTexcoord2       : TEXCOORD1;
    float4 vDiffuse         : TEXCOORD2;
    float3 vNormalVS        : TEXCOORD4;
    float3 vViewPosition    : TEXCOORD5;
    float4 vCurrClipPos     : TEXCOORD6;
    float4 vPrevClipPos     : TEXCOORD7;
    float vParticleLightingScale : TEXCOORD8;
    float4 vPosition        : SV_POSITION;
};
float4 AdaptParticleLighting(float4 color, float particleLightingScale)
{
    if (particleLightingScale < 0.0f)
        return color;
    float packedParticleTestFlags = floor(max(AC_Pad3, 0.0f) + 0.5f);
    bool disableParticleNightDimming = fmod(floor(packedParticleTestFlags / 1.0f), 2.0f) >= 1.0f;
    bool disableParticleRainAlphaReduction = fmod(floor(packedParticleTestFlags / 2.0f), 2.0f) >= 1.0f;
    float night = disableParticleNightDimming ? 0.0f : GetAmbientNightWeight();
    float rain = disableParticleRainAlphaReduction ? 0.0f : max(saturate(AC_RainFXWeight), saturate(AC_SceneWettness));
    const bool groundFog = particleLightingScale > 1.5f && particleLightingScale < 2.5f;
    const bool waterParticle = particleLightingScale >= 2.5f;
    const float enabledStrength = saturate(AC_EnableParticleLighting * AC_ParticleLightingStrength);
    const float regularStrength = enabledStrength * saturate(particleLightingScale);
    const float nightStrength = waterParticle ? enabledStrength : regularStrength;
    // Preserve the former 0.25 water-particle rain response while allowing
    // its night lighting to use the full renderer strength.
    const float rainStrength = waterParticle ? enabledStrength * 0.25f : regularStrength;
    const float nightFloor = (groundFog || waterParticle) ? 0.10f : 0.24f;
    const float nightTintStrength = waterParticle ? 1.0f : 0.80f;
    color.rgb = ApplyAmbientNightTint(color.rgb, night * nightStrength * nightTintStrength);
    if (waterParticle)
    {
        // Water keeps the shared ground-fog night tint, then receives a small
        // steel-blue bias so dense overlapping spray does not return to grey.
        const float waterNightTint = night * nightStrength;
        color.rgb *= lerp(float3(1.0f, 1.0f, 1.0f),
            float3(0.78f, 0.90f, 1.08f), waterNightTint);
    }
    float nightDim = lerp(1.0f, nightFloor, night);
    color.rgb *= lerp(1.0f, nightDim, nightStrength);
    float rainAlpha = lerp(1.0f, 0.24f, rain);
    color.a *= lerp(1.0f, rainAlpha, rainStrength);
    return color;
}
float4 PSMain( PS_INPUT Input ) : SV_TARGET
{
    float4 color = TX_Texture0.Sample(SS_Linear, Input.vTexcoord);
    color *= Input.vDiffuse;
#ifdef USE_FFDATA
    color *= cbFFData.textureFactor;
#endif
    color = AdaptParticleLighting(color, Input.vParticleLightingScale);
    return color;
}
