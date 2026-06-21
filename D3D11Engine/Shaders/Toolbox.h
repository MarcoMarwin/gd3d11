
void ClipDistanceEffect(float viewSpaceDepth, float drawDistance, float noise, float noiseScale)
{
	if(viewSpaceDepth + noise * noiseScale > drawDistance)
		discard;
}

float3x3 cotangent_frame( float3 N, float3 p, float2 uv )
{
    // get edge vectors of the pixel triangle
    float3 dp1 = ddx( p );
    float3 dp2 = ddy( p );
    float2 duv1 = ddx( uv );
    float2 duv2 = ddy( uv );
 
    // solve the linear system
    float3 dp2perp = cross( dp2, N );
    float3 dp1perp = cross( N, dp1 );
    float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    float3 B = dp2perp * duv1.y + dp1perp * duv2.y;
 
	// Negate because of left-handedness
	//T *= -1;
	//B *= -1;
 
    // construct a scale-invariant frame 
    float invmax = rsqrt( max( dot(T,T), dot(B,B) ) );
    return float3x3( T * invmax, B * invmax, N );
}

/** Resource-conscious parallax occlusion mapping using the derivative-built TBN frame. */
float2 parallax_occlusion_mapping( float3 N, float3 viewPosition, Texture2D heightMap,
    float2 texcoord, SamplerState samplerState, float strength )
{
    uint width;
    uint height;
    heightMap.GetDimensions( width, height );
    if ( width == 0 || height == 0 || strength <= 0.0001f )
        return texcoord;

    // Fade POM out before distant surfaces become sub-pixel sized.
    float distanceFade = saturate( (3500.0f - length(viewPosition)) / 1000.0f );
    float heightScale = 0.025f * clamp(strength, 0.0f, 4.0f) * distanceFade;
    if ( heightScale <= 0.0001f )
        return texcoord;

    float3x3 tbn = cotangent_frame( normalize(N), -viewPosition, texcoord );
    float3 viewDirTS = mul( tbn, normalize(-viewPosition) );
    if ( viewDirTS.z < 0.0f )
        viewDirTS = -viewDirTS;

    viewDirTS.z = max( viewDirTS.z, 0.15f );
    const float layerCount = lerp( 20.0f, 8.0f, saturate(viewDirTS.z) );
    const float layerDepth = rcp( layerCount );
    const float2 uvGradientX = ddx( texcoord );
    const float2 uvGradientY = ddy( texcoord );
    const float2 deltaUV = (viewDirTS.xy / viewDirTS.z) * heightScale / layerCount;

    float2 currentUV = texcoord;
    float currentLayerDepth = 0.0f;
    float currentHeight = heightMap.SampleGrad( samplerState, currentUV, uvGradientX, uvGradientY ).r;

    [loop]
    for ( int step = 0; step < 20 && currentLayerDepth < currentHeight; ++step )
    {
        currentUV -= deltaUV;
        currentLayerDepth += layerDepth;
        currentHeight = heightMap.SampleGrad( samplerState, currentUV, uvGradientX, uvGradientY ).r;
    }

    const float2 previousUV = currentUV + deltaUV;
    const float previousHeight = heightMap.SampleGrad( samplerState, previousUV, uvGradientX, uvGradientY ).r;
    const float afterDepth = currentHeight - currentLayerDepth;
    const float beforeDepth = previousHeight - (currentLayerDepth - layerDepth);
    const float interpolation = saturate( afterDepth / (afterDepth - beforeDepth + 0.00001f) );
    return lerp( currentUV, previousUV, interpolation );
}

/** Magic TBN-Calculation function */
float3 perturb_normal( float3 N, float3 V, Texture2D normalmap, float2 texcoord, SamplerState samplerState, float normalmapDepth = 1.0f)
{
    // assume N, the interpolated vertex normal and 
    // V, the view vector (vertex to eye)
    float3 nrmmap = normalmap.Sample(samplerState, texcoord).xyz * 2 - 1;
	nrmmap.xy *= -1.0f;
	nrmmap.xy *= normalmapDepth;
	nrmmap = normalize(nrmmap);
	
    float3x3 TBN = cotangent_frame( N, -V, texcoord );
    return normalize( mul(transpose(TBN), nrmmap) );
}