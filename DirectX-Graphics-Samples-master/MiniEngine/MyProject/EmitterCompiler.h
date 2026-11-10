#pragma once
#include "ModuleDesc.h"
#include "CompiledEmitter.h"

std::string ModuleScope(size_t moduleIndex, const std::string& moduleName);

// 레이아웃은 그대로 두고 modules 의 입력값만 compiled.moduleParamData 에 다시 채운다
void FillModuleParamData(const std::vector<ModuleDesc>& modules, CompiledEmitter& compiled);


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
