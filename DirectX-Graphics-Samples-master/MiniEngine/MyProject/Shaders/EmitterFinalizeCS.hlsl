#include "ModuleCommon.hlsli"

#define FRAME_ARGS_DRAW   0
#define FRAME_ARGS_UPDATE 16

[numthreads(1, 1, 1)]
void main()
{
    const uint alive = min(Counters.Load(COUNTER_WRITE_CURSOR), Capacity);

    Counters.Store(COUNTER_ALIVE_IN, alive);
    Counters.Store(COUNTER_WRITE_CURSOR, 0);

    // D3D12_DRAW_ARGUMENTS: VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation
    IndirectArgs.Store4(FRAME_ARGS_DRAW, uint4(6, alive, 0, 0));
    // D3D12_DISPATCH_ARGUMENTS for next frame's ParticleUpdateMain (0 groups is fine: nothing runs)
    IndirectArgs.Store3(FRAME_ARGS_UPDATE, uint3((alive + GROUP_SIZE - 1) / GROUP_SIZE, 1, 1));
}
