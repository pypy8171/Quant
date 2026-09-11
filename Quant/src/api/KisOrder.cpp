// api/KisOrder.cpp — 국내·해외 주문 발주·정정·취소와 응답 파서. IOrderExecutor 구현부.
//  실패는 예외가 아니라 OrderAck.err_code 값으로 돌려준다(D-039). [why D-048] 파일 분할 경위.
#include "KisClientInternal.h"

// 주문 응답 파서. 게이트웨이가 HTML 오류 페이지를 주거나 rt_cd가 없으면 예외 대신 false.
//  호출부(OrderRouter)가 catch로 막고는 있지만 예외 경로에서는 msg_cd가 비어 EGW00201 적응
//  재시도가 동작하지 않는다 — 실패를 값으로 돌려줘야 그 경로가 산다.
// rt_cd≠0 응답의 msg_cd. 비어 있으면 kUnknown — OrderAck 불변식(실패면 err_code 비지 않음)을 지킨다.
static std::string kis_reject_code(const json& j)
{
    std::string code = j.value("msg_cd", std::string(""));
    return code.empty() ? std::string(kis_err::kUnknown) : code;
}

static bool kis_parse_order_resp(const std::string& resp, json& j, const char* what)
{
    try
    {
        j = json::parse(resp);
    }
    catch (const std::exception& e)
    {
        LOG_ERROR(std::string("[KIS] ") + what + " 응답 파싱 실패: " + e.what() + " — " + resp.substr(0, 200));
        return false;
    }

    if (!j.is_object() || !j.contains("rt_cd") || !j["rt_cd"].is_string())
    {
        LOG_ERROR(std::string("[KIS] ") + what + " 응답에 rt_cd 없음 — " + resp.substr(0, 200));
        return false;
    }

    return true;
}

bool KisClient::send_order(const OrderSignal& signal)
{
    bool is_us = (signal.market == Market::US);
    std::string tr_id;
    std::string url;

    if (is_us)
    {
        // 해외주식 주문: NAS/NYS
        if (signal.side == OrderSide::BUY)
        {
            tr_id = cfg_.is_paper ? "VTTT1002U" : "TTTT1002U";
        }
        else
        {
            tr_id = cfg_.is_paper ? "VTTT1006U" : "TTTT1006U";
        }

        url = base_url() + "/uapi/overseas-stock/v1/trading/order";
    }
    else
    {
        // 국내주식 현금 주문
        if (signal.side == OrderSide::BUY)
        {
            tr_id = cfg_.is_paper ? "VTTC0802U" : "TTTC0802U";
        }
        else
        {
            tr_id = cfg_.is_paper ? "VTTC0801U" : "TTTC0801U";
        }

        url = base_url() + "/uapi/domestic-stock/v1/trading/order-cash";
    }

    json body;

    if (is_us)
    {
        body = {{"CANO", cfg_.account_no},
                {"ACNT_PRDT_CD", cfg_.account_type},
                {"OVRS_EXCG_CD", signal.exchange},
                {"PDNO", signal.ticker},
                {"ORD_DVSN", "00"}, // 해외주식은 지정가(00)만 낸다. 시장가도 가격 "0"의 00으로 나간다.
                {"ORD_QTY", std::to_string(signal.quantity)},
                {"OVRS_ORD_UNPR", signal.type == OrderType::LIMIT ? std::to_string(signal.price) : "0"}};
    }
    else
    {
        body = {{"CANO", cfg_.account_no},
                {"ACNT_PRDT_CD", cfg_.account_type},
                {"PDNO", signal.ticker},
                {"ORD_DVSN", signal.type == OrderType::MARKET ? "01" : "00"},
                {"ORD_QTY", std::to_string(signal.quantity)},
                {"ORD_UNPR", signal.type == OrderType::LIMIT ? std::to_string((int)signal.price) : "0"}};
    }

    std::string resp = http_post(url,
                                 auth_headers(tr_id, {"Content-Type: application/json"}),
                                 body.dump());

    if (resp.empty())
    {
        LOG_ERROR("[KIS] 주문 실패: " + signal.ticker);
        return false;
    }

    json j;

    if (!kis_parse_order_resp(resp, j, "send_order"))
    {
        return false;
    }

    bool ok = (j["rt_cd"].get<std::string>() == "0");

    if (ok)
    {
        LOG_INFO("[KIS] 주문 성공: " + signal.ticker + (signal.side == OrderSide::BUY ? " BUY " : " SELL ") +
                 std::to_string(signal.quantity) + "주");
    }
    else
    {
        LOG_ERROR("[KIS] 주문 오류: " + j.value("msg1", std::string("")));
    }

    return ok;
}

