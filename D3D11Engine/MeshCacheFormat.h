#pragma once

#include <cstddef>
#include <cstdint>

namespace MeshCacheFormat {
    constexpr int32_t LegacyVersion = 1;
    constexpr int32_t CurrentVersion = 2;
    constexpr uint32_t Magic = 0x3248434Du; // "MCH2" in little-endian files.

    constexpr uint64_t MaxFileBytes = 512ull * 1024ull * 1024ull;
    constexpr uint64_t MaxDecodedBytes = 512ull * 1024ull * 1024ull;
    constexpr uint32_t MaxTextures = 4096;
    constexpr uint32_t MaxTextureNameBytes = 4096;
    constexpr uint32_t MaxSubmeshesPerTexture = 65535;
    constexpr uint32_t MaxVerticesPerSubmesh = 65535;
    constexpr uint32_t MaxIndicesPerSubmesh = 16u * 1024u * 1024u;
}