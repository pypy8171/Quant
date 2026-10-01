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
    // [wire] 출처: KIS 공식 샘플 order_cash(응답 KRX_FWDG_ORD_ORGNO·ODNO·ORD_TMD)와 order_rvsecncl(필수 입력
    //  KRX_FWDG_ORD_ORGNO·ORGN_ODNO), 2026-09-27 MCP 확인.
    std::string error_code;  // 실패 사유 코드(kis_error). 성공이면 ""

    [[nodiscard]] bool ok() const noexcept
    {
        return !kis_order_no.empty();
    }

    static OrderAck fail(const std::string& code)
    {
        return OrderAck{std::string(), std::string(), code};
    }
};

// 미체결(정정취소 가능) 예약주문 1건 — inquire-psbl-rvsecncl 결과.
//  장중 청산이 "주문가능분 없음"(40240000)으로 막힐 때, 그 종목의 예약매도를 찾아
//  취소→재매도로 자가정리하는 데 쓴다.
// 근거: 실측 — 40240000 거부는 D-046(모의, "잔고내역 없음")·D-055, 예약매도가 수량을 잠근 사례는 DAILY_LOG.md
//  2026-08-12 항목. 공식 샘플에는 오류코드 목록이 없다(2026-09-27 MCP 확인).
struct OpenOrder
{
    std::string ticker;    // pdno — 종목코드
    std::string name;      // prdt_name — 종목명
    std::string kis_order_no;      // KIS 주문번호(ODNO) — 취소 시 ORGN_ODNO(원주문번호)로 재입력
    std::string krx_forwarding_org_no; // ord_gno_brno — 취소 시 KRX_FWDG_ORD_ORGNO(조직번호)로 재입력
    // [wire] 근거 약함(2026-09-27): 공식 샘플은 ord_gno_brno를 "주문채번지점번호"(inquire_psbl_rvsecncl·
    //  inquire_daily_ccld), KRX_FWDG_ORD_ORGNO를 "한국거래소전송주문조직번호"(order_rvsecncl)로 적고, 둘이 같은 값이라는
    //  설명은 없다. 이 경로로 낸 취소가 접수된 실측 기록도 찾지 못했다.
    int         psbl_qty = 0;   // psbl_qty — 정정취소 가능 수량(예약으로 묶인 잔량)
    double      ord_unpr = 0.0; // ord_unpr — 주문단가
    OrderSide   side = OrderSide::NONE; // sll_buy_dvsn_cd: 01=매도, 02=매수
    // [wire] 출처: 공식 샘플 inquire_psbl_rvsecncl 응답 컬럼(ord_gno_brno·odno·pdno·prdt_name·psbl_qty·ord_unpr·
    //  sll_buy_dvsn_cd), 코드값 01 매도·02 매수는 inquire_daily_ccld 파라미터 설명, 2026-09-27 MCP 확인.
    std::string exchange;       // excg_id_dvsn_cd — 원주문이 나간 거래소(KRX·NXT·SOR). 취소는 이 거래소로 낸다
    std::string order_division; // ord_dvsn_cd — 원주문 주문구분(00 지정가·41 애프터마켓 지정가 등)
    // [wire] 출처: 공식 샘플 chk_inquire_psbl_rvsecncl 응답 컬럼 excg_id_dvsn_cd(거래소ID구분코드)·ord_dvsn_cd(주문구분코드),
    //  2026-10-01 MCP 확인. 모의(VTTC0081R) 응답에도 같은 이름이 오는지는 확인하지 못했다 — 비면 지금 구간 규칙을 쓴다.
};

// 일별주문체결조회 한 행 — 주문 하나의 오늘 누적 체결. 체결 한 건이 아니라 주문번호별 합계다.
//  소켓이 끊긴 사이 놓친 체결을 되찾는 데 쓴다(OrderRouter::recover_missed_fills). [why D-149]
struct DailyOrderFill
{
    std::string kis_order_no;              // odno
    std::string original_order_no;         // orgn_odno. 정정·취소 행이면 고친 대상, 신규는 빈 값
    std::string ticker;                    // pdno
    OrderSide   side = OrderSide::NONE;    // sll_buy_dvsn_cd: 01=매도, 02=매수
    // [wire] 출처: 공식 샘플 inquire_daily_ccld(파라미터 설명 00 전체/01 매도/02 매수, 응답 컬럼 odno·orgn_odno·
    //  pdno·ord_qty·tot_ccld_qty·tot_ccld_amt), 2026-09-27 MCP 확인.
    int         order_quantity  = 0;       // ord_qty
    int         filled_quantity = 0;       // tot_ccld_qty — 오늘 누적 체결 수량
    int64_t     filled_amount   = 0;       // tot_ccld_amt — 오늘 누적 체결 금액(원)
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
    // [wire] 출처: KIS 공식 샘플 order_rvsecncl(TTTC0013U/VTTC0013U) — 정정취소구분코드 01 정정·02 취소,
    //  잔량전부주문여부 Y 전량·N 일부, 2026-09-27 MCP 확인.
    [[nodiscard]] virtual OrderAck cancel_order(const std::string& /*ticker*/,
                                                const std::string& /*orig_odno*/,
                                                const std::string& /*krx_forwarding_org_no*/,
                                                int /*quantity*/, bool /*all_remaining*/)
    {
        return OrderAck::fail("E_UNSUPPORTED");
    }

