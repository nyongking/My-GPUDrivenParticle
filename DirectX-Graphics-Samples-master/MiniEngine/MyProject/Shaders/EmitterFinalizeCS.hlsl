#include "ModuleCommon.hlsli"

[numthreads(1, 1, 1)]
void main()
{
    const uint alive = min(Counters.Load(COUNTER_WRITE_CURSOR), Capacity);

    Counters.Store(COUNTER_ALIVE_IN, alive);
    Counters.Store(COUNTER_WRITE_CURSOR, alive);

    // D3D12_DRAW_ARGUMENTS: VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation
    IndirectArgs.Store4(0, uint4(6, alive, 0, 0));
}
