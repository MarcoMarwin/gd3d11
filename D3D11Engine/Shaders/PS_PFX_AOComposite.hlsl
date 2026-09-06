SamplerState SS_PointClamp : register( s0 );
Texture2D TX_AO : register( t0 );
Texture2D TX_AOEdges : register( t1 );

cbuffer AOCompositeConstantBuffer : register( b0 )
{
    float AO_Strength;
    float ReactiveMaskEnabled;
    float2 AO_Padding;
}

struct PS_INPUT
{
    float2 vTexcoord : TEXCOORD0;
    float3 vEyeRay   : TEXCOORD1;
    float4 vPosition : SV_POSITION;
};

struct PS_OUTPUT
{
    float4 Color : SV_TARGET0;
    float Reactive : SV_TARGET1;
};

PS_OUTPUT PSMain( PS_INPUT input )
{
    const float rawVisibility = TX_AO.SampleLevel( SS_PointClamp, input.vTexcoord, 0 ).r;
    float visibility = rawVisibility;
    visibility = saturate( lerp( 1.0, visibility, AO_Strength ) );

    PS_OUTPUT output;
    output.Color = float4( visibility.xxx, 1.0 );
    output.Reactive = 0.0f;
    [branch]
    if ( ReactiveMaskEnabled > 0.5f ) {
        // Only make AO-sensitive depth transitions reactive. Stable flat surfaces
        // keep their existing FSR3 history and do not become globally reactive.
        const uint packedEdges = (uint)( TX_AOEdges.SampleLevel( SS_PointClamp, input.vTexcoord, 0 ).r * 255.5f );
        const float edgeLeft = (float)( ( packedEdges >> 6 ) & 0x3u ) / 3.0f;
        const float edgeRight = (float)( ( packedEdges >> 4 ) & 0x3u ) / 3.0f;
        const float edgeTop = (float)( ( packedEdges >> 2 ) & 0x3u ) / 3.0f;
        const float edgeBottom = (float)( ( packedEdges >> 0 ) & 0x3u ) / 3.0f;
        const float edgeDiscontinuity = 1.0f - min( min( edgeLeft, edgeRight ), min( edgeTop, edgeBottom ) );
        const float aoDarkening = saturate( 1.0f - visibility );
        const float aoGradient = saturate( fwidth( visibility ) * 16.0f );
        const float aoTransition = max(
            smoothstep( 0.05f, 0.45f, edgeDiscontinuity ),
            smoothstep( 0.01f, 0.06f, aoGradient ) );
        output.Reactive = saturate( aoDarkening * aoTransition );
    }
    return output;
}
