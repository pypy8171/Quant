// api/KisOrder.cpp — 국내·해외 주문 발주·정정·취소와 응답 파서. IOrderExecutor 구현부.
//  실패는 예외가 아니라 OrderAck.error_code 값으로 돌려준다(D-039). [why D-048] 파일 분할 경위.
#include "KisClientInternal.h"
#include "utils/JsonNode.h"
#include "core/KstTime.h"
#include "core/TickSize.h"

#include <cstring>

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

// 국내 주문은 시간대가 주문구분과 거래소를 함께 정한다. 2026-09-14 KRX 애프터마켓이 열리면서
//  구간마다 받는 값이 갈렸다.
//    09:00~15:30 정규장       — 시장가 01 · 지정가 00, 거래소는 설정값 그대로(SOR 라우팅이 산다)
//    15:40~16:00 장후 종가매매 — 06 하나뿐이고 단가는 0(종가로 체결된다). 시간외 단일가가 폐지된
//                               뒤에도 이 구간은 남았다
//    16:00~20:00 애프터마켓    — 접속매매라 실시간으로 체결되고, 전용 주문구분 41(지정가)·42(IOC)·
//                               43(FOK)만 받는다. 시장가는 없고 거래소는 KRX 로 못박아야 한다
//  실계좌에서 두 번 되돌아온 뒤에 얻은 배선이다(2026-09-23, HJ중공업 3주 청산 12회 거부).
//  최유리지정가(03)는 "최유리지정가호가불가 [APBK1943]", 그 다음에 넣은 지정가(00)는 거래소를
//  SOR 로 둔 탓에 "SOR 시장에서 거래가 불가능한 종목입니다 [APBK3009]" 로 막혔다. 모의계좌는
//  애프터마켓 주문 자체를 받지 않아 이 경로는 실증된 적이 없었다.
//  구간 바깥(예: 08:50~09:00)은 OrderGate 세션 창이 막으므로 여기서 다시 보지 않는다.
//  [why D-097] [why D-122]
static constexpr int32_t kClosingAuctionOpenHhmmss = 154000;  // 장후 종가매매 시작 15:40:00
static constexpr int32_t kAfterMarketOpenHhmmss    = 160000;  // 애프터마켓 시작 16:00:00
static constexpr int32_t kAfterMarketCloseHhmmss   = 200000;  // 애프터마켓 끝 20:00:00

enum class MarketSession
{
    Regular,         // 정규장
    ClosingAuction,  // 장후 시간외 종가매매
    AfterMarket      // 애프터마켓 접속매매
};

static MarketSession market_session_now()
{
    const int32_t hhmmss = kst::hhmmss_int(std::time(nullptr));

    if (hhmmss >= kClosingAuctionOpenHhmmss && hhmmss < kAfterMarketOpenHhmmss)
    {
        return MarketSession::ClosingAuction;
    }

    if (hhmmss >= kAfterMarketOpenHhmmss && hhmmss < kAfterMarketCloseHhmmss)
    {
        return MarketSession::AfterMarket;
    }

    return MarketSession::Regular;
}

static const char* kis_order_division(OrderType type, MarketSession session)
{
    if (session == MarketSession::ClosingAuction)
    {
        return "06";
    }

    if (session == MarketSession::AfterMarket)
    {
        return "41";
    }

    return type == OrderType::MARKET ? "01" : "00";
}

// 애프터마켓 시장가 신호를 대신할 지정가. 청산을 끝내는 것이 목적이라 현재가에서 한 걸음 물러선
//  자리에 호가를 얹는다 — 매도는 아래로, 매수는 위로. 한 걸음은 1%로 둔다.
//  krx::round_to_tick은 BUY=내림·SELL=올림(스프레드 보존)이라, 여기처럼 체결을 당기려는 자리에서는
//  반대쪽 side를 넘긴다.
//  현재가를 못 구하면 0 — 호출부가 주문을 접는다(가격 0인 지정가는 KIS가 거부한다).
static constexpr double kOffHoursPriceStepPct = 0.01;

static int offhours_limit_price(double current_price, OrderSide side)
{
    if (!(current_price > 0.0))
    {
        return 0;
    }

    const double    step          = side == OrderSide::SELL ? 1.0 - kOffHoursPriceStepPct : 1.0 + kOffHoursPriceStepPct;
    const OrderSide rounding_side = side == OrderSide::SELL ? OrderSide::BUY : OrderSide::SELL;
    return static_cast<int>(krx::round_to_tick(current_price * step, rounding_side));
}

// 애프터마켓 주문은 KRX로 나간다 — SOR로 보내면 "SOR 시장에서 거래가 불가능한 종목입니다
//  [APBK3009]"로 되돌아온다(2026-09-23 실계좌 실측). 그 밖의 구간은 설정값 그대로다. [why D-096]
static const char* kis_session_exchange(const KisConfig& config, MarketSession session)
{
    return session == MarketSession::AfterMarket ? "KRX" : kis_order_exchange(config);
}

