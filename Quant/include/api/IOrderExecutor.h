#pragma once
#include "core/Types.h"
#include <string>

// KIS API 또는 테스트 stub 중 어느 것이든 OrderRouter에 주입 가능한 추상 인터페이스
class IOrderExecutor
{
public:
    virtual ~IOrderExecutor() = default;
    virtual std::string submit_order(const OrderSignal& sig) = 0;
};
