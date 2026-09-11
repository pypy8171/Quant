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
#include "core/Types.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace kis_rest
{

// 문자열 숫자 필드 → double. 키 없음·빈 값·파싱 실패·문자열이 아닌 값은 0.
inline double num(const nlohmann::json& o, const std::string& k)
{
    try
    {
        return std::stod(o.value(k, "0"));
    }
    catch (...)
    {
        return 0.0;
    }
}

// YYYYMMDD + HHMMSS → time_t. 서버 TZ와 무관하게 gmtime 계열로 통일한다 — KST 오프셋은 호출자가 안 다룬다.
//  형식이 아니면 0.
inline time_t parse_dt(const std::string& d, const std::string& t)
{
    if (d.size() != 8 || t.size() < 6)
    {
        return 0;
    }

    struct tm tmv{};

    try
    {
        tmv.tm_year = std::stoi(d.substr(0, 4)) - 1900;
        tmv.tm_mon = std::stoi(d.substr(4, 2)) - 1;
        tmv.tm_mday = std::stoi(d.substr(6, 2));
        tmv.tm_hour = std::stoi(t.substr(0, 2));
        tmv.tm_min = std::stoi(t.substr(2, 2));
        tmv.tm_sec = std::stoi(t.substr(4, 2));
    }
    catch (...)
    {
        return 0;
    }

#ifdef _WIN32
    return _mkgmtime(&tmv);
#else
    return timegm(&tmv);
#endif
}

// 1분봉 원본 한 행.
struct RawMinute
{
    std::string date, hour;
    double o = 0, h = 0, l = 0, c = 0;
    int64_t v = 0;
};

// 1분봉 원본 → interval_min 집계봉. 반환은 최신→과거(result[0]=최신, bar_index 0=최신), 최대 count봉.
//  raws는 정렬을 위해 제자리에서 바뀐다. 두 분봉 TR이 같은 집계를 쓰므로 한 곳에 둔다.
//  버킷은 시계 정렬((hh*60+mm)/interval_min)이라 09:00 기준 3분봉은 09:00·09:03·… 으로 떨어진다.
inline std::vector<MarketData> aggregate_minutes(std::vector<RawMinute>& raws, const std::string& ticker,
                                                 int interval_min, int count)
{
    std::vector<MarketData> result;

    if (raws.empty() || interval_min <= 0 || count <= 0)
    {
        return result;
    }

    std::sort(raws.begin(), raws.end(), [](const RawMinute& a, const RawMinute& b) {
        return a.date != b.date ? a.date < b.date : a.hour < b.hour;
    });

    std::vector<MarketData> asc; // 과거→최신 집계봉
    std::string cur_key;

    for (const auto& r : raws)
    {
        int hh = 0, mm = 0;

        try
        {
            hh = std::stoi(r.hour.substr(0, 2));
            mm = std::stoi(r.hour.substr(2, 2));
        }
        catch (...)
        {
            continue;
        }

        int bucket = (hh * 60 + mm) / interval_min;
        std::string key = r.date + ":" + std::to_string(bucket);

        if (key != cur_key)
        {
            MarketData md;
            md.ticker = ticker;
            md.market = Market::KR;
            md.open = r.o;
            md.high = r.h;
            md.low = r.l;
            md.close = r.c;
            md.volume = r.v;
            md.timestamp = std::chrono::system_clock::from_time_t(parse_dt(r.date, r.hour));
            asc.push_back(md);
            cur_key = key;
        }
        else
        {
            MarketData& md = asc.back();
            md.high = (std::max)(md.high, r.h); // (): windows.h max 매크로 회피
            md.low = (std::min)(md.low, r.l);
            md.close = r.c; // 버킷 내 최신 마감
            md.volume += r.v;
            md.timestamp = std::chrono::system_clock::from_time_t(parse_dt(r.date, r.hour));
        }
    }

    for (auto it = asc.rbegin(); it != asc.rend() && static_cast<int>(result.size()) < count; ++it)
    {
        MarketData md = *it;
        md.bar_index = static_cast<int>(result.size());
        result.push_back(md);
    }

    return result;
}

// output2(최신→과거) 한 페이지 → raws에 누적. seen(date+hour)으로 페이지 경계 중복을 걸러내고,
//  이 페이지에서 가장 이른 HHMMSS를 돌려준다(역페이징 커서 — 날짜 필터·중복과 무관하게 모든 행을 본다).
//  date_filter가 비어 있지 않으면 그 날짜 행만 취한다. added_out은 이번 호출로 raws에 더한 행 수.
inline std::string parse_minute_page(const nlohmann::json& arr, std::vector<RawMinute>& raws,
                                     std::unordered_set<std::string>& seen, const std::string& date_filter,
                                     int& added_out)
{
    std::string page_earliest;
    added_out = 0;

    for (const auto& item : arr)
    {
        std::string d = item.value("stck_bsop_date", "");
        std::string t = item.value("stck_cntg_hour", "");

        if (t.size() < 6)
        {
            continue;
        }

        if (page_earliest.empty() || t < page_earliest)
        {
            page_earliest = t;
        }

        if (!date_filter.empty() && d != date_filter)
        {
            continue; // 요청 날짜 밖 행 방어
        }

        if (!seen.insert(d + t).second)
        {
            continue; // 페이지 경계 중복
        }

        RawMinute r;
        r.date = d;
        r.hour = t;
        r.o = num(item, "stck_oprc");
        r.h = num(item, "stck_hgpr");
        r.l = num(item, "stck_lwpr");
        r.c = num(item, "stck_prpr");
        r.v = static_cast<int64_t>(num(item, "cntg_vol"));
        raws.push_back(r);
        ++added_out;
    }

    return page_earliest;
}

// 문자열 숫자 필드 → optional<double>. 키 없음·빈 값·숫자 아님은 비어 있음 — num()의 0과 달리 "없다"를 남긴다.
//  잔고 요약처럼 0원과 필드 부재를 구분해야 하는 곳에 쓴다.
inline std::optional<double> opt_num(const nlohmann::json& o, const std::string& k)
{
    std::string s;

    try
    {
        s = o.value(k, "");
    }
    catch (...)
    {
        return std::nullopt;
    }

    if (s.empty())
    {
        return std::nullopt;
    }

    try
    {
        return std::stod(s);
    }
    catch (...)
    {
        return std::nullopt;
    }
}

// 잔고 output1 한 행 → Holding. ord_psbl_qty는 숫자(공백 허용)일 때만 채운다 — 09-08에 이 필드를 보유수량으로
//  대신 썼다가 전량 청산이 40240000으로 통째 거부된 적이 있어, 못 읽은 것은 못 읽었다고 남긴다.
inline Holding decode_holding(const nlohmann::json& h)
{
    Holding r;
    r.ticker    = h.value("pdno", "");
    r.name      = h.value("prdt_name", "");
    r.qty       = static_cast<int>(num(h, "hldg_qty"));
    r.avg_price = num(h, "pchs_avg_pric");
    r.eval_pnl  = num(h, "evlu_pfls_amt");
    const std::string psbl = h.value("ord_psbl_qty", "");

    if (!psbl.empty() && psbl.find_first_not_of("0123456789 ") == std::string::npos)
    {
        r.sellable_qty = std::atoi(psbl.c_str());
    }

    return r;
}

// 잔고 응답 한 페이지 → out에 누적. output1 행은 pdno가 비거나 수량 0 이하면 버린다(잔고는 매도 완료 종목을
//  0주로 며칠 남긴다). 요약(output2)은 first_page일 때만 읽는다 — 배열로도 객체로도 온다.
inline void decode_balance_page(const nlohmann::json& j, AccountBalance& out, bool first_page)
{
    if (j.contains("output1") && j["output1"].is_array())
    {
        for (const auto& h : j["output1"])
        {
            Holding hd = decode_holding(h);

            if (hd.ticker.empty() || hd.qty <= 0)
            {
                continue;
            }

            out.holdings.push_back(std::move(hd));
        }
    }

    if (!first_page || !j.contains("output2"))
    {
        return;
    }

    const auto& o2 = j["output2"];
    const nlohmann::json* row = nullptr;

    if (o2.is_array() && !o2.empty())
    {
        row = &o2[0];
    }
    else if (o2.is_object())
    {
        row = &o2;
    }

    if (!row)
    {
        return;
    }

    out.total_eval_amt = opt_num(*row, "tot_evlu_amt");

    if (!out.total_eval_amt)
    {
        out.total_eval_amt = opt_num(*row, "nass_amt"); // 순자산 폴백
    }

    // 주문가능현금 — 가수도정산금(미체결·미결제로 묶인 몫이 빠진 실질 상한)을 우선, 없으면 예수금.
    out.available_cash = opt_num(*row, "prvs_rcdl_excc_amt");

    if (!out.available_cash)
    {
        out.available_cash = opt_num(*row, "dnca_tot_amt");
    }

    out.prev_day_total_asset = opt_num(*row, "bfdy_tot_asst_evlu_amt");
}

// 선물 전광판 응답 → 계약 목록. 행 배열은 output1·output2·output 중 처음 비어 있지 않은 것이다
//  (실키 응답이 어느 키로 오는지 문서가 못 박지 않아 셋을 본다). 코드가 빈 행은 버린다.
inline std::vector<FutureContract> decode_future_board(const nlohmann::json& j)
{
    std::vector<FutureContract> out;

    for (const char* key : {"output1", "output2", "output"})
    {
        if (!j.contains(key) || !j[key].is_array() || j[key].empty())
        {
            continue;
        }

        for (const auto& r : j[key])
        {
            FutureContract c;
            c.iscd = r.value("futs_shrn_iscd", "");
            c.name = r.value("hts_kor_isnm", "");

            if (!c.iscd.empty())
            {
                out.push_back(std::move(c));
            }
        }

        break;
    }

    return out;
}

} // namespace kis_rest
