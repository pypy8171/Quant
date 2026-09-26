#include "api/KisRestDecode.h"

#include <cmath>

namespace kis_rest
{
double number(const nlohmann::json& node, const char* key)
{
    try
    {
        return std::stod(node.value(key, "0"));
    }
    catch (...)
    {
        return 0.0;
    }
}

time_t parse_dt(const std::string& data, const std::string& ticker)
{
    if (data.size() != 8 || ticker.size() < 6)
    {
        return 0;
    }

    using namespace std::chrono;
    year_month_day date_yyyymmdd;
    seconds time_of_day;

    try
    {
        date_yyyymmdd = year{std::stoi(data.substr(0, 4))} /
                        month{static_cast<unsigned>(std::stoi(data.substr(4, 2)))} /
                        day{static_cast<unsigned>(std::stoi(data.substr(6, 2)))};
        time_of_day = hours{std::stoi(ticker.substr(0, 2))} + minutes{std::stoi(ticker.substr(2, 2))} +
                      seconds{std::stoi(ticker.substr(4, 2))};
    }
    catch (...)
    {
        return 0;
    }

    if (!date_yyyymmdd.ok())
    {
        return 0;
    }

    return static_cast<time_t>((sys_days{date_yyyymmdd} + time_of_day).time_since_epoch().count());
}

std::vector<MarketData> aggregate_minutes(std::vector<RawMinute>& raw_minutes, const std::string& ticker,
                                          int interval_min, int count)
{
    std::vector<MarketData> result;

    if (raw_minutes.empty() || interval_min <= 0 || count <= 0)
    {
        return result;
    }

    std::sort(raw_minutes.begin(), raw_minutes.end(),
              [](const RawMinute& raw_minute_a, const RawMinute& raw_minute_b)
              {
                  return raw_minute_a.date != raw_minute_b.date ? raw_minute_a.date < raw_minute_b.date
                                                                : raw_minute_a.hour < raw_minute_b.hour;
              });

    std::vector<MarketData> ascending; // 과거→최신 집계봉
    // 열려 있는 버킷의 날짜·번호 — 문자열을 이어 붙여 키를 만들지 않는다.
    //  [inv] current_date는 raw_minutes 원소의 date를 본다 — 루프 동안 raw_minutes를 고치지 않는다
    std::string_view current_date;
    int current_bucket = -1;

    for (const auto& raw : raw_minutes)
    {
        // "HHMM…" 앞 네 자리만 읽는다 — substr 임시 없이 자리에서 숫자로 바꾼다
        int hour = 0, minute = 0;

        if (raw.hour.size() < 4)
        {
            continue;
        }

        const char* hour_text = raw.hour.data();
        const auto hour_result = std::from_chars(hour_text, hour_text + 2, hour);
        const auto minute_result = std::from_chars(hour_text + 2, hour_text + 4, minute);

        if (hour_result.ec != std::errc{} || minute_result.ec != std::errc{})
        {
            continue;
        }

        int bucket = (hour * 60 + minute) / interval_min;

        if (bucket != current_bucket || raw.date != current_date)
        {
            MarketData& market_data = ascending.emplace_back();
            market_data.ticker = ticker;
            market_data.market = Market::KR;
            market_data.open = raw.open;
            market_data.high = raw.high;
            market_data.low = raw.low;
            market_data.close = raw.close;
            market_data.volume = raw.volume;
            market_data.timestamp =
                std::chrono::system_clock::from_time_t(parse_dt(raw.date, raw.hour) - kst::kOffsetSec);
            current_date = raw.date;
            current_bucket = bucket;
        }
        else
        {
            MarketData& market_data = ascending.back();
            market_data.high = (std::max)(market_data.high, raw.high); // (): windows.h max 매크로 회피
            market_data.low = (std::min)(market_data.low, raw.low);
            market_data.close = raw.close; // 버킷 내 최신 마감
            market_data.volume += raw.volume;
            market_data.timestamp =
                std::chrono::system_clock::from_time_t(parse_dt(raw.date, raw.hour) - kst::kOffsetSec);
        }
    }

    for (auto iterator = ascending.rbegin(); iterator != ascending.rend() && static_cast<int>(result.size()) < count;
         ++iterator)
    {
        result.push_back(std::move(*iterator)); // ascending은 여기서 끝나는 지역 변수라 옮겨도 된다
        result.back().bar_index = static_cast<int>(result.size() - 1);
    }

    return result;
}

std::string parse_minute_page(const nlohmann::json& array, std::vector<RawMinute>& raw_minutes,
                              std::unordered_set<uint64_t>& seen, const std::string& date_filter, int& added_out)
{
    std::string page_earliest;
    added_out = 0;

    for (const auto& item : array)
    {
        std::string data = item.value("stck_bsop_date", "");
        std::string ticker = item.value("stck_cntg_hour", "");

        if (ticker.size() < 6)
        {
            continue;
        }

        if (page_earliest.empty() || ticker < page_earliest)
        {
            page_earliest = ticker;
        }

        if (!date_filter.empty() && data != date_filter)
        {
            continue; // 요청 날짜 밖 행 방어
        }

        // YYYYMMDD·HHMMSS 두 자릿수 문자열을 정수 하나로 — 행마다 문자열을 붙여 해시하지 않는다.
        if (!seen.insert(digits_to_number(data) * kTimeDigitsSpan + digits_to_number(ticker)).second)
        {
            continue; // 페이지 경계 중복
        }

        RawMinute raw_minute;
        raw_minute.date = std::move(data);
        raw_minute.hour = std::move(ticker);
        raw_minute.open = number(item, "stck_oprc");
        raw_minute.high = number(item, "stck_hgpr");
        raw_minute.low = number(item, "stck_lwpr");
        raw_minute.close = number(item, "stck_prpr");
        raw_minute.volume = static_cast<int64_t>(number(item, "cntg_vol"));
        raw_minutes.push_back(std::move(raw_minute));
        ++added_out;
    }

    return page_earliest;
}

std::optional<double> option_number(const nlohmann::json& node, const char* key)
{
    std::string text;

    try
    {
        text = node.value(key, "");
    }
    catch (...)
    {
        return std::nullopt;
    }

    if (text.empty())
    {
        return std::nullopt;
    }

    try
    {
        return std::stod(text);
    }
    catch (...)
    {
        return std::nullopt;
    }
}

Holding decode_holding(const nlohmann::json& node)
{
    Holding holding;
    holding.ticker = node.value("pdno", "");
    holding.name = node.value("prdt_name", "");
    holding.quantity = static_cast<int>(number(node, "hldg_qty"));
    holding.average_price = number(node, "pchs_avg_pric");
    holding.evaluation_pnl = number(node, "evlu_pfls_amt");
    holding.current_price = number(node, "prpr"); // 필드 확인 2026-09-26 KIS 공식 샘플 inquire_balance output1
    const std::string psbl = node.value("ord_psbl_qty", "");

    if (!psbl.empty() && psbl.find_first_not_of("0123456789 ") == std::string::npos)
    {
        holding.sellable_quantity = std::atoi(psbl.c_str());
    }

    return holding;
}

void decode_balance_page(const nlohmann::json& document, AccountBalance& out, bool first_page)
{
    if (document.contains("output1") && document["output1"].is_array())
    {
        for (const auto& holding_node : document["output1"])
        {
            Holding holding = decode_holding(holding_node);

            if (holding.ticker.empty() || holding.quantity <= 0)
            {
                continue;
            }

            out.holdings.push_back(std::move(holding));
        }
    }

    if (!first_page || !document.contains("output2"))
    {
        return;
    }

    const auto& output2_node = document["output2"];
    const nlohmann::json* row = nullptr;

    if (output2_node.is_array() && !output2_node.empty())
    {
        row = &output2_node[0];
    }
    else if (output2_node.is_object())
    {
        row = &output2_node;
    }

    if (!row)
    {
        return;
    }

    out.total_evaluation_amount = option_number(*row, "tot_evlu_amt");

    if (!out.total_evaluation_amount)
    {
        out.total_evaluation_amount = option_number(*row, "nass_amt"); // 순자산 폴백
    }

    // 주문가능현금 — 가수도정산금(미체결·미결제로 묶인 몫이 빠진 실질 상한)을 우선, 없으면 예수금.
    out.available_cash = option_number(*row, "prvs_rcdl_excc_amt");

    if (!out.available_cash)
    {
        out.available_cash = option_number(*row, "dnca_tot_amt");
    }

    out.previous_day_total_asset = option_number(*row, "bfdy_tot_asst_evlu_amt");
}

std::vector<FutureContract> decode_future_board(const nlohmann::json& document)
{
    std::vector<FutureContract> out;

    for (const char* key : {"output1", "output2", "output"})
    {
        if (!document.contains(key) || !document[key].is_array() || document[key].empty())
        {
            continue;
        }

        for (const auto& row : document[key])
        {
            FutureContract future_contract;
            future_contract.issue_code = row.value("futs_shrn_iscd", "");
            future_contract.name = row.value("hts_kor_isnm", "");

            if (!future_contract.issue_code.empty())
            {
                out.push_back(std::move(future_contract));
            }
        }

        break;
    }

    return out;
}


namespace
{
std::string trim_right(std::string text)
{
    while (!text.empty() && (text.back() == ' ' || text.back() == '	'))
    {
        text.pop_back();
    }

    return text;
}
} // namespace

KisResult<OpenOrderPage> decode_open_order_page(std::string_view response, bool paper)
{
    if (response.empty())
    {
        return kis_fail("transport", "미체결 조회 응답 없음");
    }

    const nlohmann::json document = nlohmann::json::parse(response, nullptr, /*allow_exceptions=*/false);

    if (document.is_discarded() || !document.is_object())
    {
        return kis_fail("parse", "미체결 조회 응답 해석 실패");
    }

    if (document.value("rt_cd", std::string()) != "0")
    {
        return kis_fail(document.value("msg_cd", std::string()), document.value("msg1", std::string()));
    }

    OpenOrderPage page;
    // 응답 배열 이름과 수량 필드가 두 엔드포인트에서 다르다. [inv] rows는 document가 사는 동안만 유효하다.
    const auto rows = document.find(paper ? "output1" : "output");

    if (rows != document.end() && rows->is_array())
    {
        for (const auto& node : *rows)
        {
            if (paper && node.value("cncl_yn", std::string()) == "Y")
            {
                continue; // 이미 취소된 주문
            }

            OpenOrder open_order;
            open_order.ticker                = node.value("pdno", std::string());
            open_order.name                  = node.value("prdt_name", std::string());
            open_order.kis_order_no          = node.value("odno", node.value("ODNO", std::string()));
            open_order.krx_forwarding_org_no = node.value("ord_gno_brno", std::string());
            open_order.psbl_qty              = static_cast<int>(number(node, paper ? "rmn_qty" : "psbl_qty"));
            open_order.ord_unpr              = number(node, "ord_unpr");
            const std::string buy_sell_code  = node.value("sll_buy_dvsn_cd", std::string());
            open_order.side = buy_sell_code == "01" ? OrderSide::SELL
                              : buy_sell_code == "02" ? OrderSide::BUY
                                                      : OrderSide::NONE;

            if (!open_order.ticker.empty() && open_order.psbl_qty > 0)
            {
                page.rows.push_back(std::move(open_order));
            }
        }
    }

    page.forward_key = trim_right(document.value("ctx_area_fk100", std::string()));
    page.next_key    = trim_right(document.value("ctx_area_nk100", std::string()));
    return page;
}

KisResult<DailyFillPage> decode_daily_fill_page(std::string_view response)
{
    if (response.empty())
    {
        return kis_fail("transport", "일별주문체결 조회 응답 없음");
    }

    const nlohmann::json document = nlohmann::json::parse(response, nullptr, /*allow_exceptions=*/false);

    if (document.is_discarded() || !document.is_object())
    {
        return kis_fail("parse", "일별주문체결 조회 응답 해석 실패");
    }

    if (document.value("rt_cd", std::string()) != "0")
    {
        return kis_fail(document.value("msg_cd", std::string()), document.value("msg1", std::string()));
    }

    DailyFillPage page;
    const auto    rows = document.find("output1");

    if (rows != document.end() && rows->is_array())
    {
        for (const auto& node : *rows)
        {
            DailyOrderFill fill;
            fill.kis_order_no      = node.value("odno", std::string());
            fill.original_order_no = node.value("orgn_odno", std::string());
            fill.ticker            = node.value("pdno", std::string());
            fill.order_quantity    = static_cast<int>(number(node, "ord_qty"));
            fill.filled_quantity   = static_cast<int>(number(node, "tot_ccld_qty"));
            fill.filled_amount     = std::llround(number(node, "tot_ccld_amt"));
            const std::string buy_sell_code = node.value("sll_buy_dvsn_cd", std::string());
            fill.side = buy_sell_code == "01" ? OrderSide::SELL
                        : buy_sell_code == "02" ? OrderSide::BUY
                                                : OrderSide::NONE;

            if (!fill.kis_order_no.empty() && fill.filled_quantity > 0)
            {
                page.rows.push_back(std::move(fill));
            }
        }
    }

    page.forward_key = trim_right(document.value("ctx_area_fk100", std::string()));
    page.next_key    = trim_right(document.value("ctx_area_nk100", std::string()));
    return page;
}

} // namespace kis_rest
