#!/bin/bash
echo "=== GIT DIFF CHECK ==="
git diff --check

echo "=== CONFLICT MARKER SEARCH ==="
grep -rn "<<<<<<<" D3D11Engine/
grep -rn "=======" D3D11Engine/
grep -rn ">>>>>>>" D3D11Engine/

echo "=== REFLECTFRESNEL COUNT IN PSMain ==="
grep -n "float reflectFresnel =" D3D11Engine/Shaders/PS_Water.hlsl

echo "=== RAIN RING FUNCTION CHECK ==="
grep -n "AccumulateRainImpactLayer" D3D11Engine/Shaders/PS_PFX_WetGroundSSR.hlsl

