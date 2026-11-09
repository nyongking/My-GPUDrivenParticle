#include "pch.h"
#include "EmitterCompiler.h"
#include <algorithm>
#include <string>
#include <unordered_map>

namespace
{
    // 이름별로 모아두는 임시 기록
    struct Entry
    {
        AttributeKey    key;
        const TypeInfo* type = nullptr;
        int             order = 0;        // 처음 등장한 순서 (배정 순서를 안정적으로)
        bool            read = false;
        bool            written = false;
        std::string     firstUser;
    };

    // 단계별 읽기/쓰기 규칙. 어긋나면 errors / warnings 에 추가
    void CheckAccess(const AttributeKey& k, const std::string& attr, const std::string& who,
        int level, Stage stage, bool isRead, bool isRenderer, CompiledEmitter& out)
    {
        if (isRead)
        {
            // 자기보다 세밀한 단계의 값은 읽을 수 없다
            if (k.ns == Namespace::Particles && level < 2)
                out.errors.push_back(who + ": '" + attr + "' System/Emitter 단계에서는 파티클 값을 읽을 수 없음");
            if (k.ns == Namespace::Emitter && level < 1)
                out.errors.push_back(who + ": '" + attr + "' System 단계에서는 Emitter 값을 읽을 수 없음");

            if (!isRenderer && stage == Stage::ParticleSpawn && k.mod == Modifier::Previous)
                out.warnings.push_back(who + ": '" + attr + "' Spawn 단계에서는 Previous 가 아직 채워지지 않음");
            if (!isRenderer && stage == Stage::ParticleSpawn && k.mod == Modifier::Initial)
                out.warnings.push_back(who + ": '" + attr + "' Initial 은 Spawn 이 끝날 때 복사되므로 Spawn 안에서는 비어 있음");
            return;
        }

        // 쓰기: 자기 단계의 값만 쓸 수 있다
        if (k.ns == Namespace::Engine)
            out.errors.push_back(who + ": '" + attr + "' Engine 값은 쓸 수 없음");
        if (k.ns == Namespace::User)
            out.errors.push_back(who + ": '" + attr + "' User 값은 외부에서만 설정");
        if (k.mod != Modifier::None)
            out.errors.push_back(who + ": '" + attr + "' Previous/Initial 은 엔진이 채움 (직접 쓰기 금지)");
        if (k.ns == Namespace::Particles && level != 2)
            out.errors.push_back(who + ": '" + attr + "' 파티클 값은 Particle 단계에서만 쓸 수 있음");
        if (k.ns == Namespace::Emitter && level != 1)
            out.errors.push_back(who + ": '" + attr + "' Emitter 값은 Emitter 단계에서만 쓸 수 있음");
        if (k.ns == Namespace::System && level != 0)
            out.errors.push_back(who + ": '" + attr + "' System 값은 System 단계에서만 쓸 수 있음");
    }
}

std::string ModuleScope(size_t moduleIndex, const std::string& moduleName)
{
    std::string scope = "M" + std::to_string(moduleIndex) + "_";
    for (char ch : moduleName)
    {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_';
        if (ok) scope += ch;
    }
    return scope;
}

namespace
{
    // 모듈 입력값을 상수 버퍼 배치표로 (모듈 순서, 선언 순서 그대로)
    void CollectModuleParams(const std::vector<ModuleDesc>& modules, CompiledEmitter& out)
    {
        for (size_t mi = 0; mi < modules.size(); ++mi)
        {
            const ModuleDesc& m = modules[mi];
            const std::string scope = ModuleScope(mi, m.name);
            out.moduleScopes.push_back(scope);

            CompiledModule cm;
            cm.name = m.name;
            cm.scope = scope;
            cm.stage = m.stage;
            cm.hlsl = m.hlsl;
            cm.reads = m.reads;
            cm.writes = m.writes;
            for (size_t pi = 0; pi < m.params.size(); ++pi)
            {
                // 모듈이 직접 쓰는 작은 salt(0, 1, 2 ...)와 겹치면 값들이 상관되므로 대역을 분리한다
                static constexpr uint32_t kSaltBase = 0x10000u;
                static constexpr uint32_t kSaltPerModule = 0x100u;
                static constexpr uint32_t kSaltPerParam = 4u;      // float4 성분 수

                const ParamDesc& p = m.params[pi];
                cm.params.push_back(p.name);
                cm.paramInfos.push_back({ p.name, p.type, p.kind, p.perComponent,
                    kSaltBase + uint32_t(mi) * kSaltPerModule + uint32_t(pi) * kSaltPerParam });
            }
            out.modules.push_back(cm);

            for (size_t pi = 0; pi < m.params.size(); ++pi)
            {
                const ParamDesc& p = m.params[pi];
                const std::vector<std::string> slotNames = ParamSlotNames(p.name, p.kind);
                const std::string who = m.name + " 입력값 '" + p.name + "'";

                if (p.type == nullptr) { out.errors.push_back(who + ": 타입이 지정되지 않음"); continue; }
                if (p.value.size() != size_t(p.type->size) * slotNames.size()) { out.errors.push_back(who + ": 처음 값 크기(" + std::to_string(p.value.size()) + ")가 타입 크기와 다름"); continue; }

                if (p.kind == ParamKind::RandomRange)
                {
                    if (!IsFloatFamily(p.type)) { out.errors.push_back(who + ": RandomRange 는 float 계열 타입만 지원"); continue; }
                    if (m.stage != Stage::ParticleSpawn) { out.errors.push_back(who + ": RandomRange 는 ParticleSpawn 단계에서만 사용 가능 (" + StageName(m.stage) + ")"); continue; }
                }

                std::string why;
                if (!Types::IsGpuCompatible(*p.type, &why)) { out.errors.push_back(who + ": GPU 상수 버퍼에 못 넣는 타입 (" + why + ")"); continue; }

                bool dup = false;
                for (size_t k = 0; k < pi; ++k)
                    for (const std::string& other : ParamSlotNames(m.params[k].name, m.params[k].kind))
                        dup |= std::find(slotNames.begin(), slotNames.end(), other) != slotNames.end();
                if (dup) { out.errors.push_back(who + ": 같은 모듈 안에 같은 이름이 또 있음"); continue; }

                for (const std::string& slotName : slotNames)
                    out.moduleParams.Add(AttributeKey::Parse("Module." + scope + "." + slotName), p.type);
            }
        }

        // 처음 값 채우기 (상수 버퍼 크기 = 16 의 배수)
        FillModuleParamData(modules, out);
    }
}

