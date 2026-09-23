// api/KisOrder.cpp — 국내·해외 주문 발주·정정·취소와 응답 파서. IOrderExecutor 구현부.
//  실패는 예외가 아니라 OrderAck.error_code 값으로 돌려준다(D-039). [why D-048] 파일 분할 경위.
#include "KisClientInternal.h"
#include "utils/JsonNode.h"
#include "core/KstTime.h"

// 주문 응답 파서. 게이트웨이가 HTML 오류 페이지를 주거나 rt_cd가 없으면 예외 대신 false.
//  호출부(OrderRouter)가 catch로 막고는 있지만 예외 경로에서는 msg_cd가 비어 EGW00201 적응
//  재시도가 동작하지 않는다 — 실패를 값으로 돌려줘야 그 경로가 산다.
// rt_cd≠0 응답의 msg_cd. 비어 있으면 kUnknown — OrderAck 불변식(실패면 error_code 비지 않음)을 지킨다.
static std::string kis_reject_code(const json& document)
{
    std::string code = document.value("msg_cd", std::string(""));

    if (code.empty())
    {
        return kis_error::kUnknown;
    }

    return code;
}

// 국내 주문구분. 시장가(01)를 받는 곳은 KRX 정규장(09:00~15:30)뿐이다 — KRX 애프터마켓(16:00~20:00)도,
//  NXT 프리(08:00~08:50)·애프터(15:40~20:00)도 지정가·최우선·최유리만 받는다. 그래서 정규장 밖에서 나온
//  시장가 신호는 최유리지정가(03, 가격 0)로 바꿔 보낸다 — 반대편 최우선 호가에 붙는 가장 가까운 대체다.
//  창 바깥(예: 08:50~09:00)은 어차피 OrderGate 세션 창이 막으므로 여기서 다시 보지 않는다.
//  [why D-097] [why D-122]
static constexpr int32_t kRegularSessionOpenHhmmss  = 90000;   // KRX 정규장 시작 09:00:00
static constexpr int32_t kRegularSessionCloseHhmmss = 153000;  // KRX 정규장 끝 15:30:00

static const char* kis_order_division(OrderType type)
{
    if (type != OrderType::MARKET)
    {
        return "00";
    }

    const int32_t hhmmss         = kst::hhmmss_int(std::time(nullptr));
    const bool    regular_session = hhmmss >= kRegularSessionOpenHhmmss && hhmmss < kRegularSessionCloseHhmmss;
    return regular_session ? "01" : "03";
}

static bool kis_parse_order_response(const std::string& response, json& document, const char* what)
{
    try
    {
        document = json::parse(response);
    }
    catch (const std::exception& exception)
    {
        LOG_ERROR(std::string("[KIS] ") + what + " 응답 파싱 실패: " + exception.what() + " — " + response.substr(0, 200));
        return false;
    }

    if (!document.is_object() || !document.contains("rt_cd") || !document["rt_cd"].is_string())
    {
        LOG_ERROR(std::string("[KIS] ") + what + " 응답에 rt_cd 없음 — " + response.substr(0, 200));
        return false;
    }

    return true;
}

bool KisClient::send_order(const OrderSignal& signal)
{
    bool is_us = (signal.market == Market::US);
    std::string transaction_id;
    std::string url;

    if (is_us)
    {
        // 해외주식 주문: NAS/NYS
        if (signal.side == OrderSide::BUY)
        {
            transaction_id = config_.is_paper ? "VTTT1002U" : "TTTT1002U";
        }
        else
        {
            transaction_id = config_.is_paper ? "VTTT1006U" : "TTTT1006U";
        }

        url = base_url() + "/uapi/overseas-stock/v1/trading/order";
    }
    else
    {
        // 국내주식 현금 주문
        if (signal.side == OrderSide::BUY)
        {
            transaction_id = config_.is_paper ? "VTTC0012U" : "TTTC0012U";
        }
        else
        {
            transaction_id = config_.is_paper ? "VTTC0011U" : "TTTC0011U";
        }

        url = base_url() + "/uapi/domestic-stock/v1/trading/order-cash";
    }

    json body;

    if (is_us)
    {
        body = {{"CANO", config_.account_no},
                {"ACNT_PRDT_CD", config_.account_type},
                {"OVRS_EXCG_CD", signal.exchange},
                {"PDNO", signal.ticker},
                {"ORD_DVSN", "00"}, // 해외주식은 지정가(00)만 낸다. 시장가도 가격 "0"의 00으로 나간다.
                {"ORD_QTY", std::to_string(signal.quantity)},
                {"OVRS_ORD_UNPR", signal.type == OrderType::LIMIT ? std::to_string(signal.price) : "0"}};
    }
    else
    {
        body = {{"CANO", config_.account_no},
                {"ACNT_PRDT_CD", config_.account_type},
                {"PDNO", signal.ticker},
                {"ORD_DVSN", kis_order_division(signal.type)}, // 정규장 밖엔 시장가→최유리 [why D-097]
                {"ORD_QTY", std::to_string(signal.quantity)},
                {"ORD_UNPR", signal.type == OrderType::LIMIT ? std::to_string(static_cast<int>(signal.price)) : "0"},
                {"EXCG_ID_DVSN_CD", kis_order_exchange(config_)}}; // KRX/NXT/SOR [why D-096]
    }

    std::string response = http_post(url,
                                 authentication_headers(transaction_id, {"Content-Type: application/json"}),
                                 body.dump());

    if (response.empty())
    {
        LOG_ERROR("[KIS] 주문 실패: " + signal.ticker);
        return false;
    }

    json document;

    if (!kis_parse_order_response(response, document, "send_order"))
    {
        return false;
    }

    bool ok = (document["rt_cd"].get_ref<const std::string&>() == "0");

    if (ok)
    {
        LOG_INFO("[KIS] 주문 성공: " + signal.ticker + (signal.side == OrderSide::BUY ? " BUY " : " SELL ") +
                 std::to_string(signal.quantity) + "주");
    }
    else
    {
        LOG_ERROR("[KIS] 주문 오류: " + document.value("msg1", std::string("")));
    }

    return ok;
}

