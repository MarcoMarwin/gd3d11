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
     float accurateWindVelocity;
     float4 cameraWorldPosition;
     float4 interactionPositions[MAX_CHARACTER_INTERACTION_INFLUENCERS];
     float characterInteractionStrength;
     float characterInteractionRange;
     float2 padding1;
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
    float4 groundPlane;
    float grassShear;
    float3 padding;
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
static const float trunkStiffness = 0.12f;
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
    float windStrength,
    float grassShearProfile,
    float detailLod)
{
    if (grassShearProfile < 0.5f)
    {
        // Preserve the established tree path. In particular, the lower twelve
        // percent remain completely rigid; terrain-derived grass anchoring
        // must never translate a trunk at its base.
        float shouldAffect = saturate(sign(heightNorm - trunkStiffness + 0.0001f));
        float adjustedHeight = saturate(
            (heightNorm - trunkStiffness) / (1.0f - trunkStiffness)) * shouldAffect;
        float heightFactor = pow(adjustedHeight, 2.6f);
        float instancePhase = GetInstancePhaseOffset(objectWorldXZ, maxHeightValue);
        float mainWave = sin(timeSec + heightNorm * 3.0f + instancePhase) * 0.8f;
        float topSmoothing = smoothstep(0.7f, 0.9f, adjustedHeight);
        float combinedWave = mainWave * (1.0f - topSmoothing * 0.3f);
        float leafTurbulence = 0.0f;
        if (detailLod > 0.0f)
        {
            float secondaryWave = cos(timeSec * 0.7f + heightNorm * 5.0f
                + instancePhase * 1.5f) * 0.8f;
            float inertiaEffect = sin(timeSec * 0.3f + heightNorm * 8.0f) * 0.1f;
            combinedWave += (secondaryWave * 0.5f
                * (1.0f - topSmoothing * 0.3f) + inertiaEffect * topSmoothing)
                * detailLod;
            leafTurbulence = (sin(timeSec * 4.0f + vertexPos.x * 15.0f)
                + cos(timeSec * 3.7f + vertexPos.z * 12.0f))
                * (0.05f * detailLod) * topSmoothing;
        }
        return direction * windStrength * windStrengMult
            * (combinedWave + leafTurbulence) * heightFactor;
    }

    float maxHorizontalExtent = max(max(horizontalExtent.x, horizontalExtent.y), 0.001f);
    float slenderness = objectHeight / (maxHorizontalExtent * 2.0f);
    float treeProfile = lerp(
        smoothstep(1.8f, 4.5f, slenderness),
        0.0f,
        grassShearProfile);

    // Small plants may bend close to the soil; tree-like objects retain a
    // firmer trunk. The smooth cubic ramp prevents a visible hinge line.
    float anchorHeight = lerp(0.045f, 0.11f, treeProfile);
    float treeBend = saturate((heightNorm - anchorHeight) / max(1.0f - anchorHeight, 0.001f));
    treeBend = treeBend * treeBend * (3.0f - 2.0f * treeBend);
    treeBend *= heightNorm;
    // Grass root weighting is applied exactly once outside this function as
    // an affine terrain shear. Trees retain their authored nonlinear bend.
    float bend = lerp(1.0f, treeBend, treeProfile);

    float3 horizontalDirection = float3(direction.x, 0.0f, direction.z);
    horizontalDirection *= rsqrt(max(dot(horizontalDirection, horizontalDirection), 0.0001f));
    float3 crossDirection = float3(-horizontalDirection.z, 0.0f, horizontalDirection.x);

    float instancePhase = GetInstancePhaseOffset(objectWorldXZ, maxHeightValue);
    float gustPhase = timeSec * 0.34f
        + dot(objectWorldXZ, horizontalDirection.xz) * 0.0011f;
    float gust = 0.72f + 0.28f * sin(gustPhase);

    // Broad downwind bending plus restrained crosswind inertia. Keeping the
    // main response predominantly downwind avoids the old metronome motion.
    float mainWave = sin(timeSec * 0.88f + instancePhase
        + heightNorm * 1.35f * treeProfile);
    float downwind = (0.52f + 0.48f * mainWave) * gust;
    float crosswind = sin(timeSec * 1.31f + instancePhase * 1.71f
        + heightNorm * 2.4f * treeProfile)
        * (0.16f * detailLod) * gust;

    // Approximate branch/leaf freedom from distance to the local centre. This
    // uses existing bounds and avoids vertex textures or authored weight data.
    float2 radialPosition = vertexPos.xz / max(horizontalExtent, float2(0.001f, 0.001f));
    float edgeWeight = smoothstep(0.25f, 0.85f, saturate(length(radialPosition)));
    float detailWeight = edgeWeight * smoothstep(0.42f, 0.82f, heightNorm)
        * treeProfile;
    float detailWave = sin(timeSec * 3.15f + instancePhase * 2.13f
        + dot(vertexPos.xz, float2(0.071f, 0.053f)))
        * (0.055f * detailLod) * detailWeight;

    float amplitude = windStrength * windStrengMult * bend;
    return (horizontalDirection * (downwind + detailWave)
        + crossDirection * crosswind) * amplitude;
}
#endif

