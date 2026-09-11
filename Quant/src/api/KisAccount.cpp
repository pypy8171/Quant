// api/KisAccount.cpp — 잔고·미체결 조회(연속조회 tr_cont 포함). 원장 재시드·대사가 읽는다.
//  [why D-048] 파일 분할 경위.
#include "KisClientInternal.h"

// ─── 잔고 조회 (체결 확인용) — inquire-balance ────────────────────────────
//  output1 = 보유종목 배열(pdno·hldg_qty·pchs_avg_pric), output2 = 계좌 요약.
//  체결 후 보유수량 변화로 체결을 확인한다. (모의: VTTC8434R / 실거래: TTTC8434R)
nlohmann::json KisClient::get_balance()
{
    std::string tr_id = cfg_.is_paper ? "VTTC8434R" : "TTTC8434R";

    // 연속조회(페이지네이션): 잔고는 페이지당 ~20종목만 반환하고, 더 있으면 응답 body의
    //  ctx_area_nk100(다음페이지 키)가 채워진다. 이를 CTX_AREA_FK100/NK100로 되넣고
    //  요청헤더 tr_cont:N으로 다음 페이지를 받아 output1을 전부 누적한다. 트림 후 raw 연결로
    //  충분(모의계좌 실측: 20+11=31종목 정상 수신). output2/최상위는 첫 페이지 것을 유지.
    auto rtrim = [](std::string s)
    {
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
        {
            s.pop_back();
        }

        return s;
    };

    nlohmann::json result;
    nlohmann::json out1 = nlohmann::json::array();
    std::string fk, nk, cont;

    for (int page = 0; page < 30; ++page) // 안전 상한(무한루프 방지)
    {
        std::string url = base_url() + "/uapi/domestic-stock/v1/trading/inquire-balance" +
                          "?CANO=" + cfg_.account_no + "&ACNT_PRDT_CD=" + cfg_.account_type +
                          "&AFHR_FLPR_YN=N&OFL_YN=&INQR_DVSN=02&UNPR_DVSN=01" +
                          "&FUND_STTL_ICLD_YN=N&FNCG_AMT_AUTO_RDPT_YN=N&PRCS_DVSN=00" +
                          "&CTX_AREA_FK100=" + fk + "&CTX_AREA_NK100=" + nk;

        std::vector<std::string> headers = auth_headers(tr_id, {"tr_cont: " + cont});

        std::string resp = http_get(url, headers);

        if (resp.empty())
        {
            break;
        }

        nlohmann::json j;

        try
        {
            j = json::parse(resp);
        }
        catch (...)
        {
            break;
        }

        // [wire] 한도 초과(EGW00201)나 서버 오류 본문은 rt_cd≠"0"에 output1이 빈 배열이다. 이걸
        //  정상 응답처럼 돌려주면 호출자가 "보유 0종목"으로 읽어 원장을 비운 채 매매한다
        //  (09-11 09:17 재기동 시드 0건 → 3분간 빈 원장). 첫 페이지 실패는 빈 객체로 돌려
        //  호출자가 재시도하게 하고, 뒤 페이지 실패는 부분 목록으로 진행하지 않고 끊는다.
        if (j.value("rt_cd", "") != "0")
        {
            LOG_WARN("[KIS] 잔고 조회 응답 오류(page=" + std::to_string(page) + ") " +
                     j.value("msg_cd", "") + " " + j.value("msg1", ""));
            result = json();
            break;
        }

        if (page == 0)
        {
            result = j; // output2(계좌요약)·최상위 필드는 첫 페이지 기준
        }

        if (j.contains("output1") && j["output1"].is_array())
        {
            for (auto& h : j["output1"])
            {
                out1.push_back(h);
            }
        }

        std::string nk_next = rtrim(j.value("ctx_area_nk100", ""));

        if (nk_next.empty())
        {
            break; // 다음 페이지 없음
        }

        fk = rtrim(j.value("ctx_area_fk100", ""));
        nk = nk_next;
        cont = "N";
    }

    if (result.is_null())
    {
        return json::object();
    }

    result["output1"] = out1;
    return result;
}