    // 정정 (order-rvsecncl, RVSE_CNCL_DVSN_CD="01"). 성공 시 kis_order_no=새 ODNO(정정접수번호). 기본은 미지원.
    // [wire] 코드값 01은 위와 같은 샘플. 정정 응답 output의 ODNO가 새 번호라는 것은 샘플에 응답 컬럼 설명이 없어
    //  확인하지 못했다(2026-09-27). 샘플은 "정정은 원주문의 주문단가 혹은 주문구분을 변경, 수량은 원주문수량 이하"라고 적는다.
    [[nodiscard]] virtual OrderAck revise_order(const std::string& /*ticker*/,
                                                const std::string& /*orig_odno*/,
                                                const std::string& /*krx_forwarding_org_no*/,
                                                int /*new_quantity*/, double /*new_price*/)
    {
        return OrderAck::fail("E_UNSUPPORTED");
    }

    // 모의투자 서버 여부. 모의도 get_open_orders는 동작한다(VTTC0081R, D-101). is_paper는 OrderRouter가
    //  모의에서 브로커 대신 라우터 이력을 쓰는 분기(청산차단 예약매도 찾기·기동 시 주문 복원)에 쓴다. 기본 false(실전).
    [[nodiscard]] virtual bool is_paper() const noexcept
    {
        return false;
    }

    // 미체결(정정취소 가능) 예약주문 조회 (inquire-psbl-rvsecncl). 기본은 빈 목록.
    //  장중 청산이 "주문가능분 없음"(40240000)으로 막힐 때, 해당 종목의 예약매도를 찾아
    //  취소→재매도로 자가정리하기 위한 조회 경로. 세션 간/수동 예약도 감지 가능.
    //  근거: 40240000은 실측 — D-046·D-055. 조회 TR TTTC0084R은 공식 샘플 inquire_psbl_rvsecncl, 2026-09-27 MCP 확인.
    //  [inv] 한 쪽이라도 못 받으면 실패다 — 빈 목록은 "브로커가 미체결 없음이라고 답했다"일 때만 돌려준다.
    //  잘린 목록을 성공으로 넘기면 재기동 대조가 빠진 주문의 선점을 "KIS에 없다"로 풀어 같은 수량을 또 낸다. [why 전수조사 B1-2]
    [[nodiscard]] virtual KisResult<std::vector<OpenOrder>> get_open_orders()
    {
        return std::vector<OpenOrder>{};
    }

    // 오늘 체결이 있는 주문의 누적 체결 (inquire-daily-ccld, CCLD_DVSN=01). 기본은 빈 목록.
    // [wire] 출처: 공식 샘플 inquire_daily_ccld — 체결구분 00 전체/01 체결/02 미체결, TR TTTC0081R(실전)·VTTC0081R(모의),
    //  2026-09-27 MCP 확인.
    //  [inv] 쪽을 하나라도 못 받으면 실패다 — 잘린 목록으로 차이를 재면 받은 쪽의 주문만 되찾고 나머지는 모른 채 넘어간다.
    [[nodiscard]] virtual KisResult<std::vector<DailyOrderFill>> get_daily_order_fills()
    {
        return std::vector<DailyOrderFill>{};
    }

    // 계측: 이 스레드가 브로커 초당 한도 버킷에서 기다린 누적 시간(nanoseconds). 호출자가 전송 전후 차이로 자기 몫을 잰다.
    //  한도가 없는 구현(가짜·페이퍼)은 0. [why D-071]
    [[nodiscard]] virtual std::uint64_t rate_limit_wait_ns_this_thread() const noexcept
    {
        return 0;
    }
};
