#include "universe/UniverseScanner.h"
#include "utils/JsonNode.h"
#include "universe/MaAlign.h"
#include "core/KstTime.h"
#include "core/Types.h"
#include "utils/EtfFilter.h"
#include "utils/Logger.h"
#include <algorithm>
#include <functional>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace universe
{

namespace
{
// ── 일봉 정배열 판정 캐시 ───────────────────────────────────────────────
//  캐시하는 것은 정배열 판정이 아니라 그 재료인 확정된 과거 일봉이다.
//  KIS 일봉을 include_today=false로 받으므로(D-005) d[0]은 전일 확정봉이고 장중에
//  바뀔 일이 없다. 당일봉은 따로 받지 않고 현재가를 SMA에 직접 접어 넣는다 —
//  s_n_live = (s_n*n - r_n + px_live) / n. 그래서 정배열·이격 판정은 재스캔마다
//  새 값으로 다시 나오고, REST만 하루 1회로 줄어든다.
//  px_live는 랭킹 축이 실어오는 전 종목 시세 파일(네이버 벌크)에서 온다. 이 파일이
//  끊기면 price가 전일 종가로 돌아가 판정이 정말로 얼어붙는다 — 나이를 경고로 내보낸다.
//  캐시가 없던 때는 재스캔마다 후보 전체의 일봉을 다시 받았고, 그 비용이
//  rescan_interval에 반비례해 후보 집합을 넓히는 것 자체가 막혔다(align_probe_max가 그 캡).
//  ATR·봉수처럼 확정봉만 쓰는 값은 그대로 하루 고정이다.
struct DailyProbe
{
    std::string date_yyyymmdd;                       // 조회 시각의 로컬 날짜(YYYYMMDD)
    int    bars  = 0;                      // 확보 봉수(<60이면 판정 불가)
    double s5 = 0.0, s10 = 0.0, s20 = 0.0, s60 = 0.0;
    double close = 0.0;                    // 최신 종가(d[0])
    // [formula] SMA에 오늘 가격을 접어 넣을 때 빠지는 봉의 종가.
    //  s_n_live = (s_n*n - roll_n + px_live) / n — REST 없이 정배열을 장중 갱신한다.
    double r5 = 0.0, r10 = 0.0, r20 = 0.0, r60 = 0.0;
    double atr_pct = 0.0;                  // ATR(14)/종가. 정배열 판정용 일봉 재활용(추가 REST 0)
    std::time_t at = 0;                    // 마지막 조회 시각. 장중 재조회 순번을 이걸로 정한다
    // 저항·거래량 축(2026-09-11 회의 §3). 봉이 모자라면 있는 만큼으로 잰다. 0=미산출.
    double hi250    = 0.0;                 // 확보 봉 안 최고가(align_daily_n=250이면 52주 고가)
    double pivot_hi = 0.0;                 // 최근 스윙 고점 — 좌우 5봉보다 높은 고가 중 가장 최근(당일 제외)
    double avg_vol20 = 0.0;                // 20일 평균 거래량(주). 장중 누적거래량 배율의 분모
    double close21  = 0.0;                 // 21봉 전 종가(≈1개월 수익률 분모)
};

// 일봉 요약 캐시. 스캔 스레드 하나가 쓰지만 재조회 대상 선정과 조회가 같은 맵을
//  보므로 락으로 감싼다. 디스크 사본은 장중 재기동 대비다 — 메모리 캐시가 비면 후보
//  수백 건의 일봉을 150ms 간격으로 다시 받아야 하고 그동안 발주 경로의 REST까지 밀린다.
//  확정된 과거 일봉이라 같은 거래일 안에서는 그대로 재사용해도 된다. 파일은 거래일별로
//  나누므로 날짜가 바뀌면 자연히 무시된다.
class DailyProbeCache
{
public:
    // 프로세스당 거래일 1회. 읽기 실패는 캐시 미스와 결과가 같으므로 경고만 남긴다.
    void load_today(const std::string& date_yyyymmdd)
    {
        if (loaded_ == date_yyyymmdd)
        {
            return;
        }

        loaded_ = date_yyyymmdd;
        std::ifstream file(cache_path(date_yyyymmdd));

        if (!file)
        {
            return;
        }

        int count = 0;

        try
        {
            nlohmann::json document;
            file >> document;

            if (!document.is_object())
            {
                return;
            }

            std::lock_guard<std::mutex> lock(mu_);

            for (auto iterator = document.begin(); iterator != document.end(); ++iterator)
            {
                // 12칸은 구버전 파일이다 — 뒤 4칸(저항·거래량 축)은 0으로 두고 그대로 쓴다.
                if (!iterator.value().is_array() || iterator.value().size() < 12)
                {
                    continue;
                }

                const auto& value = iterator.value();
                DailyProbe pr;
                pr.date_yyyymmdd     = date_yyyymmdd;
                pr.bars    = value[0].get<int>();
                pr.s5      = value[1].get<double>();
                pr.s10     = value[2].get<double>();
                pr.s20     = value[3].get<double>();
                pr.s60     = value[4].get<double>();
                pr.close   = value[5].get<double>();
                pr.r5      = value[6].get<double>();
                pr.r10     = value[7].get<double>();
                pr.r20     = value[8].get<double>();
                pr.r60     = value[9].get<double>();
                pr.atr_pct = value[10].get<double>();
                pr.at      = static_cast<std::time_t>(value[11].get<long long>());

                if (value.size() >= 16)
                {
                    pr.hi250     = value[12].get<double>();
                    pr.pivot_hi  = value[13].get<double>();
                    pr.avg_vol20 = value[14].get<double>();
                    pr.close21   = value[15].get<double>();
                }

                map_[iterator.key()] = pr;
                ++count;
            }
        }
        catch (const std::exception& exception)
        {
            LOG_WARN(std::string("[Main] 일봉 캐시 파일 읽기 실패: ") + exception.what());
            return;
        }

        if (count > 0)
        {
            LOG_INFO("[Main] 일봉 캐시 " + std::to_string(count) + "종목을 파일에서 복원했다(" + date_yyyymmdd + ")");
        }
    }

    // 쓰다 만 파일을 다음 기동이 읽지 않도록 임시 파일에 쓰고 바꿔치운다.
    void save_today(const std::string& date_yyyymmdd) const
    {
        // [wire] 값 순서: bars, s5, s10, s20, s60, close, r5, r10, r20, r60, atr_pct, at,
        //  hi250, pivot_hi, avg_vol20, close21 (뒤 4칸은 나중에 붙었다 — 읽을 때 없어도 된다)
        nlohmann::json document = nlohmann::json::object();
        {
            std::lock_guard<std::mutex> lock(mu_);

            for (const auto& entry : map_)
            {
                const DailyProbe& pr = entry.second;

                if (pr.date_yyyymmdd != date_yyyymmdd)
                {
                    continue;
                }

                document[entry.first] = nlohmann::json::array({pr.bars, pr.s5, pr.s10, pr.s20, pr.s60,
                                                     pr.close, pr.r5, pr.r10, pr.r20, pr.r60,
                                                     pr.atr_pct, static_cast<long long>(pr.at),
                                                     pr.hi250, pr.pivot_hi, pr.avg_vol20, pr.close21});
            }
        }

        const std::string path = cache_path(date_yyyymmdd);
        const std::string tmp  = path + ".tmp";

        try
        {
            {
                std::ofstream file(tmp, std::ios::trunc);

                if (!file)
                {
                    return;
                }

                file << document.dump();
            }

            std::error_code error_code;
            std::filesystem::rename(tmp, path, error_code);

            if (error_code)
            {
                std::filesystem::remove(tmp, error_code);
            }
        }
        catch (const std::exception& exception)
        {
            LOG_WARN(std::string("[Main] 일봉 캐시 파일 쓰기 실패: ") + exception.what());
        }
    }

    // 오늘치가 있으면 채우고 true. 날짜가 다르면 미스로 본다.
    bool get(const std::string& ticker, const std::string& date_yyyymmdd, DailyProbe& out) const
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto iterator = map_.find(ticker);

        if (iterator == map_.end() || iterator->second.date_yyyymmdd != date_yyyymmdd)
        {
            return false;
        }

        out = iterator->second;
        return true;
    }

    void put(const std::string& ticker, const DailyProbe& pr)
    {
        std::lock_guard<std::mutex> lock(mu_);
        map_[ticker] = pr;
    }

    // 장중 재조회 대상 고르기 — 판정 재료인 현재가는 장중 내내 변하지만 일봉 요약은
    //  조회 시각에 묶여 있다. 날짜만 보고 히트시키면 기동 시각의 판정이 마감까지 얼어붙어
    //  재스캔이 같은 종목만 돌려준다. 그렇다고 매번 전량을 다시 조회할 수는 없다 — 3분봉
    //  폴링이 이미 REST 초당 한도를 쓰고 있어 수백 건을 더 얹으면 발주 경로까지 밀린다.
    //  그래서 가장 오래 안 본 순으로 예산만큼만 다시 본다. 재스캔이 반복되면 후보 전체를
    //  순회하게 되고, 한 바퀴에 걸리는 시간은 후보수/예산 × 재스캔주기다.
    //  미조회분은 넣지 않는다 — 어차피 미스라 조회 경로로 간다.
    std::unordered_set<std::string> stale_targets(const std::vector<std::string>& cand,
                                                  const std::string& date_yyyymmdd, std::time_t fresh_sec,
                                                  int budget, std::size_t& considered) const
    {
        const std::time_t now_t = std::time(nullptr);
        std::vector<std::pair<std::time_t, std::string>> stale;
        {
            std::lock_guard<std::mutex> lock(mu_);

            for (const auto& candidate : cand)
            {
                auto iterator = map_.find(candidate);

                if (iterator == map_.end() || iterator->second.date_yyyymmdd != date_yyyymmdd)
                {
                    continue;
                }

                if (now_t - iterator->second.at < fresh_sec)
                {
                    continue;
                }

                stale.emplace_back(iterator->second.at, candidate);
            }
        }

        std::ranges::sort(stale, {}, &std::pair<std::time_t, std::string>::first);
        considered = stale.size();
        const std::size_t take = std::min<std::size_t>(stale.size(), static_cast<std::size_t>(budget));
        std::unordered_set<std::string> out;

        for (std::size_t take_index = 0; take_index < take; ++take_index)
        {
            out.insert(stale[take_index].second);
        }

        return out;
    }

private:
    static std::string cache_path(const std::string& date_yyyymmdd)
    {
        return Logger::instance().path_for("daily_probe_" + date_yyyymmdd + ".json").string();
    }

    mutable std::mutex mu_;
    std::unordered_map<std::string, DailyProbe> map_;
    std::string loaded_;
};
DailyProbeCache g_probe_cache;

// 전 종목 장중 시세. 시세 파일(네이버 벌크)이 준 시장 전체에 랭킹 축 스냅샷가가 덮인다.
//  후보만이 아니라 시장 전체를 담는다 — 전 종목 확장 축이 이 표를 후보 원천으로 쓴다.
struct MarketQuote
{
    double      price   = 0.0;   // 원, 장중 갱신
    double      val  = 0.0;   // 당일 누적 거래대금(원). 0=미제공
    double      volume  = 0.0;   // 당일 누적 거래량(주). 0=미제공
    std::string name;         // 시세 파일이 준 종목명. 비면 미제공
};
using QuoteTable = std::unordered_map<std::string, MarketQuote>;

// 후보 합집합 — 수집 축들이 공유하는 누적기이자 그대로 재사용 캐시의 몸통이다.
//  [why D-028] 랭킹·업종 축은 KIS REST 23콜(업종 20콜은 100ms 간격)이라 재스캔을 20초로
//  당기면 이 축만으로 초당 한도를 먹는다. 반면 정배열·이격·점수를 다시 매기는 데 필요한 건
//  일봉 캐시와 시세 표뿐이라 REST가 0이다. 그래서 "누가 후보인가"(비싼 축)와
//  "그 중 누가 좋은가"(싼 축)의 주기를 분리한다.
struct CandidateSet
{
    std::string date_yyyymmdd;
    std::time_t at = 0;
    std::vector<std::string> tickers;                       // 등록 순서 = 일봉 점검 우선순위
    std::unordered_map<std::string, std::string> names;
    std::unordered_map<std::string, std::string> market;    // 티커→"KOSPI"/"KOSDAQ"
    std::unordered_set<std::string> seen;
    bool have_market_map = false;
    int  etf_drop  = 0;
    int  reit_drop = 0;

    // 중복이면 false. 이름은 로그 라벨과 out_names에 쓴다.
    bool add(const std::string& ticker, const std::string& nm)
    {
        if (!seen.insert(ticker).second)
        {
            return false;
        }

        tickers.push_back(ticker);
        names[ticker] = nm;
        return true;
    }

    // [inv] 반환 뷰는 candidates 수명 안, names에 삽입이 없는 구간에서만 유효하다(재해시가 뷰를 끊는다).
    std::string_view name_of(const std::string& ticker) const
    {
        auto iterator = names.find(ticker);
        return iterator != names.end() ? std::string_view(iterator->second) : std::string_view{};
    }

    // market_map이 있는데도 사전에 없는 티커는 코스피·코스닥 보통주가 아니다(ETN 등).
    //  사전이 없는 구 파일에서는 태그 없는 후보를 KOSPI로 간주해 기존 동작을 유지한다.
    //  [inv] name_of와 같다 — 반환 뷰는 market에 삽입이 없는 구간에서만 유효하다.
    std::string_view market_of(const std::string& ticker) const
    {
        auto iterator = market.find(ticker);

        if (iterator != market.end() && !iterator->second.empty())
        {
            return iterator->second;
        }

        return have_market_map ? std::string_view("UNKNOWN") : std::string_view("KOSPI");
    }
};
CandidateSet g_candidate_cache;
std::mutex   g_candidate_mutex;

// 지수 게이트의 래치. 축(코스피·코스닥)마다 현재 차단 여부와 마지막 전환 시각을 들고 있는다.
//  재스캔 스레드가 유일한 호출자지만 g_pool과 같은 규약으로 뮤텍스를 둔다.
//  [inv] 프로세스 전역이라 슬리브 여럿이 같은 래치를 공유한다. 슬리브마다 임계가 다르면
//   먼저 발화한 쪽 판정이 나머지에도 걸린다 — 임계가 갈리는 순간 1회 경고한다. [why D-033]
struct IdxGateLatch
{
    bool off = false;                              // [inv] 현재 차단 상태(래치된 값)
    bool primed = false;                           // [inv] since가 유효한가 — 첫 전환 전에는 false
    std::chrono::steady_clock::time_point since{}; // 마지막 전환 시각
    double seen_trip = 0.0;                        // 직전 호출이 준 차단 임계(공유 감지용)
    double seen_resume = 0.0;                      // 직전 호출이 준 재개 임계
    bool cfg_warned = false;                       // 설정 경고를 이미 냈나(도배 방지)
};

IdxGateLatch g_kospi_latch;
IdxGateLatch g_kosdaq_latch;
IdxGateLatch g_itb_kospi_latch;   // ITB 슬리브 전용 — 임계가 DevScale과 달라 래치를 나눈다
std::mutex   g_idx_latch_mu;

// 히스테리시스 한 축. 등락률이 trip 아래로 내려가면 차단, resume 위로 올라오면 재개하고,
//  그 사이 중립대에서는 직전 상태를 유지한다. 차단 임계 하나로 20초마다 다시 재던 옛 판정은
//  지수가 경계를 오갈 때 게이트도 같이 떨었다(2026-08-21 최소 2분 53초 간격 토글).
//  observed=false는 조회 실패다 — 판정도 타이머도 건드리지 않는다. 반환은 "지금 차단인가".
//  [why D-033]
bool latch_risk_off(IdxGateLatch& latch, double chg, bool observed, double trip, double resume,
                    int dwell_sec, const char* label)
{
    // 관측 실패에는 판정하지 않는다. KisClient::get_index_price는 응답 파싱이 어긋나면
    //  (EGW00201 초당한도·HTTP 오류) 로그 없이 change_rate=0.0을 돌려준다. 0.0은 언제나
    //  "재개" 쪽으로만 틀리고, 그 오판이 래치를 풀면 체류가 그 상태를 dwell초 고정한다.
    //  지수 급락 구간은 초당한도가 가장 잘 터지는 구간이라 이 오판과 상관이 있다.
    if (!observed)
    {
        return latch.off;
    }

    // resume이 trip보다 낮으면 히스테리시스가 뒤집힌다 — 설정 실수는 옛 동작(단일 임계)으로 접는다.
    if (resume < trip)
    {
        if (!latch.cfg_warned)
        {
            LOG_WARN(std::string("[Universe] ") + label + " 지수 게이트 재개 임계가 차단 임계보다 낮다"
                     " — 히스테리시스를 끄고 단일 임계로 판정한다 (차단 " +
                     std::to_string(trip * 100.0) + "% / 재개 " + std::to_string(resume * 100.0) + "%)");
            latch.cfg_warned = true;
        }

        resume = trip;
    }
    else if (latch.primed && !latch.cfg_warned &&
             (latch.seen_trip != trip || latch.seen_resume != resume))
    {
        // 래치는 프로세스 전역이고 슬리브마다 config가 따로 온다. 임계가 갈리면 먼저 발화한 쪽
        //  판정이 나머지 슬리브에도 그대로 걸린다는 뜻이라 한 번 알린다.
        LOG_WARN(std::string("[Universe] ") + label + " 지수 게이트 임계가 슬리브마다 다르다"
                 " — 래치는 전역이라 먼저 발화한 판정이 공유된다");
        latch.cfg_warned = true;
    }

    latch.seen_trip   = trip;
    latch.seen_resume = resume;

    bool want = latch.off;

    if (chg < trip)
    {
        want = true;
    }
    else if (chg >= resume)
    {
        want = false;
    }

    if (want == latch.off)
    {
        return latch.off;
    }

    const auto now = std::chrono::steady_clock::now();

    // 체류는 재개 방향에만 건다. 08-21의 문제는 재개 쪽 떨림이었지 차단 지연이 아니었고,
    //  안전 게이트는 닫는 쪽이 언제나 즉시여야 한다.
    if (dwell_sec > 0 && latch.primed && !want)
    {
        const auto held =
            std::chrono::duration_cast<std::chrono::seconds>(now - latch.since).count();

        if (held < static_cast<long long>(dwell_sec))
        {
            return latch.off;   // 체류 미달 — 이번 재스캔은 직전 상태를 그대로 쓴다
        }
    }

    latch.off    = want;
    latch.primed = true;
    latch.since  = now;
    LOG_WARN(std::string("[Universe] ") + label + " 지수 게이트 " + (want ? "차단" : "재개") +
             " — 등락률 " + std::to_string(chg * 100.0) + "%, 차단 " +
             std::to_string(trip * 100.0) + "% / 재개 " + std::to_string(resume * 100.0) + "%");
    return latch.off;
}

// 시장별 risk_off 게이트(2026-08-19 회의). 코스피 급락은 전이 회피를 위해 코스피·코스닥
//  신규진입 모두에 영향을 준다(코스닥은 하루 늦게 따라오는 전이 지연이 잦다).
//  코스닥 종목은 이중 AND — 코스피 정상 AND 코스닥 정상일 때만 통과.
struct MarketGate
{
    double kospi_chg  = 0.0;
    double kosdaq_chg = 0.0;   // [inv] kosdaq_enabled=false면 미관측이라 0.0 — 표시에 쓰지 않는다
    bool   kospi_obs  = false; // [inv] 이번 조회가 성공했나. false면 chg는 의미 없다 [why D-033]
    bool   kosdaq_obs = false;
    bool   kospi_pass  = false;
    bool   kosdaq_pass = false;

    bool closed() const { return !kospi_pass && !kosdaq_pass; }

    // 시장 미상은 코스닥과 같은 보수 판정(닫혀 있으면 드롭).
    bool allows(std::string_view mk) const
    {
        if (mk == "KOSDAQ" || mk == "UNKNOWN")
        {
            return kosdaq_pass;
        }

        return kospi_pass;
    }
};

// 지수 등락률 조회 2콜. kosdaq_enabled=false면 코스닥 지수 조회조차 생략한다.
//  판정은 래치를 거친다(히스테리시스·최소 체류) — 시세 조회를 먼저 끝내고 락을 잡는다.
//  [lock-order] g_idx_latch_mu는 REST 호출 밖에서만 잡는다. g_pool_mu와 겹치지 않는다.
MarketGate build_market_gate(KisClient& kis, const DevScanCfg& config)
{
    MarketGate market_gate;
    // [wire] 조회가 어긋나면 KisClient가 로그 없이 IndexPrice{}를 돌려준다 — price>0이 관측
    //  성공의 유일한 표식이다. 지수 평보합도 price는 양수라 오탐이 없다.
    const auto kospi = kis.get_index_price("0001");
    market_gate.kospi_obs = kospi.price > 0.0;
    market_gate.kospi_chg = kospi.change_rate / 100.0;   // [wire] KIS는 % 단위

    if (config.kosdaq_enabled)
    {
        const auto kosdaq = kis.get_index_price("1001");   // [wire] 코스닥 종합지수
        market_gate.kosdaq_obs = kosdaq.price > 0.0;
        market_gate.kosdaq_chg = kosdaq.change_rate / 100.0;
    }

    std::lock_guard<std::mutex> lock(g_idx_latch_mu);
    const bool kospi_off = latch_risk_off(g_kospi_latch, market_gate.kospi_chg, market_gate.kospi_obs,
                                          config.risk_off_index, config.risk_off_idx_resume,
                                          config.risk_off_dwell_sec, "코스피");
    bool kosdaq_off = false;

    if (config.kosdaq_enabled)
    {
        kosdaq_off = latch_risk_off(g_kosdaq_latch, market_gate.kosdaq_chg, market_gate.kosdaq_obs,
                                    config.risk_off_idx_kosdaq, config.risk_off_idx_kosdaq_resume,
                                    config.risk_off_dwell_sec, "코스닥");
    }

    market_gate.kospi_pass  = !kospi_off;
    market_gate.kosdaq_pass = config.kosdaq_enabled && !kospi_off && !kosdaq_off;
    return market_gate;
}

// ETF/ETN·리츠 배제(개별주만). ETF는 브랜드 접두사(경계검사)∪상품 토큰, 리츠는 접미사·정확일치다.
//  KIS 축은 KisClient에서 이미 걸러지지만 data.go.kr 축과 한 규칙으로 이중 차단한다.
//  리츠를 따로 보는 이유는 배당·NAV로 움직여 일봉 프리필터를 그대로 통과하기 때문이다(334890 유입).
bool excluded_by_name(const std::string& nm, int& etf_drop, int& reit_drop)
{
    static const std::vector<std::string> kEtfPrefixes =
        etf_filter::load_list("etf_prefixes.json", etf_filter::default_prefixes());
    static const std::vector<std::string> kEtfTokens =
        etf_filter::load_list("etf_name_tokens.json", etf_filter::default_tokens());
    static const std::vector<std::string> kReitSuffixes =
        etf_filter::load_list("reit_name_suffixes.json", etf_filter::default_reit_suffixes());
    static const std::vector<std::string> kReitExacts =
        etf_filter::load_list("reit_names.json", etf_filter::default_reit_exacts());

    if (etf_filter::is_etf_like(nm, kEtfPrefixes, kEtfTokens))
    {
        ++etf_drop;
        return true;
    }

    if (etf_filter::is_reit_like(nm, kReitSuffixes, kReitExacts))
    {
        ++reit_drop;
        return true;
    }

    return false;
}

// 거래일 YYYYMMDD — KST 고정(머신 TZ 무관). 이름은 호출부와 맞춰 둔다.
std::string local_ymd()
{
    return kst::date_yyyymmdd(std::time(nullptr));
}

// 전 종목 장중 시세 파일. 네이버 벌크를 묶어오므로 KIS 초당 한도를 쓰지 않고 후보 전체의
//  현재가를 얻는다. 이게 있어야 정배열·이격을 매 재스캔마다 다시 판정한다.
//  실패는 경고만 내고 표를 비운 채 돌아간다 — 그러면 랭킹 축 스냅샷가만 쓰게 된다.
void load_quote_table(const DevScanCfg& config, QuoteTable& quotes)
{
    if (config.prices_file.empty())
    {
        return;
    }

    std::ifstream pf(config.prices_file);

    if (!pf)
    {
        LOG_WARN("[Main] 전 종목 시세 파일 없음(" + config.prices_file + ") — 랭킹 축 가격만 쓴다");
        return;
    }

    try
    {
        nlohmann::json pj;
        pf >> pj;
        // 필드 타입이 기대와 다르면 nlohmann은 예외를 던진다. 그대로 두면 바깥 catch로 빠져
        //  시세 파일 전체가 버려지는데, 결과가 "price가 전일 종가로 회귀"라 로그만 보면 파일 없음과
        //  구분되지 않는다. 항목 단위로 막아 어긋난 종목만 버린다.
        auto num = [](const nlohmann::json& document, const char* key) -> double
        {
            const auto found = document.find(key);
            return (found != document.end() && found->is_number()) ? found->get<double>() : 0.0;
        };
        const auto fp = pj.is_object() ? pj.find("prices") : pj.end();
        const nlohmann::json pm =
            (fp != pj.end() && fp->is_object()) ? *fp : nlohmann::json::object();
        int bad = 0;

        for (auto iterator = pm.begin(); iterator != pm.end(); ++iterator)
        {
            if (!iterator.value().is_object())
            {
                ++bad;
                continue;
            }

            const double price = num(iterator.value(), "px");

            if (price <= 0.0)
            {
                continue;
            }

            MarketQuote& mq = quotes[iterator.key()];
            mq.price  = price;
            mq.val = num(iterator.value(), "val");
            mq.volume = num(iterator.value(), "vol");
            const auto name_node = iterator.value().find("nm");

            if (name_node != iterator.value().end() && name_node->is_string())
            {
                mq.name = name_node->get<std::string>();
            }
        }

        const auto ft = pj.is_object() ? pj.find("ts") : pj.end();
        const std::time_t timestamp =
            (ft != pj.end() && ft->is_number()) ? static_cast<std::time_t>(ft->get<long long>()) : 0;
        const std::time_t age = std::time(nullptr) - timestamp;
        LOG_INFO("[Main] 전 종목 시세: " + std::to_string(quotes.size()) +
                 "종목 (" + std::to_string(static_cast<long long>(age)) + "초 전 갱신)");

        if (bad > 0)
        {
            LOG_WARN("[Main] 전 종목 시세 항목 " + std::to_string(bad) +
                     "건이 형식에 맞지 않아 건너뛴다");
        }

        // 이 파일이 멈추면 px_live가 전일 종가로 돌아가 정배열·이격 판정이 장 마감까지
        //  얼어붙는다. 재스캔은 돌지만 결과가 같아 구분이 안 된다.
        if (timestamp <= 0)
        {
            LOG_WARN("[Main] 전 종목 시세 파일에 갱신 시각(ts)이 없다 — 최신 여부 확인 불가");
        }
        else if (age > 600)
        {
            LOG_WARN("[Main] 전 종목 시세가 " + std::to_string(static_cast<long long>(age)) +
                     "초 지났다 — 보조 프로세스 확인 필요. 정배열 판정이 전일 종가로 고정된다");
        }
    }
    catch (const std::exception& exception)
    {
        LOG_WARN(std::string("[Main] 전 종목 시세 파일 파싱 실패: ") + exception.what());
    }
}

// 랭킹 응답을 후보 집합에 붓는다. 스냅샷가는 중복분에도 반영한다 — 표의 현재가를 최신으로 둔다.
void take_ranking(const std::vector<KisClient::RankingStock>& rank, const DevScanCfg& config,
                  QuoteTable& quotes, CandidateSet& candidates)
{
    for (const auto& ranked : rank)
    {
        if (ranked.price < config.min_price)
        {
            continue;
        }

        if (config.max_price > 0.0 && ranked.price > config.max_price)
        {
            continue;
        }

        if (excluded_by_name(ranked.name, candidates.etf_drop, candidates.reit_drop))
        {
            continue;
        }

        if (ranked.price > 0.0)
        {
            quotes[ranked.ticker].price = ranked.price;
        }

        candidates.add(ranked.ticker, ranked.name);
    }
}

// data.go.kr 시총∪거래대금 유니버스 피드. KIS 30행캡·ETF 잠식을 우회한 개별주 깊은 집합이라
//  후보 집합 맨 앞에 넣어 일봉 조회 우선순위를 준다. 파일이 없으면 조용히 스킵한다(하위호환).
//  전종목 코드→시장 사전(market_map)을 top-N보다 먼저 적재해, KIS 랭킹축 티커의 시장도
//  해석되게 한다 — 없으면 kosdaq_enabled 게이트가 그쪽으로 샌다.
void take_universe_file(const DevScanCfg& config, CandidateSet& candidates)
{
    if (config.universe_file.empty())
    {
        return;
    }

    std::ifstream file(config.universe_file);

    if (!file)
    {
        LOG_WARN("[Main] DEVSCALE 유니버스 파일 없음(" + config.universe_file +
                 ") — data.go.kr 축 스킵, KIS 랭킹 축만 사용");
        return;
    }

    try
    {
        nlohmann::json document;
        file >> document;
        const std::string basDt = document.value("basDt", std::string());

        if (document.contains("market_map") && document["market_map"].is_object())
        {
            for (auto iterator = document["market_map"].begin(); iterator != document["market_map"].end(); ++iterator)
            {
                if (iterator.value().is_string())
                {
                    candidates.market[iterator.key()] = iterator.value().get<std::string>();
                }
            }

            candidates.have_market_map = !candidates.market.empty();
        }

        const nlohmann::json& array = jsonx::array_or_empty(document, "universe");
        int added_file = 0, dup = 0;

        for (const auto& element : array)
        {
            const std::string ticker = element.value("ticker", std::string());

            if (ticker.empty())
            {
                continue;
            }

            const std::string nm = element.value("name", std::string());

            if (excluded_by_name(nm, candidates.etf_drop, candidates.reit_drop))
            {
                continue;
            }

            const double price = element.value("close", 0.0);

            // close(0=미제공)면 가격필터는 뒤 정배열 프리필터의 일봉이 대신 검증한다.
            if (price > 0.0 && price < config.min_price)
            {
                continue;
            }

            if (config.max_price > 0.0 && price > config.max_price)
            {
                continue;
            }

            if (!candidates.add(ticker, nm))
            {
                ++dup;
                continue;
            }

            candidates.market[ticker] = element.value("market", std::string());   // 시장별 risk_off 게이트용
            ++added_file;
        }

        LOG_INFO("[Main] DEVSCALE data.go.kr 축(기준일 " + basDt + "): 파일 " +
                 std::to_string(array.size()) + "종목 → 신규 " + std::to_string(added_file) +
                 " union (중복 " + std::to_string(dup) + ")");
    }
    catch (const std::exception& ex)
    {
        LOG_WARN("[Main] DEVSCALE 유니버스 파일 파싱 실패(" + config.universe_file +
                 "): " + std::string(ex.what()) + " — data.go.kr 축 스킵");
    }
}

// 업종 등락률 축 — 다른 축과 data.go.kr 축이 전부 전일 이전 상태를 보는 것과 달리 이 축만
//  장중을 본다. 등락률 내림차순이라 상위 N행이 곧 지금 강한 종목이고, 업종을 순회하므로
//  한 섹터가 집합을 독식하지 않는다. 정배열·과확장 판정은 뒤 프리필터가 그대로 한다.
void take_sector_ranking(KisClient& kis, const DevScanCfg& config, QuoteTable& quotes, CandidateSet& candidates)
{
    if (config.sector_codes.empty())
    {
        return;
    }

    const std::size_t before = candidates.tickers.size();
    int sec_ok = 0, sec_weak = 0;

    for (const auto& sc : config.sector_codes)
    {
        auto rows = kis.fetch_sector_ranking(sc, config.sector_top_n);
        // 26콜을 쉬지 않고 내면 8.8콜/s로 나가 문서상 한도 20/s의 절반을 이 축 하나가
        //  버스트로 먹는다(09-08: ranking/fluctuation HTTP 500 70건). 재스캔 주기가
        //  600초라 2.6초에서 5.2초로 늘어나는 지연은 무시할 만하다.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        if (rows.empty())
        {
            continue;
        }

        ++sec_ok;
        std::vector<KisClient::RankingStock> strong;
        strong.reserve(rows.size());

        for (const auto& row : rows)
        {
            if (row.change_rate < config.sector_min_chg)
            {
                ++sec_weak;
                continue;
            }

            strong.push_back(row);
        }

        take_ranking(strong, config, quotes, candidates);
    }

    LOG_INFO("[Main] DEVSCALE 업종 등락률 축: " + std::to_string(sec_ok) + "/" +
             std::to_string(config.sector_codes.size()) + "업종 응답, 신규 " +
             std::to_string(candidates.tickers.size() - before) + "종목 union (약세컷 " +
             std::to_string(sec_weak) + ")");
}

// 전 종목 확장 — market_map(코스피+코스닥 전체)을 후보로 붓는다. 종목명은 시세 표에서
//  가져와 ETF·리츠 필터를 그대로 적용한다(이름이 없으면 버린다).
//  티커를 정렬해 순회한다. 해시맵 순서로 돌면 같은 입력에서도 후보 순서가 실행마다 달라지고,
//  align_probe_max로 잘리는 지점이 함께 바뀌어 유니버스가 재현되지 않는다.
void take_full_market(const DevScanCfg& config, const QuoteTable& quotes, CandidateSet& candidates)
{
    if (!config.full_market || !candidates.have_market_map)
    {
        return;
    }

    const std::size_t before_fm = candidates.tickers.size();
    int no_name = 0;
    std::vector<std::string> tickers;
    tickers.reserve(candidates.market.size());

    for (const auto& entry : candidates.market)
    {
        if (entry.first.size() == 6)
        {
            tickers.push_back(entry.first);
        }
    }

    std::sort(tickers.begin(), tickers.end());

    for (const auto& ticker : tickers)
    {
        auto itq = quotes.find(ticker);

        if (itq == quotes.end() || itq->second.name.empty())
        {
            ++no_name;
            continue;
        }

        if (excluded_by_name(itq->second.name, candidates.etf_drop, candidates.reit_drop))
        {
            continue;
        }

        if (itq->second.price < config.min_price)
        {
            continue;
        }

        if (config.max_price > 0.0 && itq->second.price > config.max_price)
        {
            continue;
        }

        candidates.add(ticker, itq->second.name);
    }

    LOG_INFO("[Main] 전 종목 확장: 신규 " + std::to_string(candidates.tickers.size() - before_fm) +
             "종목 union (시세없음 " + std::to_string(no_name) + ", 총 후보 " +
             std::to_string(candidates.tickers.size()) + ")");
}

// 후보 합집합을 채운다. union_refresh_sec 안에 다시 불리면 수집을 통째로 건너뛰고
//  지난 집합을 그대로 쓴다 — 이 단계만 KIS REST 23콜이고 이후 재판정은 0콜이다(D-028).
//  0이면 매 호출 새로 모은다(기존 동작).
void collect_candidates(KisClient& kis, const DevScanCfg& config, const std::string& date_yyyymmdd,
                        QuoteTable& quotes, CandidateSet& candidates)
{
    if (config.union_refresh_sec > 0)
    {
        long long age = -1;
        {
            std::lock_guard<std::mutex> lock(g_candidate_mutex);

            if (g_candidate_cache.date_yyyymmdd == date_yyyymmdd && !g_candidate_cache.tickers.empty() &&
                std::time(nullptr) - g_candidate_cache.at < config.union_refresh_sec)
            {
                candidates = g_candidate_cache;
                age  = static_cast<long long>(std::time(nullptr) - g_candidate_cache.at);
            }
        }

        if (age >= 0)
        {
            // 현재가·거래대금은 시세 표에서 방금 읽은 값을 쓴다. 랭킹 축이 실어오던
            //  스냅샷가는 재사용분에 없지만, 네이버 쪽이 더 최신이라 판정에는 그편이 낫다.
            LOG_INFO("[Main] DEVSCALE 후보 합집합 재사용: " + std::to_string(candidates.tickers.size()) +
                     "종목 (" + std::to_string(age) +
                     "초 전 수집, 갱신주기 " + std::to_string(config.union_refresh_sec) + "초)");
            return;
        }
    }

    // 정배열 프리필터로 상당수가 탈락하므로 여기선 max_register로 자르지 않고 넓게 모은다.
    take_universe_file(config, candidates);
    take_ranking(kis.fetch_kr_ranking(config.scan_top_n, "J"), config, quotes, candidates);            // 시총 상위
    take_ranking(kis.fetch_value_ranking(config.value_top_n, "J", "3"), config, quotes, candidates);   // 거래대금 상위
    // 랭킹 TR은 축마다 상위 30행 고정(연속조회 불가)이라 정렬축을 하나 더 union해 집합을 넓힌다.
    //  거래증가율(1)은 대형주에 편중된 시총·거래대금축과 겹침이 적어(중소형 모멘텀) 정배열 후보를 늘린다.
    take_ranking(kis.fetch_value_ranking(config.value_top_n, "J", "1"), config, quotes, candidates);   // 거래증가율 상위
    take_sector_ranking(kis, config, quotes, candidates);
    take_full_market(config, quotes, candidates);

    if (config.union_refresh_sec > 0)
    {
        std::lock_guard<std::mutex> lock(g_candidate_mutex);
        candidates.date_yyyymmdd = date_yyyymmdd;
        candidates.at  = std::time(nullptr);
        g_candidate_cache   = candidates;
    }
}

// 후보 하나의 일봉을 받아 SMA·롤오프·ATR로 요약한다. 60봉 미만이면 bars만 채워 돌려준다.
//  조회 간격은 호출자가 책임진다.
DailyProbe fetch_probe(KisClient& kis, const DevScanCfg& config, const std::string& ticker,
                       const std::string& date_yyyymmdd)
{
    auto daily_ohlcv = kis.get_daily_ohlcv(ticker, config.align_daily_n);
    DailyProbe pr;
    pr.date_yyyymmdd  = date_yyyymmdd;
    pr.at   = std::time(nullptr);
    pr.bars = static_cast<int>(daily_ohlcv.size());

    if (pr.bars < 60)
    {
        return pr;
    }

    auto sma = [&](int count) { double sum = 0.0; for (int index = 0; index < count; ++index) sum += daily_ohlcv[index].close; return sum / count; };
    pr.s5 = sma(5); pr.s10 = sma(10); pr.s20 = sma(20); pr.s60 = sma(60);
    pr.r5 = daily_ohlcv[4].close; pr.r10 = daily_ohlcv[9].close;
    pr.r20 = daily_ohlcv[19].close; pr.r60 = daily_ohlcv[59].close;
    pr.close = daily_ohlcv[0].close;
    // [formula] ATR(14) — True Range = max(고−저, |고−전일종가|, |저−전일종가|)의 14봉 평균.
    //  d[0]이 최신이므로 d[i+1]이 i의 전일. 종가로 나눠 종목 간 비교 가능한 비율로 만든다.
    double tr_sum = 0.0;
    int    tr_n   = 0;

    for (size_t index = 0; index + 1 < daily_ohlcv.size() && tr_n < 14; ++index, ++tr_n)
    {
        const double prev_c = daily_ohlcv[index + 1].close;
        const double high = daily_ohlcv[index].high, low = daily_ohlcv[index].low;
        double tr = high - low;
        const double high_gap = std::fabs(high - prev_c), low_gap = std::fabs(low - prev_c);

        if (high_gap > tr)
        {
            tr = high_gap;
        }

        if (low_gap > tr)
        {
            tr = low_gap;
        }

        tr_sum += tr;
    }

    pr.atr_pct = (tr_n > 0 && pr.close > 0.0) ? (tr_sum / tr_n) / pr.close : 0.0;

    // 저항·거래량 축. 일봉은 전일까지(include_today=false)라 d[0]이 전일이다.
    for (const auto& bar : daily_ohlcv)
    {
        if (bar.high > pr.hi250)
        {
            pr.hi250 = bar.high;
        }
    }

    // [formula] 스윙 고점 = 좌우 5봉의 고가보다 모두 높은 봉. 가장 최근 것 하나만 쓴다.
    for (size_t index = 5; index + 5 < daily_ohlcv.size(); ++index)
    {
        bool peak = true;

        for (size_t innermost_index = 1; innermost_index <= 5 && peak; ++innermost_index)
        {
            peak = daily_ohlcv[index].high > daily_ohlcv[index - innermost_index].high && daily_ohlcv[index].high > daily_ohlcv[index + innermost_index].high;
        }

        if (peak)
        {
            pr.pivot_hi = daily_ohlcv[index].high;
            break;
        }
    }

    double volume_sum = 0.0;

    for (int index = 0; index < 20; ++index)
    {
        volume_sum += static_cast<double>(daily_ohlcv[index].volume);
    }

    pr.avg_vol20 = volume_sum / 20.0;
    pr.close21   = daily_ohlcv.size() > 21 ? daily_ohlcv[21].close : 0.0;
    return pr;
}

//  점수는 원자료를 바로 더하지 않는다. 추세·눌림·변동성은 단위도 일별 분산도 달라서 그대로
//   더하면 그날 우연히 많이 벌어진 축이 점수를 지배한다. 통과 집합 안에서 각각 z-score로
//   정규화하고 ±2σ에서 자른 뒤 가중합한다(스케일-프리 + 이상치 1종목 지배 차단).
struct Feat
{
    std::string ticker;
    double trend, pull, volume, turnover, score;
};

struct ProbeStats
{
    int probed = 0, aligned = 0, short_bars = 0, overext = 0;
    int fetched = 0, cache_hit = 0, refreshed = 0;
    int illiquid = 0;         // 거래대금 하한 미달로 버린 수
    int misaligned = 0;       // 정배열 조건 미충족으로 버린 수(진단용)
    int budget_skipped = 0;   // 일봉 조회 예산이 끝났고 캐시도 없어 판정 못 한 수
};

// 2단: 정배열 프리필터 — 후보를 일봉으로 검사해 정배열=Y(≥60봉)만 통과시킨다.
//  데이터부족(신규상장 <60봉)은 여기서 자동 제외된다. 일봉 조회 비용은 align_probe_max로
//  캡하되 캐시 히트는 예산을 쓰지 않는다. 정배열 규칙은 MaAlign.h의 quant::ma::aligned 하나를 전략과 같이 쓴다.
std::vector<Feat> probe_and_filter(KisClient& kis, const DevScanCfg& config, const std::string& date_yyyymmdd,
                                   const CandidateSet& candidates, const QuoteTable& quotes,
                                   const MarketGate& gate, ProbeStats& stats)
{
    std::vector<Feat> passed;
    std::unordered_set<std::string> refresh_set;

    if (config.align_refresh_max > 0)
    {
        std::size_t stale_n = 0;
        refresh_set = g_probe_cache.stale_targets(candidates.tickers, date_yyyymmdd,
                                                  static_cast<std::time_t>(config.align_refresh_sec),
                                                  config.align_refresh_max, stale_n);

        if (stale_n > 0)
        {
            LOG_INFO("[Main] DEVSCALE 일봉 재조회: 대상 " + std::to_string(stale_n) +
                     "종목 중 " + std::to_string(refresh_set.size()) + "건 (예산 " +
                     std::to_string(config.align_refresh_max) + ", 재조회 기준 " +
                     std::to_string(config.align_refresh_sec) + "초)");
        }
    }

    for (const auto& ticker : candidates.tickers)
    {
        if (!gate.allows(candidates.market_of(ticker)))
        {
            continue;   // 시장 risk_off 게이트. 일봉 조회 비용도 여기서 아낀다
        }

        // 유동성 하한 — 거래대금이 받침하지 못하는 종목은 체결이 안 되거나 슬리피지로 손익을
        //  먹는다. 거래대금을 모르는 후보는 통과시킨다(기존 동작 유지).
        if (config.min_turnover > 0.0)
        {
            auto itv = quotes.find(ticker);

            if (itv != quotes.end() && itv->second.val > 0.0 && itv->second.val < config.min_turnover)
            {
                ++stats.illiquid;
                continue;
            }
        }

        // 스코어링 시엔 max_register 대신 align_probe_max까지 넓게 모아 랭킹한다(더 나은 상위 N).
        if (config.score_top_n <= 0 && static_cast<int>(passed.size()) >= config.max_register)
        {
            break;
        }

        DailyProbe pr;
        bool have = g_probe_cache.get(ticker, date_yyyymmdd, pr);
        const bool refresh_me = refresh_set.find(ticker) != refresh_set.end();   // 대상은 전부 오늘치가 있다

        if (!have || refresh_me)
        {
            if (stats.fetched >= config.align_probe_max)
            {
                // 예산은 REST에만 건다. 예전에는 여기서 루프를 끊어 뒤쪽 후보의 공짜 캐시
                //  히트까지 같이 버렸고, 그래서 후보 집합을 넓힐수록 뒤쪽이 영구히 미검사로 남았다.
                if (!have)
                {
                    ++stats.budget_skipped;
                    continue;
                }

                ++stats.cache_hit;
            }
            else
            {
                if (refresh_me)
                {
                    ++stats.refreshed;
                }

                // 하루 첫 스캔은 수백 건이 연속으로 나간다. 60ms에서는 초당한도(EGW00201) 거부가
                //  09-08 하루 149건 났고 CANCEL뿐 아니라 NEW에도 걸려 진입이 4초씩 밀렸다.
                //  같은 날 주문 RTT p50이 09시 381ms에서 10시 1870ms로 단조증가한 것도 계좌 단위
                //  REST 누적 부하로 보여 150ms로 올린다. 캐시 히트 경로에는 걸리지 않는다.
                if (stats.fetched > 0)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(150));
                }

                pr = fetch_probe(kis, config, ticker, date_yyyymmdd);
                ++stats.fetched;
                g_probe_cache.put(ticker, pr);
            }
        }
        else
        {
            ++stats.cache_hit;
        }

        ++stats.probed;

        if (pr.bars < 60)
        {
            ++stats.short_bars;
            continue;
        }

        // 오늘 가격을 최신 봉으로 접어 넣어 SMA를 다시 계산한다. 일봉 캐시는 include_today=false라
        //  전일치에서 멈춰 있고, 그대로 쓰면 정배열 판정이 하루 종일 얼어붙어 재스캔이 같은 종목만
        //  돌려준다. 시세 표의 현재가를 쓰므로 REST 추가 없이 매 재스캔마다 다시 판정한다.
        double price = pr.close;
        double turnover = 0.0;
        {
            auto itp = quotes.find(ticker);

            if (itp != quotes.end())
            {
                if (itp->second.price > 0.0)
                {
                    price = itp->second.price;
                }

                turnover = itp->second.val;
            }
        }

        quant::ma::Smas previous;
        previous.s5 = pr.s5; previous.s10 = pr.s10; previous.s20 = pr.s20; previous.s60 = pr.s60;
        const quant::ma::Smas ma =
            quant::ma::fold_today(previous, pr.r5, pr.r10, pr.r20, pr.r60, price);
        const double s5 = ma.s5, s10 = ma.s10, s20 = ma.s20, s60 = ma.s60;

        if (!quant::ma::aligned(ma, config.align_ma_tol_pct))
        {
            ++stats.misaligned;
            continue;
        }

        double trend = s60 > 0.0 ? (s5 - s60) / s60 : 0.0;   // 추세강도(정배열 기울기)
        double pull  = s20 > 0.0 ? (price - s20) / s20 : 0.0;   // 눌림깊이(음수=SMA20 아래)

        // 과확장 컷 — 이격 상한 초과는 존 밴드 진입이 불가한 폭등주라 슬롯만 낭비한다.
        if (config.max_dev_pct > 0.0 && pull > config.max_dev_pct)
        {
            ++stats.overext;
            continue;
        }

        // 과확장 하한 — 밴드 아래(덜 벌어진 종목)는 눌림 슬리브 몫이다.
        if (config.min_dev_pct > 0.0 && pull < config.min_dev_pct)
        {
            ++stats.overext;
            continue;
        }

        passed.push_back({ticker, trend, pull, pr.atr_pct, turnover, 0.0});
        ++stats.aligned;
    }

    if (stats.budget_skipped > 0)
    {
        LOG_WARN("[Main] DEVSCALE 정배열 프리필터: 일봉 조회 상한(" +
                 std::to_string(config.align_probe_max) + ") 도달 — 캐시 없는 후보 " +
                 std::to_string(stats.budget_skipped) + "건은 다음 재스캔에서 채움");
    }

    return passed;
}

