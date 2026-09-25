#pragma once
#include "api/KisResult.h"
#include "core/Types.h"
#include <string>
#include <vector>

// 주문·취소·정정 한 번의 결과. 성공이면 kis_order_no, 실패면 error_code — 한 값에 둘 다 있어
//  호출자가 부채널을 다시 묻지 않는다(D-039).
// [inv] ok() == !kis_order_no.empty(). 실패면 error_code가 비지 않는다 — KIS msg_cd, 또는 자체 코드
//  kis_error::kTransport·kUnknown·kLedgerWriteFailed, "E_NO_ORIG_ODNO"(Quant/src/api/KisOrder.cpp), "E_UNSUPPORTED"(아래 기본 구현).
struct OrderAck
{
    std::string kis_order_no;      // KIS 접수번호 (ODNO). 취소·정정은 그 접수번호
    std::string krx_forwarding_org_no; // KRX_FWDG_ORD_ORGNO — 정정/취소 시 원주문 조직번호로 재입력
    std::string error_code;  // 실패 사유 코드(kis_error). 성공이면 ""

    [[nodiscard]] bool ok() const noexcept { return !kis_order_no.empty(); }

    static OrderAck fail(const std::string& code) { return OrderAck{std::string(), std::string(), code}; }
};

// 미체결(정정취소 가능) 예약주문 1건 — inquire-psbl-rvsecncl 결과.
//  장중 청산이 "주문가능분 없음"(40240000)으로 막힐 때, 그 종목의 예약매도를 찾아
//  취소→재매도로 자가정리하는 데 쓴다.
struct OpenOrder
{
    std::string ticker;    // pdno — 종목코드
    std::string name;      // prdt_name — 종목명
    std::string kis_order_no;      // KIS 주문번호(ODNO) — 취소 시 ORGN_ODNO(원주문번호)로 재입력
    std::string krx_forwarding_org_no; // ord_gno_brno — 취소 시 KRX_FWDG_ORD_ORGNO(조직번호)로 재입력
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
    //  error_code에 사유가 있다. 결과를 버리면 컴파일러가 알린다 — 접수 여부를 모른 채 넘어가는 경로가 없게.
    [[nodiscard]] virtual OrderAck submit_order_acknowledgement(const OrderSignal& signal) = 0;

    // 미체결 취소 (order-rvsecncl, RVSE_CNCL_DVSN_CD="02"). 성공 시 kis_order_no=취소접수번호.
    // all_remaining=true → QTY_ALL_ORD_YN="Y" (잔량 전체 취소). 기본은 미지원.
    [[nodiscard]] virtual OrderAck cancel_order(const std::string& /*ticker*/,
                                                const std::string& /*orig_odno*/,
                                                const std::string& /*krx_forwarding_org_no*/,
                                                int /*quantity*/, bool /*all_remaining*/)
    {
        return OrderAck::fail("E_UNSUPPORTED");
    }

    // 정정 (order-rvsecncl, RVSE_CNCL_DVSN_CD="01"). 성공 시 kis_order_no=새 ODNO(정정접수번호). 기본은 미지원.
    [[nodiscard]] virtual OrderAck revise_order(const std::string& /*ticker*/,
                                                const std::string& /*orig_odno*/,
                                                const std::string& /*krx_forwarding_org_no*/,
                                                int /*new_quantity*/, double /*new_price*/)
    {
        return OrderAck::fail("E_UNSUPPORTED");
    }

    // 모의투자 서버 여부. 모의도 get_open_orders는 동작한다(VTTC0081R, D-101). is_paper는 OrderRouter가
    //  모의에서 브로커 대신 라우터 이력을 쓰는 분기(청산차단 예약매도 찾기·기동 시 주문 복원)에 쓴다. 기본 false(실전).
    [[nodiscard]] virtual bool is_paper() const noexcept { return false; }

    // 미체결(정정취소 가능) 예약주문 조회 (inquire-psbl-rvsecncl). 기본은 빈 목록.
    //  장중 청산이 "주문가능분 없음"(40240000)으로 막힐 때, 해당 종목의 예약매도를 찾아
    //  취소→재매도로 자가정리하기 위한 조회 경로. 세션 간/수동 예약도 감지 가능.
    //  [inv] 한 쪽이라도 못 받으면 실패다 — 빈 목록은 "브로커가 미체결 없음이라고 답했다"일 때만 돌려준다.
    //  잘린 목록을 성공으로 넘기면 재기동 대조가 빠진 주문의 선점을 "KIS에 없다"로 풀어 같은 수량을 또 낸다. [why 전수조사 B1-2]
    [[nodiscard]] virtual KisResult<std::vector<OpenOrder>> get_open_orders() { return std::vector<OpenOrder>{}; }

    // 계측: 이 스레드가 브로커 초당 한도 버킷에서 기다린 누적 시간(nanoseconds). 호출자가 전송 전후 차이로 자기 몫을 잰다.
    //  한도가 없는 구현(가짜·페이퍼)은 0. [why D-071]
    [[nodiscard]] virtual std::uint64_t rate_limit_wait_ns_this_thread() const noexcept { return 0; }
};
