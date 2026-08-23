#ifndef D3D12LOOKDEVPT_PATH_TRACING_SAMPLING_HLSLI
#define D3D12LOOKDEVPT_PATH_TRACING_SAMPLING_HLSLI

// Shared deterministic sampling contract for the path tracer and RTXDI.
// Callers reserve non-overlapping dimension ranges for camera, bounce, and
// reservoir uses; the global benchmark seed is carried in materialFocusOptions.
uint Hash(uint value)
{
    value ^= value >> 16u;
    value *= 2246822519u;
    value ^= value >> 13u;
    value *= 3266489917u;
    value ^= value >> 16u;
    return value;
}

// The first two reversed-bit Sobol dimensions are sufficient for Burley's
// padded-dimension construction. Higher logical dimensions get independent
// index shuffles and Owen scrambles instead of growing a direction table.
static const uint SobolBurleyDirection1[32] =
{
    0x00000001u, 0x00000003u, 0x00000005u, 0x0000000fu,
    0x00000011u, 0x00000033u, 0x00000055u, 0x000000ffu,
    0x00000101u, 0x00000303u, 0x00000505u, 0x00000f0fu,
    0x00001111u, 0x00003333u, 0x00005555u, 0x0000ffffu,
    0x00010001u, 0x00030003u, 0x00050005u, 0x000f000fu,
    0x00110011u, 0x00330033u, 0x00550055u, 0x00ff00ffu,
    0x01010101u, 0x03030303u, 0x05050505u, 0x0f0f0f0fu,
    0x11111111u, 0x33333333u, 0x55555555u, 0xffffffffu
};

uint SamplingMix32(uint value)
{
    value ^= value >> 16u;
    value *= 0x21f0aaadu;
    value ^= value >> 15u;
    value *= 0xd35a2d97u;
    value ^= value >> 15u;
    return value ^ 0xe6fe3bebu;
}

// Practical hash-based Owen scrambling operates in reversed-bit order.
uint ReversedBitOwen(uint value, uint seed)
{
    value ^= value * 0x3d20adeau;
    value += seed;
    value *= (seed >> 16u) | 1u;
    value ^= value * 0x05526c56u;
    value ^= value * 0x53a22864u;
    return value;
}

uint SobolBurleyBits(uint reversedIndex, uint dimension, uint scrambleSeed)
{
    uint value = 0u;
    if (dimension == 0u)
    {
        value = reversebits(reversedIndex);
    }
    else
    {
        [unroll]
        for (uint bit = 0u; bit < 32u; ++bit)
        {
            if ((reversedIndex & (0x80000000u >> bit)) != 0u)
            {
                value ^= SobolBurleyDirection1[bit];
            }
        }
    }

    return reversebits(ReversedBitOwen(value, scrambleSeed));
}

float SamplingUintToUnitFloat(uint value)
{
    // Retaining the high 24 bits avoids rounding 0xffffffff to exactly 1.0f.
    return (float)(value >> 8u) * (1.0f / 16777216.0f);
}

uint SamplingPixelSeed(uint2 pixel)
{
    uint globalSeed = (uint)round(g_scene.materialFocusOptions.z) |
        ((uint)round(g_scene.materialFocusOptions.w) << 16u);
    return SamplingMix32(pixel.x ^ SamplingMix32(pixel.y + 0x9e3779b9u) ^ SamplingMix32(globalSeed));
}

float OwenScrambledSobol1D(uint2 pixel, uint sampleIndex, uint dimension)
{
    uint seed = SamplingPixelSeed(pixel) ^ SamplingMix32(dimension);
    uint shuffledIndex = ReversedBitOwen(reversebits(sampleIndex), seed ^ 0xbff95bfeu);
    return SamplingUintToUnitFloat(SobolBurleyBits(shuffledIndex, 0u, seed ^ 0x635c77bdu));
}

float2 OwenScrambledSobol2D(uint2 pixel, uint sampleIndex, uint dimensionSet)
{
    uint seed = SamplingPixelSeed(pixel) ^ SamplingMix32(dimensionSet);
    uint shuffledIndex = ReversedBitOwen(reversebits(sampleIndex), seed ^ 0xf8ade99au);
    return float2(
        SamplingUintToUnitFloat(SobolBurleyBits(shuffledIndex, 0u, seed ^ 0xe0aaaf76u)),
        SamplingUintToUnitFloat(SobolBurleyBits(shuffledIndex, 1u, seed ^ 0x94964d4eu)));
}

float BlueNoise01(uint2 pixel, uint frame, uint dimension)
{
    return OwenScrambledSobol1D(pixel, frame, dimension);
}

float StableSample01(uint2 pixel, uint frame, uint sampleIndex, uint dimension)
{
    return OwenScrambledSobol1D(pixel, frame * 32u + sampleIndex, dimension);
}

#endif