// 2.5단: 횡단면 정규화로 종합 점수 하나를 만든다. 이 점수가 등록 순서(=진입 우선순위)와
//  종목별 비중 배수 두 가지를 모두 정한다.
//  [formula] S = w_trend·z(추세) + w_pull·z(-눌림) - w_vol·z(변동성) + w_liq·z(log 거래대금).
//   변동성은 뺀다 — 추세·눌림이 같다면 덜 흔들리는 쪽이 낫다.
//   거래대금은 더한다 — 같은 조건이면 두꺼운 쪽이 청산 슬리피지가 작다. 기본값 0(비활성)이다.
void score_cross_section(const DevScanCfg& config, std::vector<Feat>& passed)
{
    auto zscore = [&](double Feat::*field, bool invert, std::vector<double>& values)
    {
        const size_t count = passed.size();
        values.assign(count, 0.0);

        if (count < 2)
        {
            return;
        }

        double mean = 0.0;

        for (const auto& passed_entry : passed)
        {
            mean += passed_entry.*field;
        }

        mean /= static_cast<double>(count);
        double var = 0.0;

        for (const auto& passed_entry : passed) { const double d0 = passed_entry.*field - mean; var += d0 * d0; }
        var /= static_cast<double>(count);
        const double sd = std::sqrt(var);

        // 분산이 사실상 0이면(전 종목 동일) 정규화가 무의미하다. 전부 0으로 두어 균등 폴백.
        if (!(sd > 1e-12))
        {
            return;
        }

        for (size_t index = 0; index < count; ++index)
        {
            double value = (passed[index].*field - mean) / sd;

            if (value > 2.0)
            {
                value = 2.0;
            }

            if (value < -2.0)
            {
                value = -2.0;
            }

            values[index] = invert ? -value : value;
        }
    };
    std::vector<double> zt, zp, zv, zl;
    zscore(&Feat::trend, false, zt);
    zscore(&Feat::pull,  true,  zp);   // 눌림은 음수(SMA20 아래)일수록 좋아 부호를 뒤집는다. 추세확장 슬리브(min_dev_pct>0)에선 전부 양수라 "덜 벌어진 쪽 우대"(과확장 감점)로 작동한다
    zscore(&Feat::volume,   false, zv);

    if (config.score_w_liquidity != 0.0)
    {
        // 거래대금은 자릿수 분포라 로그를 취해 z를 낸다. 원값 그대로면 대형주 한둘이 표준편차를
        //  다 먹어 나머지가 한 점에 뭉친다.
        std::vector<double> known;

        for (const auto& passed_entry : passed)
        {
            if (passed_entry.turnover > 0.0)
            {
                known.push_back(std::log(passed_entry.turnover));
            }
        }

        // 시세 파일이 거래대금을 안 준 종목은 중앙값으로 받쳐 중립(z≈0)에 둔다. 0을 그대로
        //  로그로 넘기면 데이터 결측이 최하위 점수로 둔갑한다.
        double fill = 0.0;

        if (!known.empty())
        {
            std::sort(known.begin(), known.end());
            fill = known[known.size() / 2];
        }

        for (auto& passed_entry : passed)
        {
            passed_entry.turnover = passed_entry.turnover > 0.0 ? std::log(passed_entry.turnover) : fill;
        }

        zscore(&Feat::turnover, false, zl);
    }
    else
    {
        zl.assign(passed.size(), 0.0);
    }

    for (size_t passed_index = 0; passed_index < passed.size(); ++passed_index)
    {
        passed[passed_index].score = config.score_w_trend * zt[passed_index] + config.score_w_pullback * zp[passed_index]
                        - config.score_w_vol * zv[passed_index] + config.score_w_liquidity * zl[passed_index];
    }
}