// ─── MM-1: 신규 주문 + KRX 조직번호 캡처 ──────────────────────────────────
//  send_order와 본문·tr_id 동일. 응답에서 ODNO에 더해 KRX_FWDG_ORD_ORGNO를
//  추출해 반환한다(정정/취소 시 원주문 조직번호로 재입력해야 함).
//  HTTP 플랫폼 분기(WinHTTP/libcurl)는 http_post 내부에 이미 캡슐화됨.
OrderAck KisClient::submit_order_ack(const OrderSignal& signal)
{
    bool is_us = (signal.market == Market::US);
    std::string tr_id, url;

    if (is_us)
    {
        tr_id = (signal.side == OrderSide::BUY) ? (cfg_.is_paper ? "VTTT1002U" : "TTTT1002U")
                                                 : (cfg_.is_paper ? "VTTT1006U" : "TTTT1006U");
        url = base_url() + "/uapi/overseas-stock/v1/trading/order";
    }
    else
    {
        tr_id = (signal.side == OrderSide::BUY) ? (cfg_.is_paper ? "VTTC0802U" : "TTTC0802U")
                                                 : (cfg_.is_paper ? "VTTC0801U" : "TTTC0801U");
        url = base_url() + "/uapi/domestic-stock/v1/trading/order-cash";
    }

    json body;

    if (is_us)
    {
        body = {{"CANO", cfg_.account_no}, {"ACNT_PRDT_CD", cfg_.account_type},
                {"OVRS_EXCG_CD", signal.exchange}, {"PDNO", signal.ticker},
                {"ORD_DVSN", "00"}, {"ORD_QTY", std::to_string(signal.quantity)},
                {"OVRS_ORD_UNPR", signal.type == OrderType::LIMIT ? std::to_string(signal.price) : "0"}};
    }
    else
    {
        body = {{"CANO", cfg_.account_no}, {"ACNT_PRDT_CD", cfg_.account_type},
                {"PDNO", signal.ticker},
                {"ORD_DVSN", signal.type == OrderType::MARKET ? "01" : "00"},
                {"ORD_QTY", std::to_string(signal.quantity)},
                {"ORD_UNPR", signal.type == OrderType::LIMIT ? std::to_string((int)signal.price) : "0"}};
    }

    std::string resp = http_post(url,
        auth_headers(tr_id, {"Content-Type: application/json"}),
        body.dump());

    if (resp.empty())
    {
        LOG_ERROR("[KIS] submit_order_ack 실패: " + signal.ticker);
        return OrderAck::fail(kis_err::kTransport);
    }

    json j;

    if (!kis_parse_order_resp(resp, j, "submit_order_ack"))
    {
        return OrderAck::fail(kis_err::kTransport);
    }

    if (j["rt_cd"].get<std::string>() != "0")
    {
        LOG_ERROR("[KIS] 주문 오류: " + j.value("msg1", std::string("")));
        return OrderAck::fail(kis_reject_code(j));
    }

    auto out = j.value("output", json::object());
    OrderAck ack;
    ack.odno      = out.value("ODNO", "");
    ack.krx_orgno = out.value("KRX_FWDG_ORD_ORGNO", "");
    LOG_INFO("[KIS] 주문 접수: " + signal.ticker +
             (signal.side == OrderSide::BUY ? " BUY " : " SELL ") +
             std::to_string(signal.quantity) + "주  ODNO=" + ack.odno +
             " ORGNO=" + ack.krx_orgno);
    return ack;
}