void FillModuleParamData(const std::vector<ModuleDesc>& modules, CompiledEmitter& compiled)
{
    compiled.moduleParamData.assign(compiled.moduleParams.ConstantBufferSize(), 0);
    for (size_t mi = 0; mi < modules.size(); ++mi)
        for (const ParamDesc& p : modules[mi].params)
        {
            const std::vector<std::string> slotNames = ParamSlotNames(p.name, p.kind);
            for (size_t si = 0; si < slotNames.size(); ++si)
                if (const Slot* s = compiled.moduleParams.Find("Module." + compiled.moduleScopes[mi] + "." + slotNames[si]))
                    if (p.value.size() == size_t(s->type->size) * slotNames.size())
                        std::memcpy(compiled.moduleParamData.data() + s->offset, p.value.data() + si * s->type->size, s->type->size);
        }
}

CompiledEmitter EmitterCompiler::Compile() const
{
    CompiledEmitter out;
    CollectModuleParams(m_Modules, out);
    std::unordered_map<std::string, Entry> table;
    int nextOrder = 0;

    // ---------- [1] + [2] 모으기, 규칙 검사, 중복 제거 ----------
    auto collect = [&](const Usage& u, const std::string& user, int level, Stage stage,
        bool isRead, bool isRenderer)
        {
            const AttributeKey k = AttributeKey::Parse(u.attr);
            const std::string  who = isRenderer ? user : user + "(" + StageName(stage) + ")";

            if (u.type == nullptr)
            {
                out.errors.push_back(who + ": '" + u.attr + "' 타입이 지정되지 않음");
                return;
            }
            if (k.ns == Namespace::Unknown || k.name.empty())
            {
                out.errors.push_back(who + ": '" + u.attr + "' 네임스페이스를 알 수 없음");
                return;
            }
            if (k.mod != Modifier::None && k.ns != Namespace::Particles)
            {
                out.errors.push_back(who + ": '" + u.attr + "' Previous/Initial 은 Particles 에만 붙일 수 있음");
                return;
            }
            CheckAccess(k, u.attr, who, level, stage, isRead, isRenderer, out);

            const std::string full = k.Full();
            auto it = table.find(full);
            if (it == table.end())
            {
                // 처음 보는 값: GPU 로 옮길 수 있는 타입인지 확인
                std::string why;
                if (!Types::IsGpuCompatible(*u.type, &why))
                    out.errors.push_back(who + ": '" + full + "' 타입 " + u.type->name + " 은 GPU 버퍼에 못 넣음 (" + why + ")");

                Entry e;
                e.key = k;
                e.type = u.type;
                e.order = nextOrder++;
                e.firstUser = who;
                it = table.emplace(full, e).first;
            }
            else if (it->second.type != u.type)
            {
                out.errors.push_back("타입 충돌: '" + full + "' " + it->second.firstUser + " 는 " +
                    it->second.type->name + ", " + who + " 는 " + u.type->name);
            }

            if (isRead) it->second.read = true;
            else        it->second.written = true;
        };

    for (const ModuleDesc& m : m_Modules)
    {
        const int lv = StageLevel(m.stage);
        for (const Usage& u : m.writes) collect(u, m.name, lv, m.stage, false, false);
        for (const Usage& u : m.reads)  collect(u, m.name, lv, m.stage, true, false);
    }
    for (const RendererDesc& r : m_Renderers)
        for (const Usage& u : r.reads) collect(u, r.name, 2, Stage::ParticleUpdate, true, true);

    // 순서 의존성 경고: Spawn 에서 앞선 모듈이 쓰지 않은 속성을 읽으면 0 을 읽게 된다
    {
        std::vector<std::string> written;
        for (const ModuleDesc& m : m_Modules)
        {
            if (m.stage != Stage::ParticleSpawn)
                continue;

            for (const Usage& u : m.reads)
            {
                const AttributeKey k = AttributeKey::Parse(u.attr);
                if (k.ns == Namespace::Particles && k.mod == Modifier::None
                    && std::find(written.begin(), written.end(), k.Full()) == written.end())
                    out.warnings.push_back(m.name + ": reads '" + k.Full() + "' before any earlier Spawn module writes it (check module order)");
            }
            for (const Usage& u : m.writes)
                written.push_back(AttributeKey::Parse(u.attr).Full());
        }
    }

    // ---------- [3] Previous/Initial: 원본이 있어야 하고, 원본은 복사 때문에 "읽힘" ----------
    for (auto& kv : table)
    {
        const std::string& full = kv.first;
        Entry& e = kv.second;
        if (e.key.mod == Modifier::None || !e.read) continue;

        const std::string baseName = e.key.Base().Full();
        auto base = table.find(baseName);
        if (base == table.end() || !base->second.written)
        {
            out.errors.push_back("'" + full + "' 를 쓰려면 누군가 '" + baseName + "' 를 써야 함 (복사할 원본 없음)");
            continue;
        }
        if (base->second.type != e.type)
        {
            out.errors.push_back("'" + full + "' 와 원본 '" + baseName + "' 의 타입이 다름");
            continue;
        }
        base->second.read = true;   // 엔진 복사가 원본을 읽는다 -> Trim 되지 않게
        e.written = true;           // 엔진이 채운다 -> "아무도 안 씀" 경고 대상 아님
    }

    if (!out.Ok()) return out;

    // ---------- [4] 처음 등장 순서대로, 저장 위치별로 자리 배정 ----------
    std::vector<const Entry*> ordered;
    ordered.reserve(table.size());
    for (const auto& kv : table) ordered.push_back(&kv.second);
    std::sort(ordered.begin(), ordered.end(),
        [](const Entry* a, const Entry* b) { return a->order < b->order; });

    for (const Entry* e : ordered)
    {
        const std::string full = e->key.Full();

        // 생성기용: 선언된 속성 전부 기록 (Trim 여부와 상관없이)
        {
            const Storage st = e->key.GetStorage();
            const bool trimmedHere = (st == Storage::ParticleBuffer) && m_Trim && !e->read;
            DeclaredAttribute d{ e->key, e->type, !trimmedHere };
            if (st == Storage::ParticleBuffer) out.declaredParticles.push_back(d);
            if (st == Storage::EmitterStore)   out.declaredEmitter.push_back(d);
        }
        switch (e->key.GetStorage())
        {
        case Storage::ParticleBuffer:
            if (m_Trim && !e->read) { out.trimmed.push_back(full); break; }   // Trim
            if (e->read && !e->written)
                out.warnings.push_back("'" + full + "' 읽기만 하고 아무도 쓰지 않음 (기본값 0 사용)");
            out.particles.Add(e->key, e->type);
            break;
        case Storage::EmitterStore:
            if (e->read && !e->written)
                out.warnings.push_back("'" + full + "' 읽기만 하고 아무도 쓰지 않음 (기본값 0 사용)");
            out.emitterParams.Add(e->key, e->type);
            break;
        case Storage::SystemStore:
            out.systemParams.Add(e->key, e->type);
            break;
        case Storage::Temporary:
            if (e->read && !e->written)
                out.warnings.push_back("'" + full + "' 임시값을 읽지만 아무도 쓰지 않음");
            out.temporaries.push_back(full);
            break;
        case Storage::EngineProvided:
            out.engineValues.push_back(full);
            break;
        default:
            break;
        }
    }

    // ---------- [5] 복사 계획: 자리가 확정된 뒤라 offset 을 알 수 있다 ----------
    for (const Slot& s : out.particles.Slots())
    {
        if (s.key.mod == Modifier::None) continue;

        const Slot* base = out.particles.Find(s.key.Base().Full());
        CopyOp op;
        op.from = base->key.Full();
        op.to = s.key.Full();
        op.srcOffset = base->offset;
        op.dstOffset = s.offset;
        op.size = s.type->size;

        if (s.key.mod == Modifier::Previous) out.copyAtUpdateStart.push_back(op);
        else                                 out.copyAtSpawnEnd.push_back(op);
    }
    return out;
}
