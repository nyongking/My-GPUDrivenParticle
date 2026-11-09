#include "pch.h"
#include "ModuleDesc.h"

int StageLevel(Stage s)
{
    switch (s)
    {
    case Stage::SystemSpawn:
    case Stage::SystemUpdate:  return 0;
    case Stage::EmitterSpawn:
    case Stage::EmitterUpdate: return 1;
    default:                   return 2;
    }
}

const char* StageName(Stage s)
{
    switch (s)
    {
    case Stage::SystemSpawn:    return "SystemSpawn";
    case Stage::SystemUpdate:   return "SystemUpdate";
    case Stage::EmitterSpawn:   return "EmitterSpawn";
    case Stage::EmitterUpdate:  return "EmitterUpdate";
    case Stage::ParticleSpawn:  return "ParticleSpawn";
    case Stage::ParticleUpdate: return "ParticleUpdate";
    }
    return "?";
}
