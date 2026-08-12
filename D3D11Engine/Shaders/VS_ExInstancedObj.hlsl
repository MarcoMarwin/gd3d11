//--------------------------------------------------------------------------------------
// Simple vertex shader
//--------------------------------------------------------------------------------------

#include "Globals_VS_ExConstants.h"

cbuffer Matrices_PerFrame : register( b0 )
{
	VS_ExConstantBuffer_PerFrame frame;
};

#define MAX_CHARACTER_INTERACTION_NPCS 5
#define MAX_CHARACTER_INTERACTION_INFLUENCERS (1 + MAX_CHARACTER_INTERACTION_NPCS)

cbuffer WindParams : register(b1)
{
     float3 windDir;
     float globalTime;
     float minHeight;
     float maxHeight;
     float prevGlobalTime;
     float padding0;
     float4 interactionPositions[MAX_CHARACTER_INTERACTION_INFLUENCERS];
     float characterInteractionStrength;
     float3 padding1;
};

#ifndef WIND_META_SRV
#define WIND_META_SRV 0
#endif

#if WIND_META_SRV
struct WindMetaDataEntry
{
    float minHeight;
    float maxHeight;
    float2 horizontalExtent;
};

StructuredBuffer<WindMetaDataEntry> WindMetaData;
#endif

//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
struct VS_INPUT
{
    float3 vPosition    : POSITION;
    float3 vNormal      : NORMAL;
    float2 vTex1        : TEXCOORD0;
    float2 vTex2        : TEXCOORD1;
    float4 vDiffuse     : DIFFUSE;
    float4x4 InstanceWorldMatrix : INSTANCE_WORLD_MATRIX;
    float4x4 InstancePrevWorldMatrix : INSTANCE_PREV_WORLD_MATRIX;
    float4 InstanceColor : INSTANCE_COLOR;
    float2 InstanceWind : INSTANCE_WINDFLUENCE;
    uint InstanceWindMetaIndex : INSTANCE_WIND_META_INDEX;
    float4 InstanceEmissiveColor : INSTANCE_EMISSIVE_COLOR;
};

struct VS_OUTPUT
{
    float2 vTexcoord        : TEXCOORD0;
    float2 vTexcoord2       : TEXCOORD1;
    float4 vDiffuse         : TEXCOORD2;
    float3 vNormalVS        : TEXCOORD4;
    float3 vViewPosition    : TEXCOORD5;
    float4 vCurrClipPos     : TEXCOORD6;  // Current clip position for velocity
    float4 vPrevClipPos     : TEXCOORD7;  // Previous clip position for velocity
    float4 vEmissiveColor   : TEXCOORD8;
    
    float4 vPosition        : SV_POSITION;
};

#if SHD_WIND

//less then trunkStiffness (%) will be absolutely stay, like tree trunk
static const float windStrengMult = 16.0f; // Preserve the established Spacer wind-strength scale.
static const float PI_2 = 6.283185; // 2 * PI

float GetInstancePhaseOffset(float2 objectWorldXZ, float maxHeightValue)
{
    // Stable per-instance phase. World position also makes nearby vegetation
    // share the broad gust front instead of oscillating as isolated objects.
    float seed = dot(objectWorldXZ, float2(0.0129898f, 0.078233f))
        + maxHeightValue * 0.0053539f;
    return frac(sin(seed) * 43758.5453f) * PI_2;
}

