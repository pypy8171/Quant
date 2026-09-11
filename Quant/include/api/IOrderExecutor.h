#pragma once
#include "core/Types.h"
#include <string>
#include <vector>

// 주문·취소·정정 한 번의 결과. 성공이면 odno, 실패면 err_code — 한 값에 둘 다 있어
//  호출자가 부채널을 다시 묻지 않는다(D-039).
// [inv] ok() == !odno.empty(). 실패면 err_code가 비지 않는다(KIS msg_cd 또는 kis_err::kTransport·kUnknown).
struct OrderAck
{
    std::string odno;      // KIS 접수번호 (ODNO). 취소·정정은 그 접수번호
    std::string krx_orgno; // KRX_FWDG_ORD_ORGNO — 정정/취소 시 원주문 조직번호로 재입력
    std::string err_code;  // 실패 사유 코드(kis_err). 성공이면 ""

    [[nodiscard]] bool ok() const noexcept { return !odno.empty(); }

    static OrderAck fail(const std::string& code) { return OrderAck{std::string(), std::string(), code}; }
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

    // 신규 주문. ODNO와 KRX 조직번호(정정/취소에 필요)를 캡처한다. 실패면 ok()가 false이고
    //  err_code에 사유가 있다. 결과를 버리면 컴파일러가 알린다 — 접수 여부를 모른 채 넘어가는 경로가 없게.
    [[nodiscard]] virtual OrderAck submit_order_ack(const OrderSignal& sig) = 0;

    // 미체결 취소 (order-rvsecncl, RVSE_CNCL_DVSN_CD="02"). 성공 시 odno=취소접수번호.
    // all_remaining=true → QTY_ALL_ORD_YN="Y" (잔량 전체 취소). 기본은 미지원.
    [[nodiscard]] virtual OrderAck cancel_order(const std::string& /*ticker*/,
                                                const std::string& /*orig_odno*/,
                                                const std::string& /*krx_orgno*/,
                                                int /*qty*/, bool /*all_remaining*/)
    {
        return OrderAck::fail("E_UNSUPPORTED");
    }

    // 정정 (order-rvsecncl, RVSE_CNCL_DVSN_CD="01"). 성공 시 odno=새 ODNO(정정접수번호). 기본은 미지원.
    [[nodiscard]] virtual OrderAck revise_order(const std::string& /*ticker*/,
                                                const std::string& /*orig_odno*/,
                                                const std::string& /*krx_orgno*/,
                                                int /*new_qty*/, double /*new_price*/)
    {
        return OrderAck::fail("E_UNSUPPORTED");
    }

    // 모의투자 서버 여부. 모의는 정정취소가능조회(inquire-psbl-rvsecncl) 등 일부 거래코드(TR)를
    //  미지원("없는 서비스 코드")이라, 호출부가 그 경로(청산차단 자가정리)를 건너뛰도록 노출한다. 기본 false(실전).
    [[nodiscard]] virtual bool is_paper() const noexcept { return false; }

    // 미체결(정정취소 가능) 예약주문 조회 (inquire-psbl-rvsecncl). 기본은 빈 목록.
    //  장중 청산이 "주문가능분 없음"(40240000)으로 막힐 때, 해당 종목의 예약매도를 찾아
    //  취소→재매도로 자가정리하기 위한 조회 경로. 세션 간/수동 예약도 감지 가능.
    [[nodiscard]] virtual std::vector<OpenOrder> get_open_orders() { return {}; }
};
