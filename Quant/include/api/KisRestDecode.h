#pragma once
// KIS REST 응답(JSON) → 값 타입. 분봉 → MarketData 집계봉, 잔고 → AccountBalance, 선물 전광판 → FutureContract.
// 헤더 전용·순수 함수. 로그·HTTP·인증 의존이 없어 테스트(`Quant/tests/test_kis_decode.cpp`)가 KisClient를
// 링크하지 않고 직접 부른다. 호출 스레드: 데이터 스레드(KisClient::get_minute_ohlcv*·get_balance)와 테스트.
// 관련 결정: D-051(분봉), D-059(잔고·전광판).
//
// [wire] 당일 분봉(FHKST03010200)과 과거일 분봉(FHKST03010230)은 output2에 같은 필드명을 쓴다:
//  stck_bsop_date(YYYYMMDD)·stck_cntg_hour(HHMMSS)·stck_oprc·stck_hgpr·stck_lwpr·stck_prpr·cntg_vol.
//  숫자는 문자열이고, 빈 값·형식 오류는 0으로 둔다 — 봉 하나를 버리는 것보다 0 거래량이 낫다는 판단은 아니고,
//  둘을 구분할 신호가 응답에 없어서다.

#include "api/KisTypes.h"
#include "core/KstTime.h"
#include "core/Types.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace kis_rest
{

// 문자열 숫자 필드 → double. 키 없음·빈 값·파싱 실패·문자열이 아닌 값은 0.
//  키는 리터럴이라 const char* — json이 투명 비교자를 써서 std::string을 만들지 않는다.
inline double number(const nlohmann::json& node, const char* key)
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

// YYYYMMDD + HHMMSS → time_t. 자리값을 UTC로 읽는다 — KST 오프셋은 호출자가 뺀다. 서버 TZ와 무관하다.
//  형식이 아니거나 달력에 없는 날짜(13월·2월 30일)면 0.
inline time_t parse_dt(const std::string& data, const std::string& ticker)
{
    if (data.size() != 8 || ticker.size() < 6)
    {
        return 0;
    }

    using namespace std::chrono;
    year_month_day date_yyyymmdd;
    seconds        time_of_day;

    try
    {
        date_yyyymmdd = year{std::stoi(data.substr(0, 4))} / month{static_cast<unsigned>(std::stoi(data.substr(4, 2)))} /
              day{static_cast<unsigned>(std::stoi(data.substr(6, 2)))};
        time_of_day = hours{std::stoi(ticker.substr(0, 2))} + minutes{std::stoi(ticker.substr(2, 2))} + seconds{std::stoi(ticker.substr(4, 2))};
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

// 1분봉 원본 한 행.
struct RawMinute
{
    std::string date, hour;
    double open = 0, high = 0, low = 0, close = 0;
    int64_t volume = 0;
};

// 1분봉 원본 → interval_min 집계봉. 반환은 최신→과거(result[0]=최신, bar_index 0=최신), 최대 count봉.
//  raws는 정렬을 위해 제자리에서 바뀐다. 두 분봉 TR이 같은 집계를 쓰므로 한 곳에 둔다.
//  버킷은 시계 정렬((hour*60+minute)/interval_min)이라 09:00 기준 3분봉은 09:00·09:03·… 으로 떨어진다.
//  timestamp는 버킷 마지막 1분의 진짜 UTC다(KST 라벨 − 9h). 일봉(now)·체결 틱과 같은 축이어야 집계기 시드와
//  resample이 KST 분을 바로 읽는다 — 라벨을 UTC처럼 두면 시드 자리가 9시간 밀려 틱이 전부 버려진다. [why D-072]
inline std::vector<MarketData> aggregate_minutes(std::vector<RawMinute>& raw_minutes, const std::string& ticker,
                                                 int interval_min, int count)
{
    std::vector<MarketData> result;

    if (raw_minutes.empty() || interval_min <= 0 || count <= 0)
    {
        return result;
    }

    std::sort(raw_minutes.begin(), raw_minutes.end(), [](const RawMinute& raw_minute_a, const RawMinute& raw_minute_b) {
        return raw_minute_a.date != raw_minute_b.date ? raw_minute_a.date < raw_minute_b.date : raw_minute_a.hour < raw_minute_b.hour;
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
        const auto  hour_result = std::from_chars(hour_text, hour_text + 2, hour);
        const auto  minute_result = std::from_chars(hour_text + 2, hour_text + 4, minute);

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
            market_data.timestamp = std::chrono::system_clock::from_time_t(parse_dt(raw.date, raw.hour) - kst::kOffsetSec);
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
            market_data.timestamp = std::chrono::system_clock::from_time_t(parse_dt(raw.date, raw.hour) - kst::kOffsetSec);
        }
    }

    for (auto iterator = ascending.rbegin(); iterator != ascending.rend() && static_cast<int>(result.size()) < count; ++iterator)
    {
        result.push_back(std::move(*iterator)); // ascending은 여기서 끝나는 지역 변수라 옮겨도 된다
        result.back().bar_index = static_cast<int>(result.size() - 1);
    }

    return result;
}

// output2(최신→과거) 한 페이지 → raws에 누적. seen(날짜·시각을 한 정수로)으로 페이지 경계 중복을 걸러내고,
//  이 페이지에서 가장 이른 HHMMSS를 돌려준다(역페이징 커서 — 날짜 필터·중복과 무관하게 모든 행을 본다).
//  date_filter가 비어 있지 않으면 그 날짜 행만 취한다. added_out은 이번 호출로 raws에 더한 행 수.
constexpr uint64_t kTimeDigitsSpan = 1'000'000; // HHMMSS 여섯 자리 — 날짜를 그 위 자리로 올린다

inline std::string parse_minute_page(const nlohmann::json& array, std::vector<RawMinute>& raw_minutes,
                                     std::unordered_set<uint64_t>& seen, const std::string& date_filter,
                                     int& added_out)
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

// 문자열 숫자 필드 → optional<double>. 키 없음·빈 값·숫자 아님은 비어 있음 — number()의 0과 달리 "없다"를 남긴다.
//  잔고 요약처럼 0원과 필드 부재를 구분해야 하는 곳에 쓴다.
inline std::optional<double> option_number(const nlohmann::json& node, const char* key)
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

// 잔고 output1 한 행 → Holding. ord_psbl_qty는 숫자(공백 허용)일 때만 채운다 — 09-08에 이 필드를 보유수량으로
//  대신 썼다가 전량 청산이 40240000으로 통째 거부된 적이 있어, 못 읽은 것은 못 읽었다고 남긴다.
inline Holding decode_holding(const nlohmann::json& node)
{
    Holding holding;
    holding.ticker    = node.value("pdno", "");
    holding.name      = node.value("prdt_name", "");
    holding.quantity       = static_cast<int>(number(node, "hldg_qty"));
    holding.average_price = number(node, "pchs_avg_pric");
    holding.evaluation_pnl  = number(node, "evlu_pfls_amt");
    const std::string psbl = node.value("ord_psbl_qty", "");

    if (!psbl.empty() && psbl.find_first_not_of("0123456789 ") == std::string::npos)
    {
        holding.sellable_quantity = std::atoi(psbl.c_str());
    }

    return holding;
}

// 잔고 응답 한 페이지 → out에 누적. output1 행은 pdno가 비거나 수량 0 이하면 버린다(잔고는 매도 완료 종목을
//  0주로 며칠 남긴다). 요약(output2)은 first_page일 때만 읽는다 — 배열로도 객체로도 온다.
inline void decode_balance_page(const nlohmann::json& document, AccountBalance& out, bool first_page)
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

// 선물 전광판 응답 → 계약 목록. 행 배열은 output1·output2·output 중 처음 비어 있지 않은 것이다
//  (실키 응답이 어느 키로 오는지 문서가 못 박지 않아 셋을 본다). 코드가 빈 행은 버린다.
inline std::vector<FutureContract> decode_future_board(const nlohmann::json& document)
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

} // namespace kis_rest
