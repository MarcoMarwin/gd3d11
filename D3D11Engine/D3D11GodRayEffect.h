#pragma once

class D3D11PShader;
class D3D11VShader;
class D3D11GodRayEffect {
public:
    D3D11GodRayEffect();
    ~D3D11GodRayEffect();

    D3D11VShader* QuadVS;
    D3D11PShader* QuadPS;
};