// ─── MM-1: 신규 주문 + KRX 조직번호 캡처 ──────────────────────────────────
//  send_order와 본문·tr_id 동일. 응답에서 ODNO에 더해 KRX_FWDG_ORD_ORGNO를
//  추출해 반환한다(정정/취소 시 원주문 조직번호로 재입력해야 함).
//  HTTP 플랫폼 분기(WinHTTP/libcurl)는 http_post 내부에 이미 캡슐화됨.
OrderAck KisClient::submit_order_acknowledgement(const OrderSignal& signal)
{
    bool is_us = (signal.market == Market::US);
    std::string transaction_id, url;

    if (is_us)
    {
        transaction_id = (signal.side == OrderSide::BUY) ? (config_.is_paper ? "VTTT1002U" : "TTTT1002U")
                                                 : (config_.is_paper ? "VTTT1006U" : "TTTT1006U");
        url = base_url() + "/uapi/overseas-stock/v1/trading/order";
    }
    else
    {
        transaction_id = (signal.side == OrderSide::BUY) ? (config_.is_paper ? "VTTC0012U" : "TTTC0012U")
                                                 : (config_.is_paper ? "VTTC0011U" : "TTTC0011U");
        url = base_url() + "/uapi/domestic-stock/v1/trading/order-cash";
    }

    json body;

    if (is_us)
    {
        body = {{"CANO", config_.account_no}, {"ACNT_PRDT_CD", config_.account_type},
                {"OVRS_EXCG_CD", signal.exchange}, {"PDNO", signal.ticker},
                {"ORD_DVSN", "00"}, {"ORD_QTY", std::to_string(signal.quantity)},
                {"OVRS_ORD_UNPR", signal.type == OrderType::LIMIT ? std::to_string(signal.price) : "0"}};
    }
    else
    {
        body = {{"CANO", config_.account_no}, {"ACNT_PRDT_CD", config_.account_type},
                {"PDNO", signal.ticker},
                {"ORD_DVSN", kis_order_division(signal.type)}, // 정규장 밖엔 시장가→최유리 [why D-097]
                {"ORD_QTY", std::to_string(signal.quantity)},
                {"ORD_UNPR", signal.type == OrderType::LIMIT ? std::to_string(static_cast<int>(signal.price)) : "0"},
                {"EXCG_ID_DVSN_CD", kis_order_exchange(config_)}}; // KRX/NXT/SOR [why D-096]
    }

    std::string response = http_post(url,
        authentication_headers(transaction_id, {"Content-Type: application/json"}),
        body.dump());

    if (response.empty())
    {
        LOG_ERROR("[KIS] submit_order_ack 실패: " + signal.ticker);
        return OrderAck::fail(kis_error::kTransport);
    }

    json document;

    if (!kis_parse_order_response(response, document, "submit_order_ack"))
    {
        return OrderAck::fail(kis_error::kTransport);
    }

    if (document["rt_cd"].get_ref<const std::string&>() != "0")
    {
        LOG_ERROR("[KIS] 주문 오류: " + document.value("msg1", std::string("")));
        return OrderAck::fail(kis_reject_code(document));
    }

    const json& out = jsonx::object_or_empty(document, "output");
    OrderAck    acknowledgement;
    acknowledgement.kis_order_no      = out.value("ODNO", "");
    acknowledgement.krx_forwarding_org_no = out.value("KRX_FWDG_ORD_ORGNO", "");
    LOG_INFO("[KIS] 주문 접수: " + signal.ticker +
             (signal.side == OrderSide::BUY ? " BUY " : " SELL ") +
             std::to_string(signal.quantity) + "주  ODNO=" + acknowledgement.kis_order_no +
             " ORGNO=" + acknowledgement.krx_forwarding_org_no);
    return acknowledgement;
}

