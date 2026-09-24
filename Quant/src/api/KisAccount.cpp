// api/KisAccount.cpp — 잔고·미체결 조회(연속조회 tr_cont 포함). 원장 재시드·대사가 읽는다.
//  [why D-048] 파일 분할 경위.
#include "KisClientInternal.h"
#include "api/KisRestDecode.h"

#include <ctime>

// ─── 잔고 조회 (체결 확인용) — inquire-balance ────────────────────────────
//  output1 = 보유종목 배열(pdno·hldg_qty·pchs_avg_pric), output2 = 계좌 요약. 필드 해석은
//  kis_rest::decode_balance_page가 소유한다. (모의: VTTC8434R / 실거래: TTTC8434R)
KisResult<AccountBalance> KisClient::get_balance()
{
    std::string transaction_id = config_.is_paper ? "VTTC8434R" : "TTTC8434R";

    // 연속조회(페이지네이션): 잔고는 페이지당 ~20종목만 반환하고, 더 있으면 응답 body의
    //  ctx_area_nk100(다음페이지 키)가 채워진다. 이를 CTX_AREA_FK100/NK100로 되넣고
    //  요청헤더 tr_cont:N으로 다음 페이지를 받아 output1을 전부 누적한다. 트림 후 raw 연결로
    //  충분(모의계좌 실측: 20+11=31종목 정상 수신). output2/최상위는 첫 페이지 것을 유지.
    auto rtrim = [](std::string text)
    {
        while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        {
            text.pop_back();
        }

        return text;
    };

    AccountBalance balance;
    std::string forward_key, next_key, continuation;
    // 초당 한도(EGW00201)는 HTTP 200 본문으로 와도 http_get이 버킷을 기다려 한 번 되보낸다(CODE_REVIEW W-3).
    //  기동 직후 인증·시세·잔고가 같은 초에 몰려 걸리던 일(09-21 08:31)은 그 되보냄이 받는다.

    for (int page = 0; page < 30; ++page) // 안전 상한(무한루프 방지)
    {
        std::string url = base_url() + "/uapi/domestic-stock/v1/trading/inquire-balance" +
                          "?CANO=" + config_.account_no + "&ACNT_PRDT_CD=" + config_.account_type +
                          "&AFHR_FLPR_YN=N&OFL_YN=&INQR_DVSN=02&UNPR_DVSN=01" +
                          "&FUND_STTL_ICLD_YN=N&FNCG_AMT_AUTO_RDPT_YN=N&PRCS_DVSN=00" +
                          "&CTX_AREA_FK100=" + forward_key + "&CTX_AREA_NK100=" + next_key;

        std::string response = http_get(url, authentication_headers(transaction_id, {"tr_cont: " + continuation}));

        // [inv] 어느 페이지든 못 받으면 전체가 실패다. 2페이지째가 빠진 부분 목록을 성공으로 돌려주면 호출자의
        //  유령 정리(prune_positions)가 그 페이지의 실보유를 걷어낸다 — 빈 목록 가드로는 못 잡는 구멍.
        if (response.empty())
        {
            return kis_fail("transport", "잔고 응답 없음(page=" + std::to_string(page) + ")");
        }

        nlohmann::json document = json::parse(response, nullptr, false);

        if (document.is_discarded())
        {
            return kis_fail("parse", "잔고 JSON 파싱 불가(page=" + std::to_string(page) + ")");
        }

        // [wire] 한도 초과(EGW00201)나 서버 오류 본문은 rt_cd≠"0"에 output1이 빈 배열이다. 이걸
        //  정상 응답처럼 돌려주면 호출자가 "보유 0종목"으로 읽어 원장을 비운 채 매매한다
        //  (09-11 09:17 재기동 시드 0건 → 3분간 빈 원장).
        if (document.value("rt_cd", "") != "0")
        {
            const std::string message_code = document.value("msg_cd", "");

            LOG_WARN("[KIS] 잔고 조회 응답 오류(page=" + std::to_string(page) + ") " +
                     message_code + " " + document.value("msg1", ""));
            return kis_fail(document.value("msg_cd", "rt_cd"), document.value("msg1", ""));
        }

        kis_rest::decode_balance_page(document, balance, page == 0);

        std::string next_key_next = rtrim(document.value("ctx_area_nk100", ""));

        if (next_key_next.empty())
        {
            break; // 다음 페이지 없음
        }

        forward_key = rtrim(document.value("ctx_area_fk100", ""));
        next_key = std::move(next_key_next);
        continuation = "N";
    }

    return balance;
}

// ─── 미체결(정정취소 가능) 예약주문 조회 — inquire-psbl-rvsecncl ─────────────
//  장중 청산이 40240000(주문가능분 없음)으로 막힐 때, 그 종목의 예약매도를 찾아
//  취소→재매도로 자가정리하기 위한 조회. (실거래: TTTC0084R / 모의는 VTTC0081R(inquire-daily-ccld))
//  주문번호는 odno(ODNO), 행 배열은 모의 output1·실전 output. 필드는 소문자: ord_gno_brno·pdno·prdt_name·psbl_qty·
//  ord_unpr·sll_buy_dvsn_cd(01매도/02매수). 수량·단가는 문자열이라 파싱 가드.
//  잔고처럼 ctx_area(FK/NK)로 페이지네이션한다.
// 오늘(로컬 시각 기준 — 운영 PC가 KST라 같다) 날짜 YYYYMMDD. 모의계좌 미체결 조회가 조회구간을 요구해서 쓴다.
static std::string today_yyyymmdd()
{
    const std::time_t now = std::time(nullptr);
    std::tm broken{};
#ifdef _WIN32
    localtime_s(&broken, &now);
#else
    localtime_r(&now, &broken);
#endif
    char buffer[16] = {0};
    std::strftime(buffer, sizeof(buffer), "%Y%m%d", &broken);

    return std::string(buffer);
}