#if SHD_INFLUENCE

// HERO/NPC INTERACTION CONST
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
    float interactionRange = max(characterInteractionRange, 1.0f);
    float distanceFactor = exp(-(distanceXZ * distanceXZ) / (1.8f * interactionRange * interactionRange));

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
    float4 localGroundPlane = 0.0f;
    float interactionWindScale = 1.0f;
    float3 currentWorldWindOffset = 0.0f;
    float3 previousWorldWindOffset = 0.0f;
    float terrainRootWeight = 1.0f;
    float signedTerrainShear = 1.0f;
    float terrainTreeProfile = 1.0f;
    float grassShearProfile = 0.0f;

#if WIND_META_SRV
    WindMetaDataEntry meta = WindMetaData[Input.InstanceWindMetaIndex];
    localMinHeight = meta.minHeight;
    localMaxHeight = meta.maxHeight;
    localHorizontalExtent = meta.horizontalExtent;
    localGroundPlane = meta.groundPlane;
    grassShearProfile = saturate(meta.grassShear);

    const float validGroundPlane = step(
        1.0e-8f, dot(localGroundPlane.xyz, localGroundPlane.xyz));
    if (grassShearProfile > 0.5f && validGroundPlane > 0.5f)
    {
        float3 rootTestWorldPosition = mul(
            float4(Input.vPosition, 1.0f), Input.InstanceWorldMatrix).xyz;
        float rootHeightAboveGround = dot(
            localGroundPlane.xyz, rootTestWorldPosition) + localGroundPlane.w;
        float3 rootColumnTopWorldPosition = mul(
            float4(Input.vPosition.x, localMaxHeight, Input.vPosition.z, 1.0f),
            Input.InstanceWorldMatrix).xyz;
        float rootVisibleHeight = max(
            dot(localGroundPlane.xyz, rootColumnTopWorldPosition)
                + localGroundPlane.w,
            0.001f);
        terrainTreeProfile = 1.0f - grassShearProfile;
        float rigidRootCollar = min(5.0f, rootVisibleHeight * 0.08f);
        terrainRootWeight = saturate(
            (rootHeightAboveGround - rigidRootCollar)
            / max(rootVisibleHeight - rigidRootCollar, 0.001f));
        // A bounded signed factor makes a coarse two-triangle grass card an
        // affine shear around the terrain plane. Its interpolated soil crossing
        // is therefore stationary. The bound prevents malformed placements
        // from recreating the former large inverse-deformation artifacts.
        signedTerrainShear = clamp(
            rootHeightAboveGround / rootVisibleHeight, -0.35f, 1.25f);
    }
#endif

