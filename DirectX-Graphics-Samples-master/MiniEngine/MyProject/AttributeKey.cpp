#include "pch.h"
#include "AttributeKey.h"

AttributeKey AttributeKey::Parse(const std::string& full)
{
    std::vector<std::string> tok;
    std::stringstream ss(full);
    std::string part;

    while (std::getline(ss, part, '.')) tok.push_back(part);

    AttributeKey k;
    if (tok.size() < 2) return k;
    k.ns = ParseNamespace(tok[0]);

    size_t i = 1;
    if (tok.size() > 2) {
        if (tok[1] == "Previous") { k.mod = Modifier::Previous; i = 2; }
        else if (tok[1] == "Initial") { k.mod = Modifier::Initial; i = 2; }
    }
    for (; i < tok.size(); ++i) {
        if (!k.name.empty()) k.name += ".";
        k.name += tok[i];
    }
    return k;
}

std::string AttributeKey::Full() const
{
    std::string s = NamespaceName(ns);
    if (mod == Modifier::Previous) s += ".Previous";
    if (mod == Modifier::Initial)  s += ".Initial";
    return s + "." + name;
}

AttributeKey AttributeKey::Base() const
{
    AttributeKey b = *this;
    b.mod = Modifier::None;
    return b;
}

Storage AttributeKey::GetStorage() const
{
    switch (ns)
    {
    case Namespace::Particles: return Storage::ParticleBuffer;   // 파티클마다 1개 (SoA 버퍼)
    case Namespace::Emitter:   return Storage::EmitterStore;     // Emitter 당 1개
    case Namespace::System:
    case Namespace::User:      return Storage::SystemStore;      // System 당 1개
    case Namespace::Engine:    return Storage::EngineProvided;   // 엔진이 줌 (DeltaTime 등)
    case Namespace::Transient:
    case Namespace::Output:
    case Namespace::Local:     return Storage::Temporary;        // 단계 안에서만 쓰는 임시값
    case Namespace::Module:    return Storage::ModuleParam;      // 모듈 안에서
    default:                   return Storage::Invalid;
    }
}