// 3단: 점수 내림차순으로 등록한다. score_top_n>0이면 상위 N만 남긴다.
//  절단이 없어도 정렬은 한다 — 등록 순서가 그대로 진입 우선순위라, 안 정렬하면
//  유니버스 파일 순서(시총·거래대금)가 우선순위를 먹는다.
std::vector<std::string> rank_and_truncate(const DevScanCfg& config, std::vector<Feat>& passed,
                                           const CandidateSet& candidates,
                                           std::unordered_map<std::string, std::string>* out_names,
                                           std::unordered_map<std::string, double>* out_scores)
{
    std::ranges::sort(passed, std::ranges::greater{}, &Feat::score);
    std::size_t take_n = passed.size();

    if (config.score_top_n > 0 && static_cast<std::size_t>(config.score_top_n) < take_n)
    {
        take_n = static_cast<std::size_t>(config.score_top_n);
    }

    std::vector<std::string> out;
    out.reserve(take_n);

    for (std::size_t take_index = 0; take_index < take_n; ++take_index)
    {
        out.push_back(passed[take_index].ticker);

        if (out_names)
        {
            (*out_names)[passed[take_index].ticker] = candidates.name_of(passed[take_index].ticker);
        }

        if (out_scores)
        {
            (*out_scores)[passed[take_index].ticker] = passed[take_index].score;
        }
    }

    if (config.score_top_n > 0)
    {
        LOG_INFO("[Main] DEVSCALE 횡단면 스코어: 정배열통과=" + std::to_string(passed.size()) +
                 " → 상위 " + std::to_string(take_n) + " 선정 (w_trend=" +
                 std::to_string(config.score_w_trend) + " w_pull=" + std::to_string(config.score_w_pullback) +
                 " w_liq=" + std::to_string(config.score_w_liquidity) +
                 " w_supply=" + std::to_string(config.score_w_supply) + "(미적용) w_vol=" + std::to_string(config.score_w_vol) + ")");
    }

    return out;
}

