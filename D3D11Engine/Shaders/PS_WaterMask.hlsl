struct PS_INPUT
{
	float4 vPosition : SV_POSITION;
};

float4 PSMain(PS_INPUT Input) : SV_TARGET
{
	return float4(1.0f, 0.0f, 0.0f, 1.0f);
}