// 취소·정정 구분은 부르는 시각의 구간으로 정한다 — 애프터마켓이면 41, 그 밖은 00(지정가).
//  원주문 구분(01 시장가·06 종가)을 그대로 쓰지 않는다. 애프터마켓 주문은 41로 나갔으므로 같은
//  구간 안에서 되부르면 41이 맞는다. [why D-122]
static const char* kis_amend_order_division(MarketSession session)
{
    return session == MarketSession::AfterMarket ? "41" : "00";
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

// ─── MM-1: 신규 주문 + KRX 조직번호 캡처 ──────────────────────────────────
//  응답에서 ODNO에 더해 KRX_FWDG_ORD_ORGNO를
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
        const MarketSession session        = market_session_now();
        const char*         order_division = kis_order_division(signal.type, session);
        int                 order_price    = signal.type == OrderType::LIMIT ? static_cast<int>(signal.price) : 0;

        if (session == MarketSession::ClosingAuction)
        {
            order_price = 0; // 장후 종가매매(06)는 종가로 체결된다 — 단가를 실으면 거부된다
        }
        else if (session == MarketSession::AfterMarket && signal.type == OrderType::MARKET)
        {
            // 애프터마켓 접속매매는 지정가(41)만 받는다. 현재가는 REST로 한 번 묻는다 —
            //  청산·정정은 드물어 이 왕복이 hot path가 아니다.
            order_price = offhours_limit_price(get_current_price(signal.ticker), signal.side);

            if (order_price <= 0)
            {
                LOG_ERROR("[KIS] 애프터마켓 주문에 실을 현재가를 못 구했다 — 주문하지 않는다 " + signal.ticker);
                return OrderAck::fail(kis_error::kTransport);
            }

            LOG_INFO("[KIS] 애프터마켓이라 시장가를 지정가 " + std::to_string(order_price) + "원으로 바꾼다 " + signal.ticker);
        }

        const char* order_exchange = kis_session_exchange(config_, session);

        body = {{"CANO", config_.account_no}, {"ACNT_PRDT_CD", config_.account_type},
                {"PDNO", signal.ticker},
                {"ORD_DVSN", order_division}, // 시간대가 정한다 — 정규장 01/00 · 종가 06 · 애프터 41
                {"ORD_QTY", std::to_string(signal.quantity)},
                {"ORD_UNPR", std::to_string(order_price)},
                {"EXCG_ID_DVSN_CD", order_exchange}};
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

    // rt_cd=0인데 ODNO가 비면 접수는 됐을 수 있고 번호만 모른다 — 전송 실패와 같이 '모름'으로 돌려
    //  라우터가 재시도하지 않고 브로커에 되묻게 한다(OrderAck [inv]: 실패면 error_code가 비지 않는다).
    if (acknowledgement.kis_order_no.empty())
    {
        LOG_ERROR("[KIS] 주문 응답에 ODNO 없음: " + signal.ticker + " — 접수 여부 확인 필요");
        return OrderAck::fail(kis_error::kTransport);
    }

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

    const MarketSession session = market_session_now();

    json body = {{"CANO", config_.account_no},
                 {"ACNT_PRDT_CD", config_.account_type},
                 {"KRX_FWDG_ORD_ORGNO", krx_forwarding_org_no},              // 원주문 조직번호
                 {"ORGN_ODNO", orig_odno},                       // 원주문번호
                 {"ORD_DVSN", kis_amend_order_division(session)}, // 지금 구간으로 정한다 — 애프터 41 · 그 밖 00
                 {"RVSE_CNCL_DVSN_CD", "02"},                    // 02=취소
                 {"ORD_QTY", std::to_string(quantity)},               // 취소 수량 (QTY_ALL_ORD_YN=Y면 무시됨)
                 {"ORD_UNPR", "0"},                              // 취소는 단가 0
                 {"QTY_ALL_ORD_YN", all_remaining ? "Y" : "N"}, // 잔량 전체 취소
                 {"EXCG_ID_DVSN_CD", kis_session_exchange(config_, session)}}; // 원주문과 같은 거래소 [why D-096]

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

    if (cancel_order_no.empty())
    {
        LOG_ERROR("[KIS] 취소 응답에 ODNO 없음: " + ticker + " 원ODNO=" + orig_odno + " — 취소 여부 모름");
        return OrderAck::fail(kis_error::kTransport);
    }

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

    const MarketSession session = market_session_now();

    json body = {{"CANO", config_.account_no},
                 {"ACNT_PRDT_CD", config_.account_type},
                 {"KRX_FWDG_ORD_ORGNO", krx_forwarding_org_no},
                 {"ORGN_ODNO", orig_odno},
                 {"ORD_DVSN", kis_amend_order_division(session)}, // 지금 구간으로 정한다 — 애프터 41 · 그 밖 00
                 {"RVSE_CNCL_DVSN_CD", "01"},                     // 01=정정
                 {"ORD_QTY", std::to_string(new_quantity)},            // 정정 수량
                 {"ORD_UNPR", std::to_string(static_cast<int>(new_price))},    // 정정 단가
                 // QTY_ALL_ORD_YN="Y"는 KIS가 잔량 전체를 정정하게 하므로, 위 ORD_QTY(부분 정정
                 // 수량)는 실제로 반영되지 않는다. 현재 호출부는 단가 정정만 쓰므로 무해하나,
                 // 부분수량 정정이 필요해지면 "N"으로 바꾸고 ORD_QTY를 살려야 한다(보류 목록).
                 {"QTY_ALL_ORD_YN", "Y"},                         // 잔량 전체 정정
                 {"EXCG_ID_DVSN_CD", kis_session_exchange(config_, session)}}; // 원주문과 같은 거래소 [why D-096]

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

    if (new_order_no.empty())
    {
        LOG_ERROR("[KIS] 정정 응답에 ODNO 없음: " + ticker + " 원ODNO=" + orig_odno + " — 정정 여부 모름");
        return OrderAck::fail(kis_error::kTransport);
    }

    LOG_INFO("[KIS] 정정 접수: " + ticker + " 원ODNO=" + orig_odno +
             " 새ODNO=" + new_order_no + " @" + std::to_string(static_cast<int>(new_price)));
    return OrderAck{std::move(new_order_no), std::string(), std::string()};
}
