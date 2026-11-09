#pragma once
#include "ModuleDesc.h"
#include "CompiledEmitter.h"

std::string ModuleScope(size_t moduleIndex, const std::string& moduleName);


class EmitterCompiler
{
public:
    EmitterCompiler& Add(const ModuleDesc& m) { m_Modules.push_back(m);   return *this; }
    EmitterCompiler& Add(const RendererDesc& r) { m_Renderers.push_back(r); return *this; }
    EmitterCompiler& SetTrim(bool enable) { m_Trim = enable;          return *this; }

    CompiledEmitter Compile() const;

private:
    std::vector<ModuleDesc>   m_Modules;
    std::vector<RendererDesc> m_Renderers;
    bool                      m_Trim = true;
};
