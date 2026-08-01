#!/bin/bash
echo "=== GIT DIFF CHECK ==="
git diff --check

echo "=== CONFLICT MARKER SEARCH ==="
grep -rn "<<<<<<<" D3D11Engine/
grep -rn "=======" D3D11Engine/
grep -rn ">>>>>>>" D3D11Engine/

echo "=== ApplyMaterialCompatibility CALLS ==="
grep -rn "ApplyMaterialCompatibility" D3D11Engine/

echo "=== version < 2 ==="
grep -rn "version < 2" D3D11Engine/

echo "=== DisplacementFactor CLAMP ==="
grep -rn "DisplacementFactor = ClampMaterialScalar" D3D11Engine/

echo "=== GetEffectiveMaterialBuffer DisplacementFactor ==="
grep -rn "buffer.DisplacementFactor *=" D3D11Engine/D3D11GraphicsEngine.cpp
grep -rn "buffer.DisplacementFactor = 0.0f;" D3D11Engine/D3D11GraphicsEngine.cpp -B 1

echo "=== NormalmapStrength & WetGroundSSRStrength CLAMPS ==="
grep -rn "NormalmapStrength = ClampMaterialScalar" D3D11Engine/
grep -rn "WetGroundSSRStrength = ClampMaterialScalar" D3D11Engine/

echo "=== sampleMaterialStrength / saturate IN PS_PFX_WetGroundSSR ==="
grep -n "TX_Material.SampleLevel" D3D11Engine/Shaders/PS_PFX_WetGroundSSR.hlsl

echo "=== 10.0f IN PS_PFX_WetGroundSSR ==="
grep -n "10.0f" D3D11Engine/Shaders/PS_PFX_WetGroundSSR.hlsl
