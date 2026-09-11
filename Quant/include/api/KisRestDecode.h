#pragma once
// KIS REST 분봉 응답(JSON) → MarketData 집계봉. 헤더 전용·순수 함수.
// 로그·HTTP·인증 의존이 없어 테스트(`Quant/tests/test_kis_decode.cpp`)가 KisClient를 링크하지 않고 직접 부른다.
// 호출 스레드: 데이터 스레드(KisClient::get_minute_ohlcv*)와 테스트. 관련 결정: D-051.
//
// [wire] 당일 분봉(FHKST03010200)과 과거일 분봉(FHKST03010230)은 output2에 같은 필드명을 쓴다:
//  stck_bsop_date(YYYYMMDD)·stck_cntg_hour(HHMMSS)·stck_oprc·stck_hgpr·stck_lwpr·stck_prpr·cntg_vol.
//  숫자는 문자열이고, 빈 값·형식 오류는 0으로 둔다 — 봉 하나를 버리는 것보다 0 거래량이 낫다는 판단은 아니고,
//  둘을 구분할 신호가 응답에 없어서다.

#include "core/Types.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <nlohmann/json.hpp>
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

} // namespace kis_rest