// ─── MM-1: 정정/취소 (국내 order-rvsecncl) ─────────────────────────────────
//  RVSE_CNCL_DVSN_CD: "02"=취소, "01"=정정. 성공 시 응답 ODNO(취소/정정 접수번호) 반환.
//  KRX_FWDG_ORD_ORGNO(원주문 조직번호)와 ORGN_ODNO(원주문번호)가 필수 입력.
//  주의: 국내 현금 주문 전용. 해외(overseas) 정정/취소는 별도 tr_id/URL — 미구현(TODO).
OrderAck KisClient::cancel_order(const std::string& ticker, const std::string& orig_odno,
                                 const std::string& krx_forwarding_org_no, int quantity, bool all_remaining)
{

    if (orig_odno.empty())
    {
        LOG_ERROR("[KIS] cancel_order 원주문번호(ODNO) 없음 — " + ticker);
        return OrderAck::fail("E_NO_ORIG_ODNO");
    }

    std::string transaction_id = config_.is_paper ? "VTTC0013U" : "TTTC0013U";
    std::string url   = base_url() + "/uapi/domestic-stock/v1/trading/order-rvsecncl";

    json body = {{"CANO", config_.account_no},
                 {"ACNT_PRDT_CD", config_.account_type},
                 {"KRX_FWDG_ORD_ORGNO", krx_forwarding_org_no},              // 원주문 조직번호
                 {"ORGN_ODNO", orig_odno},                       // 원주문번호
                 {"ORD_DVSN", "00"},                             // 지정가 (취소도 원주문 구분 통상 "00")
                 {"RVSE_CNCL_DVSN_CD", "02"},                    // 02=취소
                 {"ORD_QTY", std::to_string(quantity)},               // 취소 수량 (QTY_ALL_ORD_YN=Y면 무시됨)
                 {"ORD_UNPR", "0"},                              // 취소는 단가 0
                 {"QTY_ALL_ORD_YN", all_remaining ? "Y" : "N"}, // 잔량 전체 취소
                 {"EXCG_ID_DVSN_CD", kis_order_exchange(config_)}}; // 원주문과 같은 거래소 구분 [why D-096]

    std::string response = http_post(url,
        authentication_headers(transaction_id, {"Content-Type: application/json"}),
        body.dump());

    if (response.empty())
    {
        LOG_ERROR("[KIS] cancel_order 전송 실패: " + ticker + " ODNO=" + orig_odno);
        return OrderAck::fail(kis_error::kTransport);
    }

    json document;

    if (!kis_parse_order_response(response, document, "cancel_order"))
    {
        return OrderAck::fail(kis_error::kTransport);
    }

    if (document["rt_cd"].get_ref<const std::string&>() != "0")
    {
        // 이미 체결/취소된 주문이면 KIS가 거부 → 자가치유(호출부가 reserved 미변경). 로그만.
        LOG_WARN("[KIS] 취소 거부: " + ticker + " ODNO=" + orig_odno + " — " +
                 document.value("msg1", std::string("")));
        return OrderAck::fail(kis_reject_code(document));
    }

    std::string cancel_order_no = jsonx::object_or_empty(document, "output").value("ODNO", "");
    LOG_INFO("[KIS] 취소 접수: " + ticker + " 원ODNO=" + orig_odno +
             " 취소ODNO=" + cancel_order_no);
    return OrderAck{std::move(cancel_order_no), std::string(), std::string()};
}