// ─── 미체결 조회 ──────────────────────────────────────────────────────────
//  실거래는 정정취소가능주문조회(inquire-psbl-rvsecncl, TTTC0084R)를 쓴다 — 취소 가능 수량을 바로 준다.
//  모의계좌는 그 업무를 지원하지 않아("모의투자에서는 해당업무를 지원하지 않습니다") 늘 빈 목록이 돌아왔다.
//  그래서 브로커에는 살아 있는데 엔진이 모르는 주문이 생겨도 대사가 못 잡았다 — 2026-09-23 09:26 021240에서
//  전송 실패로 접수된 매도 18주(ODNO=0000007886)를 못 찾아 손절 불능 상태가 됐다. 모의는 일별주문체결조회
//  (inquire-daily-ccld, VTTC0081R)로 당일분을 받아 잔여수량>0·미취소만 남긴다. [why D-101]
std::vector<OpenOrder> KisClient::get_open_orders()
{
    const bool paper = config_.is_paper;
    std::string transaction_id = paper ? "VTTC0081R" : "TTTC0084R";

    auto to_int = [](const std::string& text) -> int
    { try { return text.empty() ? 0 : std::stoi(text); } catch (...) { return 0; } };
    auto to_dbl = [](const std::string& text) -> double
    { try { return text.empty() ? 0.0 : std::stod(text); } catch (...) { return 0.0; } };
    auto rtrim = [](std::string text)
    {
        while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        {
            text.pop_back();
        }

        return text;
    };

    std::vector<OpenOrder> result;
    std::string forward_key, next_key, continuation;

    for (int page = 0; page < 30; ++page) // 안전 상한(무한루프 방지)
    {
        std::string url = base_url();

        if (paper)
        {
            const std::string today = today_yyyymmdd();
            url += "/uapi/domestic-stock/v1/trading/inquire-daily-ccld"
                   "?CANO=" + config_.account_no + "&ACNT_PRDT_CD=" + config_.account_type +
                   "&INQR_STRT_DT=" + today + "&INQR_END_DT=" + today +
                   "&SLL_BUY_DVSN_CD=00&INQR_DVSN=00&PDNO=&CCLD_DVSN=00&ORD_GNO_BRNO=&ODNO="
                   "&INQR_DVSN_3=00&INQR_DVSN_1=" +
                   "&CTX_AREA_FK100=" + forward_key + "&CTX_AREA_NK100=" + next_key;
        }
        else
        {
            url += "/uapi/domestic-stock/v1/trading/inquire-psbl-rvsecncl"
                   "?CANO=" + config_.account_no + "&ACNT_PRDT_CD=" + config_.account_type +
                   "&INQR_DVSN_1=0&INQR_DVSN_2=0" +
                   "&CTX_AREA_FK100=" + forward_key + "&CTX_AREA_NK100=" + next_key;
        }

        std::string response = http_get(url, authentication_headers(transaction_id, {"tr_cont: " + continuation}));

        if (response.empty())
        {
            break;
        }

        nlohmann::json document;

        try
        {
            document = json::parse(response);
        }
        catch (...)
        {
            break;
        }

        if (document.value("rt_cd", std::string("")) != "0")
        {
            LOG_WARN("[KIS] 미체결 조회 오류: " + document.value("msg1", std::string("")));
            break;
        }

        // 응답 배열 이름과 수량 필드가 두 엔드포인트에서 다르다. 모의는 output1/rmn_qty(잔여), 실거래는
        //  output/psbl_qty(취소가능). [inv] rows는 document가 사는 동안만 유효하다.
        const char* const rows_key = paper ? "output1" : "output";
        const auto rows = document.find(rows_key);

        if (rows != document.end() && rows->is_array())
        {
            for (auto& output_node : *rows)
            {
                if (paper && output_node.value("cncl_yn", std::string("")) == "Y")
                {
                    continue; // 이미 취소된 주문
                }

                OpenOrder open_order;
                open_order.ticker    = output_node.value("pdno", std::string(""));
                open_order.name      = output_node.value("prdt_name", std::string(""));
                open_order.kis_order_no      = output_node.value("odno", output_node.value("ODNO", std::string("")));
                open_order.krx_forwarding_org_no = output_node.value("ord_gno_brno", std::string(""));
                open_order.psbl_qty  = to_int(output_node.value(paper ? "rmn_qty" : "psbl_qty", std::string("")));
                open_order.ord_unpr  = to_dbl(output_node.value("ord_unpr", std::string("")));
                std::string buy_sell_code = output_node.value("sll_buy_dvsn_cd", std::string(""));
                open_order.side = (buy_sell_code == "01") ? OrderSide::SELL
                        : (buy_sell_code == "02") ? OrderSide::BUY
                                       : OrderSide::NONE;

                if (!open_order.ticker.empty() && open_order.psbl_qty > 0)
                {
                    result.push_back(std::move(open_order));
                }
            }
        }

        std::string next_key_next = rtrim(document.value("ctx_area_nk100", ""));

        if (next_key_next.empty())
        {
            break; // 다음 페이지 없음
        }

        forward_key = rtrim(document.value("ctx_area_fk100", ""));
        next_key = std::move(next_key_next);
        continuation = "N";
    }

    return result;
}
