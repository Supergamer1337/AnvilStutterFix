#pragma once

#include "common.h"

#include <d3d12.h>

struct CreateParams
{
    ID3D12Device*         device;
    ID3D12Heap*           heap;
    UINT64                offset;
    D3D12_RESOURCE_DESC   desc;
    D3D12_RESOURCE_STATES state;
    bool                  hasClear;
    D3D12_CLEAR_VALUE     clear;
    IID                   riid;
};

struct PoolKey
{
    void*  device;
    void*  heap;
    UINT64 offset, alignment, width;
    UINT   dimension, height, format, sampleCount, sampleQuality, layout, flags, state, clearFormat;
    UINT16 depth, mips;
    bool   hasClear;
    bool operator==(const PoolKey&) const = default;
};

struct PoolKeyHash
{
    size_t operator()(const PoolKey& k) const
    {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&](uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); };
        mix((uint64_t)k.device); mix((uint64_t)k.heap); mix(k.offset); mix(k.alignment); mix(k.width);
        mix(k.dimension); mix(k.height); mix(k.format); mix(k.sampleCount); mix(k.sampleQuality); mix(k.layout);
        mix(k.flags); mix(k.state); mix(k.clearFormat); mix(k.depth); mix(k.mips); mix(k.hasClear);
        return (size_t)h;
    }
};
