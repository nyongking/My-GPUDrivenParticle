#pragma once
#include "ParticleNamespace.h"

#include <iostream>
#include <sstream>

// Attribute의 진짜 이름 = 네임스페이스 + 수식어 + 이름
struct AttributeKey
{
	Namespace ns = Namespace::Unknown;
	Modifier mod = Modifier::None;
	std::string name;

	// Full Key를 namespace / mod / name 으로 나눈다.
	// Particles.Previous.Position -> Particles / Previous / Position
    static AttributeKey Parse(const std::string& full);

    std::string Full() const;

    AttributeKey Base() const;

	Storage GetStorage() const;
};