float3 ApplyVegetationWind(
    float3 vertexPos,
    float3 direction,
    float heightNorm,
    float timeSec,
    float2 objectWorldXZ,
    float maxHeightValue,
    float2 horizontalExtent,
    float objectHeight,
    float windStrength)
{
    float maxHorizontalExtent = max(max(horizontalExtent.x, horizontalExtent.y), 0.001f);
    float slenderness = objectHeight / (maxHorizontalExtent * 2.0f);
    float treeProfile = smoothstep(1.8f, 4.5f, slenderness);

    // Small plants may bend close to the soil; tree-like objects retain a
    // firmer trunk. The smooth cubic ramp prevents a visible hinge line.
    float anchorHeight = lerp(0.045f, 0.11f, treeProfile);
    float bend = saturate((heightNorm - anchorHeight) / max(1.0f - anchorHeight, 0.001f));
    bend = bend * bend * (3.0f - 2.0f * bend);
    bend *= lerp(1.0f, heightNorm, treeProfile);

    float3 horizontalDirection = float3(direction.x, 0.0f, direction.z);
    horizontalDirection *= rsqrt(max(dot(horizontalDirection, horizontalDirection), 0.0001f));
    float3 crossDirection = float3(-horizontalDirection.z, 0.0f, horizontalDirection.x);

    float instancePhase = GetInstancePhaseOffset(objectWorldXZ, maxHeightValue);
    float gustPhase = timeSec * 0.34f
        + dot(objectWorldXZ, horizontalDirection.xz) * 0.0011f;
    float gust = 0.72f + 0.28f * sin(gustPhase);

    // Broad downwind bending plus restrained crosswind inertia. Keeping the
    // main response predominantly downwind avoids the old metronome motion.
    float mainWave = sin(timeSec * 0.88f + instancePhase + heightNorm * 1.35f);
    float downwind = (0.52f + 0.48f * mainWave) * gust;
    float crosswind = sin(timeSec * 1.31f + instancePhase * 1.71f + heightNorm * 2.4f)
        * 0.16f * gust;

    // Approximate branch/leaf freedom from distance to the local centre. This
    // uses existing bounds and avoids vertex textures or authored weight data.
    float2 radialPosition = vertexPos.xz / max(horizontalExtent, float2(0.001f, 0.001f));
    float edgeWeight = smoothstep(0.25f, 0.85f, saturate(length(radialPosition)));
    float detailWeight = edgeWeight * smoothstep(0.42f, 0.82f, heightNorm);
    float detailWave = sin(timeSec * 3.15f + instancePhase * 2.13f
        + dot(vertexPos.xz, float2(0.071f, 0.053f))) * 0.055f * detailWeight;

    float amplitude = windStrength * windStrengMult * bend;
    return (horizontalDirection * (downwind + detailWave)
        + crossDirection * crosswind) * amplitude;
}
#endif

#if SHD_INFLUENCE

// HERO/NPC INTERACTION CONST
static const float heroAffectRange = 50.0f;
static const float heroAffectStrength = 38.0f;

float3 CalculateSingleActorInfluence(
    float4 actorPositionAndActive,
    float3 vertexLocalPos,
    float minHeight,
    float maxHeight,
    float4x4 instWorldMatrix
)
{
    if (actorPositionAndActive.w <= 0.5f)
        return float3(0.0f, 0.0f, 0.0f);

    float heightRange = max(maxHeight - minHeight, 0.001f);
    float vertexHeightNorm = saturate((vertexLocalPos.y - minHeight) / heightRange);

    // 15% of object height check
    float heightMask = smoothstep(0.14f, 0.16f, vertexHeightNorm);

    float3 vertexWorldPos = mul(float4(vertexLocalPos, 1.0f), instWorldMatrix).xyz;
    float3 toVertex = vertexWorldPos - actorPositionAndActive.xyz;

    float3 displaceDirWorld = lerp(float3(0.0f, 1.0f, 0.0f), normalize(toVertex), step(0.001f, length(toVertex)));

    float distanceXZ = length(toVertex.xz);
    float distanceFactor = exp(-(distanceXZ * distanceXZ) / (1.8f * heroAffectRange * heroAffectRange));

    float influence = distanceFactor * vertexHeightNorm * heightMask;

    float randomOffset = frac(sin(dot(vertexLocalPos.xz, float2(12.9898f, 78.233f))) * 43758.5453f);
    influence *= 0.9f + 0.1f * randomOffset;

    float3 displaceDirLocal = normalize(mul(displaceDirWorld, (float3x3)instWorldMatrix));
    return displaceDirLocal * heroAffectStrength * characterInteractionStrength * influence;
}

float3 CalculateActorInteractionInfluence(
    float3 vertexLocalPos,
    float minHeight,
    float maxHeight,
    float4x4 instWorldMatrix
)
{
    float3 totalInfluence = float3(0.0f, 0.0f, 0.0f);

    [unroll]
    for (int i = 0; i < MAX_CHARACTER_INTERACTION_INFLUENCERS; ++i)
    {
        totalInfluence += CalculateSingleActorInfluence(
            interactionPositions[i],
            vertexLocalPos,
            minHeight,
            maxHeight,
            instWorldMatrix);
    }

    float maxDisplacement = heroAffectStrength * characterInteractionStrength;
    float totalLengthSq = dot(totalInfluence, totalInfluence);
    if (totalLengthSq > maxDisplacement * maxDisplacement)
        totalInfluence *= maxDisplacement * rsqrt(max(totalLengthSq, 0.0001f));

    return totalInfluence;
}
#endif