#if SHD_INFLUENCE
    if (Input.InstanceWind.y > 0) {
        // CHARACTER INTERACTION MOVING BUSHES SHADER
        float3 interactionOffset = CalculateActorInteractionInfluence(
            position, localMinHeight, localMaxHeight, Input.InstanceWorldMatrix);
        interactionOffset *= lerp(terrainRootWeight, 1.0f, terrainTreeProfile);
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
        float terrainHeightNorm = terrainRootWeight;
        float legacyHeightNorm = saturate((Input.vPosition.y - localMinHeight) / heightRange);
        float validTerrainAnchor = step(
            1.0e-8f, dot(localGroundPlane.xyz, localGroundPlane.xyz));
        // Terrain-relative height is exclusively a grass-card solution. Trees
        // retain their authored local bounding-box height and rigid trunk.
        float vertexHeightNorm = lerp(legacyHeightNorm, terrainHeightNorm,
            validTerrainAnchor * grassShearProfile);
        float2 currentObjectWorldXZ = mul(float4(0.0f, 0.0f, 0.0f, 1.0f), Input.InstanceWorldMatrix).xz;
        float2 previousObjectWorldXZ = mul(float4(0.0f, 0.0f, 0.0f, 1.0f), Input.InstancePrevWorldMatrix).xz;

        float3 objectWorldPosition = mul(
            float4(0.0f, 0.0f, 0.0f, 1.0f), Input.InstanceWorldMatrix).xyz;
        float3 cameraDelta = objectWorldPosition - cameraWorldPosition.xyz;
        float windDistanceSq = dot(cameraDelta, cameraDelta);
        float grassWindFade = 1.0f - smoothstep(9000000.0f, 25000000.0f, windDistanceSq);
        float treeWindFade = 1.0f - smoothstep(144000000.0f, 196000000.0f, windDistanceSq);
        float windDistanceFade = lerp(treeWindFade, grassWindFade, grassShearProfile);
        float treeDetailLod = 1.0f - smoothstep(64000000.0f, 100000000.0f, windDistanceSq);
        float windDetailLod = lerp(treeDetailLod, 1.0f, grassShearProfile);

        // Apply current and previous wind phases with the same local interaction
        // attenuation so FSR receives consistent vegetation motion vectors.
        if (windDistanceFade > 0.0f)
        {
            currentWorldWindOffset = ApplyVegetationWind(
                Input.vPosition, windDir, vertexHeightNorm, globalTime, currentObjectWorldXZ,
                localMaxHeight, localHorizontalExtent, heightRange, Input.InstanceWind.x,
                grassShearProfile, windDetailLod
            );
            if (accurateWindVelocity > 0.5f)
            {
                previousWorldWindOffset = ApplyVegetationWind(
                    Input.vPosition, windDir, vertexHeightNorm, prevGlobalTime, previousObjectWorldXZ,
                    localMaxHeight, localHorizontalExtent, heightRange, Input.InstanceWind.x,
                    grassShearProfile, windDetailLod
                );
            }
            else
            {
                previousWorldWindOffset = currentWorldWindOffset;
            }
            currentWorldWindOffset *= windDistanceFade;
            previousWorldWindOffset *= windDistanceFade;
        }
        currentWorldWindOffset *= interactionWindScale;
        previousWorldWindOffset *= interactionWindScale;
        const float rootFactor = lerp(
            signedTerrainShear,
            1.0f,
            terrainTreeProfile);
        currentWorldWindOffset *= rootFactor;
        previousWorldWindOffset *= rootFactor;
    }
#endif
    
    // Common processing for both cases
    // Wind direction is authored in world space. Apply its displacement after
    // the instance transform so rotating a vegetation VOB in the Spacer does
    // not rotate the wind along with the mesh.
    float3 worldPos = mul(float4(position, 1.0), Input.InstanceWorldMatrix).xyz
        + currentWorldWindOffset;
    
    // Calculate previous world position for motion vectors
    float3 prevWorldPos = mul(float4(prevPosition, 1.0), Input.InstancePrevWorldMatrix).xyz
        + previousWorldWindOffset;

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