// 프리필터 off — 기존 동작(후보 앞에서부터 max_register개).
std::vector<std::string> take_first_n(const DevScanCfg& config, const CandidateSet& candidates,
                                      const MarketGate& gate,
                                      std::unordered_map<std::string, std::string>* out_names)
{
    std::vector<std::string> out;

    for (const auto& ticker : candidates.tickers)
    {
        if (!gate.allows(candidates.market_of(ticker)))
        {
            continue;
        }

        if (static_cast<int>(out.size()) >= config.max_register)
        {
            break;
        }

        out.push_back(ticker);

        if (out_names)
        {
            (*out_names)[ticker] = candidates.name_of(ticker);
        }
    }

    return out;
}
} // namespace



std::vector<ItbCandidate> scan_itb(KisClient& scan_kis, const ItbScanCfg& config)
{
    std::vector<ItbCandidate> out;

    // 레짐 게이트: 코스피(0001) 당일 등락률이 risk_off 이하면 신규매수 유니버스 전면 스킵.
    //  판정은 DevScale과 같은 래치(latch_risk_off)로 하되 래치 객체는 따로 둔다 — 임계가
    //  다른 슬리브가 한 래치를 나눠 쓰면 먼저 발화한 쪽 판정이 다른 쪽에 걸린다. 지금은 기동 시
    //  1회 호출이라 체류·재개가 작동할 일이 없고, 재스캔 잡이 붙는 날 그대로 살아난다. [why D-033]
    auto kospi = scan_kis.get_index_price("0001");
    double idx_chg = kospi.change_rate / 100.0; // KIS는 % 단위

    // [wire] 조회가 어긋나면 KisClient가 로그 없이 IndexPrice{}를 준다 — price>0이 관측
    //  성공의 유일한 표식이다. 0.0을 그대로 믿으면 게이트가 언제나 "통과"로 틀리는데,
    //  급락장 재기동은 EGW00201(초당 한도)이 가장 잘 터지는 조합이라 그 오판이
    //  "코스피 −3%인데 신규매수 유니버스 전면 등록"이 된다. 1회성 게이트라 닫는 쪽이 싸다.
    if (kospi.price <= 0.0)
    {
        LOG_WARN("[Main] universe_from_scan: 코스피 지수 조회 실패 — 신규매수 유니버스 미등록");
        return out;
    }

    bool risk_off = false;
    {
        std::lock_guard<std::mutex> lock(g_idx_latch_mu);
        risk_off = latch_risk_off(g_itb_kospi_latch, idx_chg, /*observed=*/true, config.risk_off_index,
                                  config.risk_off_idx_resume, config.risk_off_dwell_sec, "코스피(ITB)");
    }

    if (risk_off)
    {
        LOG_WARN("[Main] universe_from_scan: 레짐 위험회피(코스피 " +
                 std::to_string(kospi.change_rate) + "%, 차단 " +
                 std::to_string(config.risk_off_index * 100.0) + "% / 재개 " +
                 std::to_string(config.risk_off_idx_resume * 100.0) + "%) — 신규매수 유니버스 미등록");
        return out;
    }

    auto rank = scan_kis.fetch_value_ranking(config.scan_top_n, "J");
    int added = 0;

    for (const auto& ranked : rank)
    {
        if (added >= config.max_register)
        {
            break;
        }

        double chg = ranked.change_rate / 100.0; // % → 비율

        // 필터①: 등락률 밴드(강세 모멘텀, 급등 추격 배제)
        if (chg < config.chg_min || chg > config.chg_max)
        {
            continue;
        }

        // 필터②: 최소가(동전주·호가스프레드 배제)
        if (ranked.price < config.min_price)
        {
            continue;
        }

        // 필터③: 수급(option) — 외국인 T-1 확정 순매수 > 0 (후보 소수에만 조회)
        if (config.sd_filter)
        {
            auto tr = scan_kis.get_investor_trend(ranked.ticker);

            if (tr.foreign_net <= 0)
            {
                LOG_INFO("[Main]   - ITB 스캔 제외 " + ranked.ticker + " 외국인순매수<=0");
                continue;
            }
        }

        // 통과 → 신규 진입 유니버스로 등록(당일 시가 앵커 주입).
        //  ⚠️ 앵커는 랭킹 스냅샷 현재가(r.price)가 아니라 실제 당일 시가여야 함.
        //  갭업일엔 스냅샷=장중 고점 근처라 앵커가 고점에 고정되어 돌파 진입이 영구 차단됨.
        //  inquire-price(FHKST01010100)의 stck_oprc로 진짜 시가를 조회, 0이면 r.price 폴백.
        double day_open = scan_kis.get_fundamentals(ranked.ticker).open;

        if (day_open <= 0.0)
        {
            day_open = ranked.price;
        }

        out.push_back({ranked.ticker, ranked.name, day_open});
        LOG_INFO("[Main]   + ITB 스캔 " + ranked.ticker + " " + ranked.name + " (등락 " +
                 std::to_string(ranked.change_rate) + "% 가격 " +
                 std::to_string(static_cast<long long>(ranked.price)) + " 시가앵커 " +
                 std::to_string(static_cast<long long>(day_open)) + " 거래대금 " +
                 std::to_string(static_cast<long long>(ranked.trade_value)) + ")");
        ++added;
    }

    LOG_INFO("[Main] universe_from_scan: 후보 " + std::to_string(rank.size()) +
             "종목 중 " + std::to_string(added) + "종목 등록");
    return out;
}

