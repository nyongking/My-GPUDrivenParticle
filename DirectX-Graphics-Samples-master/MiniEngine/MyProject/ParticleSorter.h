#pragma once

#include "AttributeType.h"

#include <cstdint>
#include <vector>

enum class SortMode
{
    None,
    ViewDepth, // 카메라 방향 기준 먼것부터
};

struct SortScratch
{
    struct KeyIndex { float key; uint32_t index; };
    std::vector<KeyIndex> items;
};

void SortBackToFront(const float3* positions,
    uint32_t               count,
    const float3& cameraPosition,
    const float3& cameraForward,
    std::vector<uint32_t>& outIndices,   // 최소 count 칸을 assign
    SortScratch& scratch);