// ─── MM-1: 정정/취소 (국내 order-rvsecncl) ─────────────────────────────────
//  RVSE_CNCL_DVSN_CD: "02"=취소, "01"=정정. 성공 시 응답 ODNO(취소/정정 접수번호) 반환.
//  KRX_FWDG_ORD_ORGNO(원주문 조직번호)와 ORGN_ODNO(원주문번호)가 필수 입력.
//  주의: 국내 현금 주문 전용. 해외(overseas) 정정/취소는 별도 tr_id/URL — 미구현(TODO).
OrderAck KisClient::cancel_order(const std::string& ticker, const std::string& orig_odno,
                                 const std::string& krx_orgno, int qty, bool all_remaining)
{

    if (orig_odno.empty())
    {
        LOG_ERROR("[KIS] cancel_order 원주문번호(ODNO) 없음 — " + ticker);
        return OrderAck::fail("E_NO_ORIG_ODNO");
    }

    std::string tr_id = cfg_.is_paper ? "VTTC0803U" : "TTTC0803U";
    std::string url   = base_url() + "/uapi/domestic-stock/v1/trading/order-rvsecncl";

    json body = {{"CANO", cfg_.account_no},
                 {"ACNT_PRDT_CD", cfg_.account_type},
                 {"KRX_FWDG_ORD_ORGNO", krx_orgno},              // 원주문 조직번호
                 {"ORGN_ODNO", orig_odno},                       // 원주문번호
                 {"ORD_DVSN", "00"},                             // 지정가 (취소도 원주문 구분 통상 "00")
                 {"RVSE_CNCL_DVSN_CD", "02"},                    // 02=취소
                 {"ORD_QTY", std::to_string(qty)},               // 취소 수량 (QTY_ALL_ORD_YN=Y면 무시됨)
                 {"ORD_UNPR", "0"},                              // 취소는 단가 0
                 {"QTY_ALL_ORD_YN", all_remaining ? "Y" : "N"}}; // 잔량 전체 취소

    std::string resp = http_post(url,
        auth_headers(tr_id, {"Content-Type: application/json"}),
        body.dump());

    if (resp.empty())
    {
        LOG_ERROR("[KIS] cancel_order 전송 실패: " + ticker + " ODNO=" + orig_odno);
        return OrderAck::fail(kis_err::kTransport);
    }

    json j;

    if (!kis_parse_order_resp(resp, j, "cancel_order"))
    {
        return OrderAck::fail(kis_err::kTransport);
    }

    if (j["rt_cd"].get<std::string>() != "0")
    {
        // 이미 체결/취소된 주문이면 KIS가 거부 → 자가치유(호출부가 reserved 미변경). 로그만.
        LOG_WARN("[KIS] 취소 거부: " + ticker + " ODNO=" + orig_odno + " — " +
                 j.value("msg1", std::string("")));
        return OrderAck::fail(kis_reject_code(j));
    }

    std::string cancel_odno = j.value("output", json::object()).value("ODNO", "");
    LOG_INFO("[KIS] 취소 접수: " + ticker + " 원ODNO=" + orig_odno +
             " 취소ODNO=" + cancel_odno);
    return OrderAck{cancel_odno, std::string(), std::string()};
}