//--------------------------------------------------------------------------------------
// Vertex Shader
//--------------------------------------------------------------------------------------
VS_OUTPUT VSMain( VS_INPUT Input )
{
    VS_OUTPUT Output;
            
    // Base vertex positions (local). Previous uses the previous wind phase for motion vectors.
    float3 position = Input.vPosition;
    float3 prevPosition = Input.vPosition;
    float localMinHeight = minHeight;
    float localMaxHeight = maxHeight;
    float2 localHorizontalExtent = float2(1.0f, 1.0f);
    float interactionWindScale = 1.0f;

#if WIND_META_SRV
    WindMetaDataEntry meta = WindMetaData[Input.InstanceWindMetaIndex];
    localMinHeight = meta.minHeight;
    localMaxHeight = meta.maxHeight;
    localHorizontalExtent = meta.horizontalExtent;
#endif

#if SHD_INFLUENCE
    if (Input.InstanceWind.y > 0) {
        // CHARACTER INTERACTION MOVING BUSHES SHADER
        float3 interactionOffset = CalculateActorInteractionInfluence(
            position, localMinHeight, localMaxHeight, Input.InstanceWorldMatrix);
        float maxInteractionDisplacement = max(
            heroAffectStrength * characterInteractionStrength, 0.0001f);
        float interactionInfluence = saturate(
            length(interactionOffset) / maxInteractionDisplacement);
        interactionWindScale = 1.0f - interactionInfluence;
        position += interactionOffset;
        prevPosition += interactionOffset;
    }
#endif

#if SHD_WIND
    if (Input.InstanceWind.x > 0) {
        // WIND SHADER
        // Protect 0 height
        float heightRange = max(localMaxHeight - localMinHeight, 0.001);
        float vertexHeightNorm = saturate((Input.vPosition.y - localMinHeight) / heightRange);
        float2 currentObjectWorldXZ = mul(float4(0.0f, 0.0f, 0.0f, 1.0f), Input.InstanceWorldMatrix).xz;
        float2 previousObjectWorldXZ = mul(float4(0.0f, 0.0f, 0.0f, 1.0f), Input.InstancePrevWorldMatrix).xz;

        // Apply current and previous wind phases with the same local interaction
        // attenuation so FSR receives consistent vegetation motion vectors.
        float3 windDirection = normalize(windDir);
        float3 currentWindOffset = ApplyVegetationWind(
            Input.vPosition, windDirection, vertexHeightNorm, globalTime, currentObjectWorldXZ,
            localMaxHeight, localHorizontalExtent, heightRange, Input.InstanceWind.x
        );
        float3 previousWindOffset = ApplyVegetationWind(
            Input.vPosition, windDirection, vertexHeightNorm, prevGlobalTime, previousObjectWorldXZ,
            localMaxHeight, localHorizontalExtent, heightRange, Input.InstanceWind.x
        );
        position += currentWindOffset * interactionWindScale;
        prevPosition += previousWindOffset * interactionWindScale;
    }
#endif
    
    // Common processing for both cases
    float3 worldPos = mul(float4(position, 1.0), Input.InstanceWorldMatrix).xyz;
    
    // Calculate previous world position for motion vectors
    float3 prevWorldPos = mul(float4(prevPosition, 1.0), Input.InstancePrevWorldMatrix).xyz;

    Output.vPosition = mul(float4(worldPos, 1.0), frame.M_ViewProj);
    Output.vTexcoord = Input.vTex1;
    Output.vTexcoord2 = Input.vTex2;
    Output.vDiffuse = Input.InstanceColor;
    Output.vEmissiveColor = Input.InstanceEmissiveColor;
    Output.vNormalVS = mul(Input.vNormal, mul((float3x3)Input.InstanceWorldMatrix, (float3x3)frame.M_View));
    Output.vViewPosition = mul(float4(worldPos, 1.0), frame.M_View);
    
    // Store clip positions for velocity calculation in pixel shader
    // Use UNJITTERED matrices for correct velocity (jitter would cause incorrect motion)
    Output.vCurrClipPos = mul(float4(worldPos, 1.0), frame.M_UnjitteredViewProj);
    Output.vPrevClipPos = mul(float4(prevWorldPos, 1.0), frame.M_PrevViewProj);
    
    return Output;
}