OrderAck KisClient::revise_order(const std::string& ticker, const std::string& orig_odno,
                                 const std::string& krx_forwarding_org_no, int new_quantity, double new_price)
{

    if (orig_odno.empty())
    {
        LOG_ERROR("[KIS] revise_order 원주문번호(ODNO) 없음 — " + ticker);
        return OrderAck::fail("E_NO_ORIG_ODNO");
    }

    std::string transaction_id = config_.is_paper ? "VTTC0013U" : "TTTC0013U";
    std::string url   = base_url() + "/uapi/domestic-stock/v1/trading/order-rvsecncl";

    json body = {{"CANO", config_.account_no},
                 {"ACNT_PRDT_CD", config_.account_type},
                 {"KRX_FWDG_ORD_ORGNO", krx_forwarding_org_no},
                 {"ORGN_ODNO", orig_odno},
                 {"ORD_DVSN", "00"},                              // 지정가
                 {"RVSE_CNCL_DVSN_CD", "01"},                     // 01=정정
                 {"ORD_QTY", std::to_string(new_quantity)},            // 정정 수량
                 {"ORD_UNPR", std::to_string(static_cast<int>(new_price))},    // 정정 단가
                 // QTY_ALL_ORD_YN="Y"는 KIS가 잔량 전체를 정정하게 하므로, 위 ORD_QTY(부분 정정
                 // 수량)는 실제로 반영되지 않는다. 현재 호출부는 단가 정정만 쓰므로 무해하나,
                 // 부분수량 정정이 필요해지면 "N"으로 바꾸고 ORD_QTY를 살려야 한다(보류 목록).
                 {"QTY_ALL_ORD_YN", "Y"},                         // 잔량 전체 정정
                 {"EXCG_ID_DVSN_CD", kis_order_exchange(config_)}}; // 원주문과 같은 거래소 구분 [why D-096]

    std::string response = http_post(url,
        authentication_headers(transaction_id, {"Content-Type: application/json"}),
        body.dump());

    if (response.empty())
    {
        LOG_ERROR("[KIS] revise_order 전송 실패: " + ticker + " ODNO=" + orig_odno);
        return OrderAck::fail(kis_error::kTransport);
    }

    json document;

    if (!kis_parse_order_response(response, document, "revise_order"))
    {
        return OrderAck::fail(kis_error::kTransport);
    }

    if (document["rt_cd"].get_ref<const std::string&>() != "0")
    {
        LOG_WARN("[KIS] 정정 거부: " + ticker + " ODNO=" + orig_odno + " — " +
                 document.value("msg1", std::string("")));
        return OrderAck::fail(kis_reject_code(document));
    }

    // 정정 성공 시 새 ODNO 발급 → 반환 (호출부가 kis_order_no 갱신)
    std::string new_order_no = jsonx::object_or_empty(document, "output").value("ODNO", "");
    LOG_INFO("[KIS] 정정 접수: " + ticker + " 원ODNO=" + orig_odno +
             " 새ODNO=" + new_order_no + " @" + std::to_string(static_cast<int>(new_price)));
    return OrderAck{std::move(new_order_no), std::string(), std::string()};
}

// ═══════════════════════════════════════════════════════════════════════════
//  해외 주식 주문
//  매수: TTTT1002U(실거래) / VTTT1002U(모의)
//  매도: TTTT1006U(실거래) / VTTT1006U(모의)
// ═══════════════════════════════════════════════════════════════════════════
bool KisClient::send_us_order(const OrderSignal& signal)
{
    std::string transaction_id;

    if (signal.side == OrderSide::BUY)
    {
        transaction_id = config_.is_paper ? "VTTT1002U" : "TTTT1002U";
    }
    else
    {
        transaction_id = config_.is_paper ? "VTTT1006U" : "TTTT1006U";
    }

    // KIS 해외주식 주문: 시장가 = ORD_DVSN "00", 가격 "0"
    json body = {{"CANO", config_.account_no},
                 {"ACNT_PRDT_CD", config_.account_type},
                 {"OVRS_EXCG_CD", signal.exchange.empty() ? "NASD" : signal.exchange},
                 {"PDNO", signal.ticker},
                 {"ORD_DVSN", "00"}, // 해외주식은 지정가(00)만 낸다. 시장가도 가격 "0"의 00으로 나간다.
                 {"ORD_QTY", std::to_string(signal.quantity)},
                 {"OVRS_ORD_UNPR", signal.type == OrderType::LIMIT ? std::to_string(signal.price) : "0"}};

    std::string url = base_url() + "/uapi/overseas-stock/v1/trading/order";
    std::string response = http_post(url,
                                 authentication_headers(transaction_id, {"Content-Type: application/json"}),
                                 body.dump());

    if (response.empty())
    {
        LOG_ERROR("[KIS-US] 주문 실패: " + signal.ticker);
        return false;
    }

    json document;

    if (!kis_parse_order_response(response, document, "send_us_order"))
    {
        return false;
    }

    bool ok = (document["rt_cd"].get_ref<const std::string&>() == "0");

    if (ok)
    {
        LOG_INFO("[KIS-US] 주문 성공: " + signal.ticker + (signal.side == OrderSide::BUY ? " BUY " : " SELL ") +
                 std::to_string(signal.quantity) + "주");
    }
    else
    {
        LOG_ERROR("[KIS-US] 주문 오류: " + document.value("msg1", "unknown"));
    }

    return ok;
}
