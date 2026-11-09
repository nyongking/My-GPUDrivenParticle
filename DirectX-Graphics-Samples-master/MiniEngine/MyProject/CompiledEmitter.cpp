#include "pch.h"
#include "CompiledEmitter.h"

#include <sstream>

namespace
{
    void DescribeBlock(std::ostringstream& os, const char* title, const Block& b)
    {
        os << "  [" << title << "] " << b.Stride() << " bytes\n";
        for (const Slot& s : b.Slots())
        {
            os << "     " << s.key.Full()
                << "  (" << s.type->name << ", " << s.type->size << "B)"
                << "  offset " << s.offset << "\n";
        }
    }

    void DescribeCopy(std::ostringstream& os, const char* when, const CopyOp& op)
    {
        os << "  [복사: " << when << "] " << op.to << " <- " << op.from
            << "  (offset " << op.srcOffset << " -> " << op.dstOffset << ", " << op.size << "B)\n";
    }
}

std::string DescribeCompiled(const CompiledEmitter& c, const std::string& title)
{
    std::ostringstream os;
    os << "=== " << title << " ===\n";

    for (const std::string& e : c.errors) os << "  [에러] " << e << "\n";
    if (!c.Ok()) return os.str();

    DescribeBlock(os, "파티클 1개", c.particles);
    DescribeBlock(os, "Emitter 저장소", c.emitterParams);
    DescribeBlock(os, "System 저장소", c.systemParams);
    DescribeBlock(os, "모듈 입력값 (상수 버퍼)", c.moduleParams);

    for (const CopyOp& op : c.copyAtSpawnEnd)    DescribeCopy(os, "Spawn 끝", op);
    for (const CopyOp& op : c.copyAtUpdateStart) DescribeCopy(os, "Update 시작", op);

    for (const std::string& t : c.temporaries)  os << "  [임시, 버퍼 없음] " << t << "\n";
    for (const std::string& t : c.engineValues) os << "  [엔진 제공] " << t << "\n";
    for (const std::string& t : c.trimmed)      os << "  [제거됨] " << t << " (아무도 읽지 않음)\n";
    for (const std::string& w : c.warnings)     os << "  [경고] " << w << "\n";

    return os.str();
}
