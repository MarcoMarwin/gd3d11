#pragma once
#include "pch.h"

class D3D11TemporalState {
public:
    D3D11TemporalState();

    void AdvanceJitter();
    void OnDisabled();

    XMFLOAT2 GetJitterOffset() const { return m_CurrentJitter; }
    XMFLOAT2 GetJitterOffsetUnscaled() const { return m_CurrentJitterUnscaled; }
    const XMFLOAT4X4& GetUnjitteredViewProj() const { return m_UnjitteredViewProj; }

private:
    int m_JitterIndex;
    XMFLOAT2 m_CurrentJitter;
    XMFLOAT2 m_CurrentJitterUnscaled;
    XMFLOAT4X4 m_UnjitteredViewProj;
};