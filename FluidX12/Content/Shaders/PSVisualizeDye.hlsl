//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

//--------------------------------------------------------------------------------------
// Structure
//--------------------------------------------------------------------------------------
struct PSIn
{
	float4 Pos : SV_POSITION;
	float2 Tex : TEXCOORD;
};

//--------------------------------------------------------------------------------------
// Texture
//--------------------------------------------------------------------------------------
Texture3D		g_txSrc;

//--------------------------------------------------------------------------------------
// Texture sampler
//--------------------------------------------------------------------------------------
SamplerState	g_smpLinear;

min16float4 main(PSIn input) : SV_TARGET
{
	min16float4 color = min16float4(g_txSrc.SampleLevel(g_smpLinear, float3(input.Tex, 0.5), 0.0));
	color.xyz = sqrt(color.xyz * color.w);
	
	return saturate(color);
}
