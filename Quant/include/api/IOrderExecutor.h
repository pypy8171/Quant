#pragma once
#include "core/Types.h"
#include <string>
#include <vector>

// KIS 접수 응답 — ODNO + KRX 조직번호(정정/취소 필수). (MM-1)
struct OrderAck
{
    std::string odno;      // KIS 접수번호 (ODNO)
    std::string krx_orgno; // KRX_FWDG_ORD_ORGNO — 정정/취소 시 원주문 조직번호로 재입력
};

// 미체결(정정취소 가능) 예약주문 1건 — inquire-psbl-rvsecncl 결과.
//  장중 청산이 "주문가능분 없음"(40240000)으로 막힐 때, 그 종목의 예약매도를 찾아
//  취소→재매도로 자가정리하는 데 쓴다.
struct OpenOrder
{
    std::string ticker;    // pdno — 종목코드
    std::string name;      // prdt_name — 종목명
    std::string odno;      // odno — 취소 시 ORGN_ODNO(원주문번호)로 재입력
    std::string krx_orgno; // ord_gno_brno — 취소 시 KRX_FWDG_ORD_ORGNO(조직번호)로 재입력
    int         psbl_qty = 0;   // psbl_qty — 정정취소 가능 수량(예약으로 묶인 잔량)
    double      ord_unpr = 0.0; // ord_unpr — 주문단가
    OrderSide   side = OrderSide::NONE; // sll_buy_dvsn_cd: 01=매도, 02=매수
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

    // 직전 주문/취소/정정 호출의 KIS 오류코드(msg_cd). 성공 시 "".
    //  초당 거래건수 초과(EGW00201) 등 "접수 전 거부·재발주 안전" 사유를 호출부(OrderRouter/
    //  order_thread)가 판별해 적응적 재시도를 걸 수 있게 노출한다. 기본은 미지원("").
    virtual std::string last_order_error_code() const { return std::string(); }

    // 모의투자 서버 여부. 모의는 정정취소가능조회(inquire-psbl-rvsecncl) 등 일부 TR을 미지원
    //  ("없는 서비스 코드") → 호출부가 그 경로(청산차단 자가정리)를 건너뛰도록 노출. 기본 false(실전).
    virtual bool is_paper() const { return false; }

    // 미체결(정정취소 가능) 예약주문 조회 (inquire-psbl-rvsecncl). 기본은 빈 목록.
    //  장중 청산이 "주문가능분 없음"(40240000)으로 막힐 때, 해당 종목의 예약매도를 찾아
    //  취소→재매도로 자가정리하기 위한 조회 경로. 세션 간/수동 예약도 감지 가능.
    virtual std::vector<OpenOrder> get_open_orders() { return {}; }
};