// ─── 미체결(정정취소 가능) 예약주문 조회 — inquire-psbl-rvsecncl ─────────────
//  장중 청산이 40240000(주문가능분 없음)으로 막힐 때, 그 종목의 예약매도를 찾아
//  취소→재매도로 자가정리하기 위한 조회. (모의: VTTC0084R / 실거래: TTTC0084R)
//  응답 output(array) 필드는 소문자: odno·ord_gno_brno·pdno·prdt_name·psbl_qty·
//  ord_unpr·sll_buy_dvsn_cd(01매도/02매수). 수량·단가는 문자열이라 파싱 가드.
//  잔고처럼 ctx_area(FK/NK)로 페이지네이션한다.
std::vector<OpenOrder> KisClient::get_open_orders()
{
    std::string tr_id = cfg_.is_paper ? "VTTC0084R" : "TTTC0084R";

    auto to_int = [](const std::string& s) -> int
    { try { return s.empty() ? 0 : std::stoi(s); } catch (...) { return 0; } };
    auto to_dbl = [](const std::string& s) -> double
    { try { return s.empty() ? 0.0 : std::stod(s); } catch (...) { return 0.0; } };
    auto rtrim = [](std::string s)
    {
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
        {
            s.pop_back();
        }

        return s;
    };

    std::vector<OpenOrder> result;
    std::string fk, nk, cont;

    for (int page = 0; page < 30; ++page) // 안전 상한(무한루프 방지)
    {
        std::string url = base_url() +
                          "/uapi/domestic-stock/v1/trading/inquire-psbl-rvsecncl" +
                          "?CANO=" + cfg_.account_no + "&ACNT_PRDT_CD=" + cfg_.account_type +
                          "&INQR_DVSN_1=0&INQR_DVSN_2=0" +
                          "&CTX_AREA_FK100=" + fk + "&CTX_AREA_NK100=" + nk;

        std::vector<std::string> headers = auth_headers(tr_id, {"tr_cont: " + cont});

        std::string resp = http_get(url, headers);

        if (resp.empty())
        {
            break;
        }

        nlohmann::json j;

        try
        {
            j = json::parse(resp);
        }
        catch (...)
        {
            break;
        }

        if (j.value("rt_cd", std::string("")) != "0")
        {
            LOG_WARN("[KIS] 미체결 조회 오류: " + j.value("msg1", std::string("")));
            break;
        }

        if (j.contains("output") && j["output"].is_array())
        {
            for (auto& o : j["output"])
            {
                OpenOrder oo;
                oo.ticker    = o.value("pdno", std::string(""));
                oo.name      = o.value("prdt_name", std::string(""));
                oo.odno      = o.value("odno", o.value("ODNO", std::string("")));
                oo.krx_orgno = o.value("ord_gno_brno", std::string(""));
                oo.psbl_qty  = to_int(o.value("psbl_qty", std::string("")));
                oo.ord_unpr  = to_dbl(o.value("ord_unpr", std::string("")));
                std::string sb = o.value("sll_buy_dvsn_cd", std::string(""));
                oo.side = (sb == "01") ? OrderSide::SELL
                        : (sb == "02") ? OrderSide::BUY
                                       : OrderSide::NONE;

                if (!oo.ticker.empty() && oo.psbl_qty > 0)
                {
                    result.push_back(oo);
                }
            }
        }

        std::string nk_next = rtrim(j.value("ctx_area_nk100", ""));

        if (nk_next.empty())
        {
            break; // 다음 페이지 없음
        }

        fk = rtrim(j.value("ctx_area_fk100", ""));
        nk = nk_next;
        cont = "N";
    }

    return result;
}
