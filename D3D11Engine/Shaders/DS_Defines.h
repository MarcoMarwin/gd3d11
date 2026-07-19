struct DEFERRED_PS_OUTPUT
{
	float4 vDiffuse : SV_TARGET0;
	float2 vNrm : SV_TARGET1; 
	float2 vSI_SP : SV_TARGET2;
	float2 vVelocity : SV_TARGET3;  // Screen-space velocity for motion vectors
	float vTransparencyAndCompositionMask : SV_TARGET4;
	float vReactiveMask : SV_TARGET5;
};

struct DEFERRED_PS_OUTPUT_ALPHA_TO_COVERAGE
{
	float4 vDiffuse : SV_TARGET0;
	float4 vNrm_SI_SP : SV_TARGET1; 
	uint fCoverage	: SV_Coverage;
};

struct FORWARD_PLUS_PS_OUTPUT
{
	float4 vColor : SV_TARGET0;
	float2 vNrm : SV_TARGET1;
	float2 vSI_SP : SV_TARGET2;
	float2 vVelocity : SV_TARGET3;
	float vTransparencyAndCompositionMask : SV_TARGET4;
	float vReactiveMask : SV_TARGET5;
};


// Octahedral encoding: map a unit normal to [-1,1]^2 for R16G16_SNORM storage
// Reference: "A Survey of Efficient Representations for Independent Unit Vectors" (Cigolle et al. 2014)
float2 OctWrap(float2 v)
{
    return (1.0 - abs(v.yx)) * (v.xy >= 0.0 ? 1.0 : -1.0);
}

float2 EncodeNormalGBuffer(float3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    n.xy = n.z >= 0.0 ? n.xy : OctWrap(n.xy);
    return n.xy;
}

// Decode octahedral [-1,1]^2 back to a unit normal
float3 DecodeNormalGBuffer(float2 encoded)
{
    float3 n;
    n.z = 1.0 - abs(encoded.x) - abs(encoded.y);
    n.xy = n.z >= 0.0 ? encoded.xy : OctWrap(encoded.xy);
    return normalize(n);
}

// Indoor daylight state is stored in the albedo alpha channel after the
// geometry pass. Keep the two values in a reserved, narrow range near one so
// ordinary dark vertex lighting can never be mistaken for an indoor marker.
static const float INDOOR_NO_DAYLIGHT_GBUFFER_MARKER = 254.0f / 255.0f;
static const float INDOOR_DAYLIGHT_WINDOW_GBUFFER_MARKER = 253.0f / 255.0f;
static const float INDOOR_GBUFFER_MARKER_EPSILON = 0.0015f;

float IsEncodedIndoorNoDaylight(float rawLighting)
{
    return abs(rawLighting - INDOOR_NO_DAYLIGHT_GBUFFER_MARKER)
        < INDOOR_GBUFFER_MARKER_EPSILON ? 1.0f : 0.0f;
}

float IsEncodedIndoorDaylightWindow(float rawLighting)
{
    return abs(rawLighting - INDOOR_DAYLIGHT_WINDOW_GBUFFER_MARKER)
        < INDOOR_GBUFFER_MARKER_EPSILON ? 1.0f : 0.0f;
}

float DecodeIndoorReceiverMask(float rawLighting)
{
    return max(IsEncodedIndoorNoDaylight(rawLighting),
        IsEncodedIndoorDaylightWindow(rawLighting));
}

float DecodeIndoorDaylightMask(float rawLighting)
{
    return 1.0f - IsEncodedIndoorNoDaylight(rawLighting);
}

float DecodeIndoorVertexLighting(float rawLighting)
{
    return lerp(rawLighting, 0.05f, DecodeIndoorReceiverMask(rawLighting));
}

float EncodeIndoorDaylightMarker(
    float rawLighting, float noDaylight, float daylightWindow)
{
    // Reserve the two top R8 values for the markers. Mapping an unmarked
    // value this close to full brightness to 1.0 is visually lossless and
    // prevents an accidental collision after the BGRA8 GBuffer write.
    float unmarkedLighting = rawLighting > 0.989f ? 1.0f : rawLighting;
    return noDaylight > 0.5f
        ? INDOOR_NO_DAYLIGHT_GBUFFER_MARKER
        : (daylightWindow > 0.5f
            ? INDOOR_DAYLIGHT_WINDOW_GBUFFER_MARKER
            : unmarkedLighting);
}
