#!/bin/bash

echo "=== git diff --check ==="
"C:/Users/winkler.WS/Documents/Antigravity/Projekte/tools/PortableGit-2.55.0.2/bin/git.exe" diff --check

echo "=== Conflict Markers ==="
grep -rn "<<<<<<< " D3D11Engine/ 2>/dev/null || true
grep -rn "=======" D3D11Engine/ 2>/dev/null || true
grep -rn ">>>>>>> " D3D11Engine/ 2>/dev/null || true

echo "=== materialPuddleEligibility ==="
grep -rn "materialPuddleEligibility" D3D11Engine/

echo "=== materialWetMask ==="
grep -rn "materialWetMask" D3D11Engine/

echo "=== Check if any Puddle depends on Normal/POM/etc ==="
grep -riE "AllowNormalmaps|EnableParallaxOcclusionMapping|NormalmapStrength|DisplacementFactor" D3D11Engine/Shaders/PS_PFX_WetGroundSSR.hlsl || true

echo "=== Check Syntax PS_PFX_WetGroundSSR.hlsl ==="
"C:/Users/winkler.WS/Documents/Antigravity/Projekte/tools/PortableGit-2.55.0.2/usr/bin/awk.exe" 'BEGIN {p=0} /\{/ {p++} /\}/ {p--} END {print "Braces PS_PFX_WetGroundSSR: " p}' D3D11Engine/Shaders/PS_PFX_WetGroundSSR.hlsl

echo "=== Check Syntax PS_Water.hlsl ==="
"C:/Users/winkler.WS/Documents/Antigravity/Projekte/tools/PortableGit-2.55.0.2/usr/bin/awk.exe" 'BEGIN {p=0} /\{/ {p++} /\}/ {p--} END {print "Braces PS_Water: " p}' D3D11Engine/Shaders/PS_Water.hlsl
