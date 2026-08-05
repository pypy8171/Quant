#pragma once
#include "core/Types.h"
#include <string>

// KIS 접수 응답 — ODNO + KRX 조직번호(정정/취소 필수). (MM-1)
struct OrderAck
{
    std::string odno;      // KIS 접수번호 (ODNO)
    std::string krx_orgno; // KRX_FWDG_ORD_ORGNO — 정정/취소 시 원주문 조직번호로 재입력
};

// KIS API 또는 테스트 stub 중 어느 것이든 OrderRouter에 주입 가능한 추상 인터페이스
class IOrderExecutor
{
public:
    virtual ~IOrderExecutor() = default;

    // 신규 주문 — ODNO 반환, 실패 시 빈 문자열 (기존 계약 유지)
    virtual std::string submit_order(const OrderSignal& sig) = 0;

    // ── MM-1 확장 (비파괴: 기본 구현으로 기존 executor/stub 무변경) ──────────
    // 신규 주문 + orgno까지 캡처. 기본은 submit_order에 위임(orgno 없음).
    // 정정/취소를 낼 주문은 이 경로로 제출해 krx_orgno를 보존해야 한다.
    virtual OrderAck submit_order_ack(const OrderSignal& sig)
    {
        return OrderAck{submit_order(sig), std::string()};
    }

    // 미체결 취소 (order-rvsecncl, RVSE_CNCL_DVSN_CD="02"). 성공 시 취소접수 ODNO, 실패 시 "".
    // all_remaining=true → QTY_ALL_ORD_YN="Y" (잔량 전체 취소).
    virtual std::string cancel_order(const std::string& /*ticker*/,
                                     const std::string& /*orig_odno*/,
                                     const std::string& /*krx_orgno*/,
                                     int /*qty*/, bool /*all_remaining*/)
    {
        return std::string();
    }

    // 정정 (order-rvsecncl, RVSE_CNCL_DVSN_CD="01"). 성공 시 새 ODNO(정정접수번호), 실패 시 "".
    virtual std::string revise_order(const std::string& /*ticker*/,
                                     const std::string& /*orig_odno*/,
                                     const std::string& /*krx_orgno*/,
                                     int /*new_qty*/, double /*new_price*/)
    {
        return std::string();
    }
};
