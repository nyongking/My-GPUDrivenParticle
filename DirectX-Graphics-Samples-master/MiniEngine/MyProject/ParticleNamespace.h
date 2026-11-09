#pragma once

#include <string>

enum class Namespace { Particles, Emitter, System, User, Engine, Transient, Output, Local, Module, Unknown };
enum class Modifier { None, Previous, Initial };
enum class Storage { ParticleBuffer, EmitterStore, SystemStore, EngineProvided, Temporary, ModuleParam, Invalid };

inline Namespace ParseNamespace(const std::string& s)
{
    if (s == "Particles") return Namespace::Particles;
    if (s == "Emitter")   return Namespace::Emitter;
    if (s == "System")    return Namespace::System;
    if (s == "User")      return Namespace::User;
    if (s == "Engine")    return Namespace::Engine;
    if (s == "Transient") return Namespace::Transient;
    if (s == "Output")    return Namespace::Output;
    if (s == "Local")     return Namespace::Local;
    if (s == "Module")    return Namespace::Module;
    return Namespace::Unknown;
}

inline const char* NamespaceName(Namespace n)
{
    switch (n) {
    case Namespace::Particles: return "Particles";
    case Namespace::Emitter:   return "Emitter";
    case Namespace::System:    return "System";
    case Namespace::User:      return "User";
    case Namespace::Engine:    return "Engine";
    case Namespace::Transient: return "Transient";
    case Namespace::Output:    return "Output";
    case Namespace::Local:     return "Local";
    case Namespace::Module:    return "Module";
    default:                   return "?";
    }
}