// DeviationScale 유니버스 선정. 단계는 넷이고 비용이 다르다 — 후보 합집합 수집만 KIS REST를
//  쓰고(D-028로 주기 분리), 정배열·이격·점수 재판정은 일봉 캐시와 시세 표만 본다.
//  스캔 스레드에서만 부른다. 실패는 예외 대신 빈 목록으로 돌려준다.
std::vector<std::string> scan_devscale(KisClient& kis, const DevScanCfg& config,
                                       std::unordered_map<std::string, std::string>* out_mapNames,
                                       std::unordered_map<std::string, double>* out_mapScores)
{
    const std::string date_yyyymmdd = local_ymd();   // 일봉 캐시·후보 집합 캐시의 거래일 키
    g_probe_cache.load_today(date_yyyymmdd);         // 장중 재기동 시 일봉 재조회를 막는다

    QuoteTable quotes;
    load_quote_table(config, quotes);

    const MarketGate gate = build_market_gate(kis, config);

    if (gate.closed())
    {
        // 모든 시장이 위험회피 상태다. 후보 수집·일봉 조회를 전부 생략한다.
        LOG_WARN("[Main] DEVSCALE 스캔: 레짐 위험회피(코스피 " + std::to_string(gate.kospi_chg * 100.0) +
                 "%" + (config.kosdaq_enabled ? ", 코스닥 " + std::to_string(gate.kosdaq_chg * 100.0) + "%" : "") +
                 ") — 신규 유니버스 스킵");
        return {};
    }

    CandidateSet candidates;
    collect_candidates(kis, config, date_yyyymmdd, quotes, candidates);

    if (!config.require_aligned)
    {
        return take_first_n(config, candidates, gate, out_mapNames);
    }

    ProbeStats stats;
    std::vector<Feat> passed = probe_and_filter(kis, config, date_yyyymmdd, candidates, quotes, gate, stats);
    score_cross_section(config, passed);
    std::vector<std::string> out = rank_and_truncate(config, passed, candidates, out_mapNames, out_mapScores);

    // 새로 받은 일봉이 있을 때만 파일을 갱신한다. 히트만 났으면 내용이 같다.
    if (stats.fetched > 0)
    {
        g_probe_cache.save_today(date_yyyymmdd);
    }

    LOG_INFO("[Main] DEVSCALE 정배열 프리필터: 후보=" + std::to_string(candidates.tickers.size()) +
             " ETF드롭=" + std::to_string(candidates.etf_drop) +
             " 리츠드롭=" + std::to_string(candidates.reit_drop) +
             " 검사=" + std::to_string(stats.probed) +
             " (일봉조회=" + std::to_string(stats.fetched) +
             " 재조회=" + std::to_string(stats.refreshed) +
             " 캐시=" + std::to_string(stats.cache_hit) + ")" +
             " 정배열=" + std::to_string(stats.aligned) +
             " 역배열컷=" + std::to_string(stats.misaligned) +
             " 데이터부족(<60봉)=" + std::to_string(stats.short_bars) +
             " 과확장컷=" + std::to_string(stats.overext) +
             " 거래대금미달=" + std::to_string(stats.illiquid) +
             " 예산소진=" + std::to_string(stats.budget_skipped) +
             " 등록=" + std::to_string(out.size()));
    return out;
}
} // namespace universe