OrderAck KisClient::revise_order(const std::string& ticker, const std::string& orig_odno,
                                 const std::string& krx_orgno, int new_qty, double new_price)
{

    if (orig_odno.empty())
    {
        LOG_ERROR("[KIS] revise_order 원주문번호(ODNO) 없음 — " + ticker);
        return OrderAck::fail("E_NO_ORIG_ODNO");
    }

    std::string tr_id = cfg_.is_paper ? "VTTC0803U" : "TTTC0803U";
    std::string url   = base_url() + "/uapi/domestic-stock/v1/trading/order-rvsecncl";

    json body = {{"CANO", cfg_.account_no},
                 {"ACNT_PRDT_CD", cfg_.account_type},
                 {"KRX_FWDG_ORD_ORGNO", krx_orgno},
                 {"ORGN_ODNO", orig_odno},
                 {"ORD_DVSN", "00"},                              // 지정가
                 {"RVSE_CNCL_DVSN_CD", "01"},                     // 01=정정
                 {"ORD_QTY", std::to_string(new_qty)},            // 정정 수량
                 {"ORD_UNPR", std::to_string((int)new_price)},    // 정정 단가
                 // QTY_ALL_ORD_YN="Y"는 KIS가 잔량 전체를 정정하게 하므로, 위 ORD_QTY(부분 정정
                 // 수량)는 실제로 반영되지 않는다. 현재 호출부는 단가 정정만 쓰므로 무해하나,
                 // 부분수량 정정이 필요해지면 "N"으로 바꾸고 ORD_QTY를 살려야 한다(보류 목록).
                 {"QTY_ALL_ORD_YN", "Y"}};                        // 잔량 전체 정정

    std::string resp = http_post(url,
        auth_headers(tr_id, {"Content-Type: application/json"}),
        body.dump());

    if (resp.empty())
    {
        LOG_ERROR("[KIS] revise_order 전송 실패: " + ticker + " ODNO=" + orig_odno);
        return OrderAck::fail(kis_err::kTransport);
    }

    json j;

    if (!kis_parse_order_resp(resp, j, "revise_order"))
    {
        return OrderAck::fail(kis_err::kTransport);
    }

    if (j["rt_cd"].get<std::string>() != "0")
    {
        LOG_WARN("[KIS] 정정 거부: " + ticker + " ODNO=" + orig_odno + " — " +
                 j.value("msg1", std::string("")));
        return OrderAck::fail(kis_reject_code(j));
    }

    // 정정 성공 시 새 ODNO 발급 → 반환 (호출부가 kis_order_no 갱신)
    std::string new_odno = j.value("output", json::object()).value("ODNO", "");
    LOG_INFO("[KIS] 정정 접수: " + ticker + " 원ODNO=" + orig_odno +
             " 새ODNO=" + new_odno + " @" + std::to_string((int)new_price));
    return OrderAck{new_odno, std::string(), std::string()};
}

// ═══════════════════════════════════════════════════════════════════════════
//  해외 주식 주문
//  매수: TTTT1002U(실거래) / VTTT1002U(모의)
//  매도: TTTT1006U(실거래) / VTTT1006U(모의)
// ═══════════════════════════════════════════════════════════════════════════
bool KisClient::send_us_order(const OrderSignal& signal)
{
    std::string tr_id;

    if (signal.side == OrderSide::BUY)
    {
        tr_id = cfg_.is_paper ? "VTTT1002U" : "TTTT1002U";
    }
    else
    {
        tr_id = cfg_.is_paper ? "VTTT1006U" : "TTTT1006U";
    }

    // KIS 해외주식 주문: 시장가 = ORD_DVSN "00", 가격 "0"
    json body = {{"CANO", cfg_.account_no},
                 {"ACNT_PRDT_CD", cfg_.account_type},
                 {"OVRS_EXCG_CD", signal.exchange.empty() ? "NASD" : signal.exchange},
                 {"PDNO", signal.ticker},
                 {"ORD_DVSN", "00"}, // 해외주식은 지정가(00)만 낸다. 시장가도 가격 "0"의 00으로 나간다.
                 {"ORD_QTY", std::to_string(signal.quantity)},
                 {"OVRS_ORD_UNPR", signal.type == OrderType::LIMIT ? std::to_string(signal.price) : "0"}};

    std::string url = base_url() + "/uapi/overseas-stock/v1/trading/order";
    std::string resp = http_post(url,
                                 auth_headers(tr_id, {"Content-Type: application/json"}),
                                 body.dump());

    if (resp.empty())
    {
        LOG_ERROR("[KIS-US] 주문 실패: " + signal.ticker);
        return false;
    }

    json j;

    if (!kis_parse_order_resp(resp, j, "send_us_order"))
    {
        return false;
    }

    bool ok = (j["rt_cd"].get<std::string>() == "0");

    if (ok)
    {
        LOG_INFO("[KIS-US] 주문 성공: " + signal.ticker + (signal.side == OrderSide::BUY ? " BUY " : " SELL ") +
                 std::to_string(signal.quantity) + "주");
    }
    else
    {
        LOG_ERROR("[KIS-US] 주문 오류: " + j.value("msg1", "unknown"));
    }

    return ok;
}
