#include "universe/UniverseScanner.h"
#include "core/Types.h"
#include "utils/EtfFilter.h"
#include "utils/Logger.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
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
//  끊기면 px가 전일 종가로 돌아가 판정이 정말로 얼어붙는다 — 나이를 경고로 내보낸다.
//  캐시가 없던 때는 재스캔마다 후보 전체의 일봉을 다시 받았고, 그 비용이
//  rescan_interval에 반비례해 후보 풀을 넓히는 것 자체가 막혔다(align_probe_max가 그 캡).
//  ATR·봉수처럼 확정봉만 쓰는 값은 그대로 하루 고정이다.
struct DailyProbe
{
    std::string ymd;                       // 조회 시각의 로컬 날짜(YYYYMMDD)
    int    bars  = 0;                      // 확보 봉수(<60이면 판정 불가)
    double s5 = 0.0, s10 = 0.0, s20 = 0.0, s60 = 0.0;
    double close = 0.0;                    // 최신 종가(d[0])
    // SMA에 오늘 가격을 접어 넣을 때 빠지는 봉의 종가.
    //  s_n_live = (s_n*n - roll_n + px_live) / n — REST 없이 정배열을 장중 갱신한다.
    double r5 = 0.0, r10 = 0.0, r20 = 0.0, r60 = 0.0;
    double atr_pct = 0.0;                  // ATR(14)/종가 — 변동성. 정배열 판정용 일봉 재활용(추가 REST 0)
    std::time_t at = 0;                    // 마지막 조회 시각. 장중 재기동 점검 순번을 이걸로 정한다
};
std::unordered_map<std::string, DailyProbe> g_probe;
std::mutex g_probe_mu;

// 후보 합집합 캐시 — 랭킹·업종 축은 KIS REST 23콜(업종 20콜은 100ms 간격)이라
//  재스캔을 20초로 당기면 이 축만으로 초당 한도를 먹는다. 반면 정배열·이격·점수를
//  다시 매기는 데 필요한 건 일봉 캐시(g_probe)와 네이버 시세 파일뿐이라 REST가 0이다.
//  그래서 "누가 후보인가"(비싼 축)와 "그 중 누가 좋은가"(싼 축)의 주기를 분리한다.
//  union_refresh_sec 안에 재호출되면 후보 목록·종목명·시장 사전을 그대로 재사용한다.
struct CandPool
{
    std::string ymd;
    std::time_t at = 0;
    std::vector<std::string> cand;
    std::unordered_map<std::string, std::string> names;
    std::unordered_map<std::string, std::string> market;
    bool have_market_map = false;
    int  etf_drop = 0;
    int  reit_drop = 0;
};
CandPool  g_pool;
std::mutex g_pool_mu;

std::string local_ymd()
{
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return std::string(buf);
}
} // namespace


std::vector<ItbCandidate> scan_itb(KisClient& scan_kis, const ItbScanCfg& cfg)
{
    std::vector<ItbCandidate> out;

    // 레짐 게이트: 코스피(0001) 당일 등락률이 risk_off 이하면 신규매수 유니버스 전면 스킵.
    auto kospi = scan_kis.get_index_price("0001");
    double idx_chg = kospi.change_rate / 100.0; // KIS는 % 단위

    if (idx_chg < cfg.risk_off_idx)
    {
        LOG_WARN("[Main] universe_from_scan: 레짐 위험회피(코스피 " +
                 std::to_string(kospi.change_rate) + "% < " +
                 std::to_string(cfg.risk_off_idx * 100.0) + "%) — 신규매수 유니버스 미등록");
        return out;
    }

    auto rank = scan_kis.fetch_value_ranking(cfg.scan_top_n, "J");
    int added = 0;

    for (const auto& r : rank)
    {
        if (added >= cfg.max_register)
        {
            break;
        }

        double chg = r.change_rate / 100.0; // % → 비율

        // 필터①: 등락률 밴드(강세 모멘텀, 급등 추격 배제)
        if (chg < cfg.chg_min || chg > cfg.chg_max)
        {
            continue;
        }

        // 필터②: 최소가(동전주·호가스프레드 배제)
        if (r.price < cfg.min_price)
        {
            continue;
        }

        // 필터③: 수급(opt) — 외국인 T-1 확정 순매수 > 0 (후보 소수에만 조회)
        if (cfg.sd_filter)
        {
            auto tr = scan_kis.get_investor_trend(r.ticker);

            if (tr.foreign_net <= 0)
            {
                LOG_INFO("[Main]   - ITB 스캔 제외 " + r.ticker + " 외국인순매수<=0");
                continue;
            }
        }

        // 통과 → 신규 진입 유니버스로 등록(당일 시가 앵커 주입).
        //  ⚠️ 앵커는 랭킹 스냅샷 현재가(r.price)가 아니라 실제 당일 시가여야 함.
        //  갭업일엔 스냅샷=장중 고점 근처라 앵커가 고점에 고정되어 돌파 진입이 영구 차단됨.
        //  inquire-price(FHKST01010100)의 stck_oprc로 진짜 시가를 조회, 0이면 r.price 폴백.
        double day_open = scan_kis.get_fundamentals(r.ticker).open;

        if (day_open <= 0.0)
        {
            day_open = r.price;
        }

        out.push_back({r.ticker, r.name, day_open});
        LOG_INFO("[Main]   + ITB 스캔 " + r.ticker + " " + r.name + " (등락 " +
                 std::to_string(r.change_rate) + "% 가격 " +
                 std::to_string((long long)r.price) + " 시가앵커 " +
                 std::to_string((long long)day_open) + " 거래대금 " +
                 std::to_string((long long)r.trade_value) + ")");
        ++added;
    }

    LOG_INFO("[Main] universe_from_scan: 후보 " + std::to_string(rank.size()) +
             "종목 중 " + std::to_string(added) + "종목 등록");
    return out;
}

std::vector<std::string> scan_devscale(KisClient& c, const DevScanCfg& cfg,
                                       std::unordered_map<std::string, std::string>* out_names,
                                       std::unordered_map<std::string, double>* out_scores)
{
    std::vector<std::string> out;
    const std::string ymd = local_ymd();   // 일봉 캐시·후보 풀 캐시의 거래일 키
    // 후보 수집 단계에서 티커→종목명을 함께 보관해, 최종 등록 확정 때 out_names에 채운다.
    std::unordered_map<std::string, std::string> cand_names;
    // 티커→시장("KOSPI"/"KOSDAQ"). 두 곳에서 채운다.
    //   (1) universe_scan.json "market_map" — 스냅샷 전종목 사전. KIS 랭킹축으로만 들어온
    //       티커의 시장도 여기서 해석된다.
    //   (2) 같은 파일 "universe[].market" — top-N 항목의 태그(값은 (1)과 동일).
    //  market_map이 있는데도 사전에 없는 티커는 코스피·코스닥 보통주가 아니다(ETN 등) → UNKNOWN.
    //  market_map이 없는 구 파일에서는 태그 없는 후보를 KOSPI로 간주해 기존 동작을 유지한다.
    std::unordered_map<std::string, std::string> cand_market;
    // 랭킹 축이 실어오는 현재가. 일봉 캐시의 close는 전일치라 장중 고정되므로
    //  이격(pull) 판정은 이 값을 쓴다. REST 추가 없이 매 재스캔마다 갱신된다.
    std::unordered_map<std::string, double> cand_px;
    std::unordered_map<std::string, double> cand_val;   // 누적 거래대금(원) — 유동성 하한용
    std::unordered_map<std::string, std::string> px_names;  // 시세 파일이 준 종목명

    // 전 종목 장중 시세 파일. 네이버 벌크를 묶어오므로 KIS 초당 한도를 쓰지 않고
    //  후보 전체의 현재가를 얻는다. 이게 있어야 정배열·이격을 매 재스캔마다 다시 판정한다.
    if (!cfg.prices_file.empty())
    {
        std::ifstream pf(cfg.prices_file);

        if (!pf)
        {
            LOG_WARN("[Main] 전 종목 시세 파일 없음(" + cfg.prices_file + ") — 랭킹 축 가격만 쓴다");
        }
        else
        {
            try
            {
                nlohmann::json pj; pf >> pj;
                const auto pm = pj.value("prices", nlohmann::json::object());

                for (auto it = pm.begin(); it != pm.end(); ++it)
                {
                    const double px = it.value().value("px", 0.0);

                    if (px <= 0.0)
                    {
                        continue;
                    }

                    cand_px[it.key()]  = px;
                    cand_val[it.key()] = it.value().value("val", 0.0);
                    const std::string nm = it.value().value("nm", std::string());

                    if (!nm.empty())
                    {
                        px_names[it.key()] = nm;
                    }
                }

                const std::time_t age = std::time(nullptr) - (std::time_t)pj.value("ts", 0);
                LOG_INFO("[Main] 전 종목 시세: " + std::to_string(cand_px.size()) +
                         "종목 (" + std::to_string((long long)age) + "초 전 갱신)");

                // 이 파일이 멈추면 px_live가 전일 종가로 돌아가 정배열·이격 판정이
                //  장 마감까지 얼어붙는다. 재스캔은 돈지만 결과가 같아 구분이 안 된다.
                if (age > 600)
                {
                    LOG_WARN("[Main] 전 종목 시세가 " + std::to_string((long long)age) +
                             "초 지났다 — 사이드카 확인 필요. 정배열 판정이 전일 종가로 고정된다");
                }
            }
            catch (const std::exception& e)
            { LOG_WARN(std::string("[Main] 전 종목 시세 파일 파싱 실패: ") + e.what()); }
        }
    }

    bool have_market_map = false;
    auto label_name = [&](const std::string& t) -> std::string
    {
        auto it = cand_names.find(t);
        return it != cand_names.end() ? it->second : std::string();
    };
    auto market_of = [&](const std::string& t) -> std::string
    {
        auto it = cand_market.find(t);

        if (it != cand_market.end() && !it->second.empty())
        {
            return it->second;
        }

        return have_market_map ? std::string("UNKNOWN") : std::string("KOSPI");
    };

    // ── 시장별 risk_off 게이트(2026-08-19 회의) ──────────────────────────────
    //  코스피 급락은 전이 회피를 위해 코스피·코스닥 신규진입 모두에 영향(코스닥은 하루 늦게 따라오는
    //  전이 지연이 잦음). 코스닥 종목은 이중 AND — (코스피 정상 AND 코스닥 정상)일 때만 통과.
    //  kosdaq_enabled=false면 코스닥 지수 조회조차 생략하고 코스닥 후보는 아래에서 전량 드롭.
    double kospi_chg = c.get_index_price("0001").change_rate / 100.0; // KIS는 % 단위
    bool kospi_off   = kospi_chg < cfg.risk_off_idx;
    bool kosdaq_off  = false;
    double kosdaq_chg = 0.0;

    if (cfg.kosdaq_enabled)
    {
        kosdaq_chg = c.get_index_price("1001").change_rate / 100.0; // 코스닥 종합지수
        kosdaq_off = kosdaq_chg < cfg.risk_off_idx_kosdaq;
    }

    const bool kospi_pass  = !kospi_off;                                       // 코스피 종목 통과 가능?
    const bool kosdaq_pass = cfg.kosdaq_enabled && !kospi_off && !kosdaq_off;  // 코스닥 종목 통과 가능?(이중 AND)
    // 이 종목의 시장이 지금 신규진입 허용 상태인가.
    auto market_allows = [&](const std::string& t) -> bool
    {
        const std::string mk = market_of(t);

        if (mk == "KOSDAQ")
        {
            return kosdaq_pass;
        }

        if (mk == "UNKNOWN")
        {
            return kosdaq_pass;  // 시장 미상 — 코스닥과 같은 보수 판정(닫혀 있으면 드롭)
        }

        return kospi_pass;
    };

    if (!kospi_pass && !kosdaq_pass)
    {
        // 모든 시장이 위험회피 → 후보 수집·일봉 기동 점검 전부 생략(기존 조기 스킵과 동일 비용).
        LOG_WARN("[Main] DEVSCALE 스캔: 레짐 위험회피(코스피 " + std::to_string(kospi_chg * 100.0) +
                 "%" + (cfg.kosdaq_enabled ? ", 코스닥 " + std::to_string(kosdaq_chg * 100.0) + "%" : "") +
                 ") — 신규 유니버스 스킵");
        return out;
    }

    // 1단: 시총 상위 ∪ 거래대금 상위 후보 수집(가격 필터·중복 제거). 정배열
    //      프리필터로 상당수가 탈락하므로 여기선 max_register로 자르지 않고 넓게 모은다.
    std::vector<std::string> cand;
    std::unordered_set<std::string> seen;
    // ETF/ETN 배제(개별주만) — 브랜드 접두사(경계검사)∪상품 토큰. KIS 축은 이미 KisClient에서
    //  걸러지지만, data.go.kr 축(구조상 ETF-free지만 방어)과 함께 한 규칙으로 이중 차단한다.
    static const std::vector<std::string> kEtfPrefixes =
        etf_filter::load_list("etf_prefixes.json", etf_filter::default_prefixes());
    static const std::vector<std::string> kEtfTokens =
        etf_filter::load_list("etf_name_tokens.json", etf_filter::default_tokens());
    // 리츠 배제 — ETF와 별도 축(접미사 일치). 리츠는 이격·정배열 매매 대상이 아닌데
    //  배당·NAV로 움직여 일봉 프리필터를 그대로 통과한다(334890 이지스밸류플러스리츠 유입).
    static const std::vector<std::string> kReitSuffixes =
        etf_filter::load_list("reit_name_suffixes.json", etf_filter::default_reit_suffixes());
    static const std::vector<std::string> kReitExacts =
        etf_filter::load_list("reit_names.json", etf_filter::default_reit_exacts());
    int etf_drop = 0;
    int reit_drop = 0;
    auto take = [&](const std::vector<KisClient::RankingStock>& rank)
    {
        for (const auto& r : rank)
        {
            if (r.price < cfg.min_price)
            {
                continue;
            }

            if (cfg.max_price > 0.0 && r.price > cfg.max_price)
            {
                continue;
            }

            if (etf_filter::is_etf_like(r.name, kEtfPrefixes, kEtfTokens)) { ++etf_drop; continue; }

            if (etf_filter::is_reit_like(r.name, kReitSuffixes, kReitExacts)) { ++reit_drop; continue; }

            if (r.price > 0.0)
            {
                cand_px[r.ticker] = r.price;
            }

            if (!seen.insert(r.ticker).second)
            {
                continue;
            }

            cand.push_back(r.ticker);
            cand_names[r.ticker] = r.name; // 종목명 보관(로그 라벨용)
        }
    };
    // 후보 합집합 — 비싼 축(KIS 랭킹·업종 REST)이라 union_refresh_sec 동안 재사용한다.
    //  0이면 매 호출 새로 모은다(기존 동작).
    bool pool_hit = false;
    long long pool_age = 0;

    if (cfg.union_refresh_sec > 0)
    {
        std::lock_guard<std::mutex> lk(g_pool_mu);

        if (g_pool.ymd == ymd && !g_pool.cand.empty() &&
            std::time(nullptr) - g_pool.at < cfg.union_refresh_sec)
        {
            cand            = g_pool.cand;
            cand_names      = g_pool.names;
            cand_market     = g_pool.market;
            have_market_map = g_pool.have_market_map;
            etf_drop        = g_pool.etf_drop;
            reit_drop       = g_pool.reit_drop;
            pool_age        = (long long)(std::time(nullptr) - g_pool.at);
            pool_hit        = true;
        }
    }

    if (pool_hit)
    {
        // 현재가·거래대금은 시세 파일에서 방금 읽은 값을 쓴다. 랭킹 축이 실어오던
        //  스냅샷가는 재사용분에 없지만, 네이버 쪽이 더 최신이라 판정에는 그편이 낫다.
        LOG_INFO("[Main] DEVSCALE 후보 합집합 재사용: " + std::to_string(cand.size()) +
                 "종목 (" + std::to_string(pool_age) +
                 "초 전 수집, 갱신주기 " + std::to_string(cfg.union_refresh_sec) + "초)");
    }
    else
    {
        // 4축(우선) — data.go.kr 시총∪거래대금 유니버스 피드. 후보 풀 "맨 앞"에 넣어 기동 점검 우선순위 확보.
        //  KIS 30행캡·ETF 잠식을 우회한 개별주 깊은 풀(ETF-free 구조적). 파일 없으면 조용히 스킵(하위호환).
        if (!cfg.universe_file.empty())
        {
            std::ifstream f(cfg.universe_file);

            if (!f)
            {
                LOG_WARN("[Main] DEVSCALE 유니버스 파일 없음(" + cfg.universe_file +
                         ") — data.go.kr 축 스킵, KIS 랭킹 축만 사용");
            }
            else
            {
                try
                {
                    nlohmann::json j;
                    f >> j;
                    const std::string basDt = j.value("basDt", std::string());

                    // 전종목 코드→시장 사전(있으면). universe(top-N)보다 먼저 적재해, KIS 랭킹축
                    //  티커의 시장도 해석되게 한다 — 없으면 kosdaq_enabled 게이트가 그쪽으로 샌다.
                    if (j.contains("market_map") && j["market_map"].is_object())
                    {
                        for (auto it = j["market_map"].begin(); it != j["market_map"].end(); ++it)
                        {
                            if (it.value().is_string())
                            {
                                cand_market[it.key()] = it.value().get<std::string>();
                            }
                        }

                        have_market_map = !cand_market.empty();
                    }

                    const auto arr = j.value("universe", nlohmann::json::array());
                    int added_file = 0, dup = 0;

                    for (const auto& e : arr)
                    {
                        const std::string t = e.value("ticker", std::string());

                        if (t.empty())
                        {
                            continue;
                        }

                        const std::string nm = e.value("name", std::string());

                        if (etf_filter::is_etf_like(nm, kEtfPrefixes, kEtfTokens)) { ++etf_drop; continue; }

                        if (etf_filter::is_reit_like(nm, kReitSuffixes, kReitExacts)) { ++reit_drop; continue; }
                        const double px = e.value("close", 0.0);

                        // close(0=미제공)면 가격필터는 뒤 정배열 프리필터의 일봉이 대신 검증.
                        if (px > 0.0 && px < cfg.min_price)
                        {
                            continue;
                        }

                        if (cfg.max_price > 0.0 && px > cfg.max_price)
                        {
                            continue;
                        }

                        if (!seen.insert(t).second) { ++dup; continue; }
                        cand.push_back(t);
                        cand_names[t] = nm;
                        cand_market[t] = e.value("market", std::string()); // 시장별 risk_off 게이트용(없으면 KOSPI 간주)
                        ++added_file;
                    }

                    LOG_INFO("[Main] DEVSCALE data.go.kr 축(기준일 " + basDt + "): 파일 " +
                             std::to_string(arr.size()) + "종목 → 신규 " + std::to_string(added_file) +
                             " union (중복 " + std::to_string(dup) + ")");
                }
                catch (const std::exception& ex)
                {
                    LOG_WARN("[Main] DEVSCALE 유니버스 파일 파싱 실패(" + cfg.universe_file +
                             "): " + std::string(ex.what()) + " — data.go.kr 축 스킵");
                }
            }
        }

        take(c.fetch_kr_ranking(cfg.scan_top_n, "J"));          // 시총 상위
        take(c.fetch_value_ranking(cfg.value_top_n, "J", "3")); // 거래대금 상위
        // 랭킹 TR은 축마다 상위 30행 고정(연속조회 불가) → 정렬축을 하나 더 union해 풀을 넓힌다.
        //  거래증가율(1)은 대형주에 편중된 시총·거래대금축과 겹침이 적어(중소형 모멘텀) 정배열 후보를 늘린다.
        take(c.fetch_value_ranking(cfg.value_top_n, "J", "1")); // 거래증가율 상위

        // 업종 등락률 축 — 위 세 축과 data.go.kr 축은 전부 전일 이전 상태를 본다. 이 축만 장중을 본다.
        //  등락률 내림차순이라 상위 N행이 곧 "지금 강한" 종목이고, 업종을 순회하므로 한 섹터가
        //  풀을 독식하지 않는다. 정배열·과확장 판정은 뒤 프리필터가 그대로 하므로 여기서는 걸러만 준다.
        if (!cfg.sector_codes.empty())
        {
            const std::size_t before = cand.size();
            int sec_ok = 0, sec_weak = 0;

            for (const auto& sc : cfg.sector_codes)
            {
                auto rows = c.fetch_sector_ranking(sc, cfg.sector_top_n);
                // 26콜을 쉬지 않고 내면 8.8콜/s로 나가 문서상 한도 20/s의 절반을 이 축 하나가
                //  버스트로 먹는다(09-08: ranking/fluctuation HTTP 500 70건). 재스캔 주기가
                //  600초라 2.6초→5.2초 지연은 무시할 만하다.
                std::this_thread::sleep_for(std::chrono::milliseconds(100));

                if (rows.empty())
                {
                    continue;
                }

                ++sec_ok;
                std::vector<KisClient::RankingStock> strong;
                strong.reserve(rows.size());

                for (const auto& r : rows)
                {
                    if (r.change_rate < cfg.sector_min_chg) { ++sec_weak; continue; }
                    strong.push_back(r);
                }

                take(strong);
            }

            LOG_INFO("[Main] DEVSCALE 업종 등락률 축: " + std::to_string(sec_ok) + "/" +
                     std::to_string(cfg.sector_codes.size()) + "업종 응답, 신규 " +
                     std::to_string(cand.size() - before) + "종목 union (약세컷 " +
                     std::to_string(sec_weak) + ")");
        }

        // 전 종목 확장 — market_map(코스피+코스닥 전체)을 후보로 부은다.
        //  종목명은 시세 파일에서 가져와 ETF·리츠 필터를 그대로 적용한다(이름 없으면 버린다).
        if (cfg.full_market && have_market_map)
        {
            const std::size_t before_fm = cand.size();
            int no_name = 0;

            for (const auto& kv : cand_market)
            {
                const std::string& t = kv.first;

                if (t.size() != 6)
                {
                    continue;
                }

                auto itn = px_names.find(t);

                if (itn == px_names.end()) { ++no_name; continue; }

                if (etf_filter::is_etf_like(itn->second, kEtfPrefixes, kEtfTokens)) { ++etf_drop; continue; }

                if (etf_filter::is_reit_like(itn->second, kReitSuffixes, kReitExacts)) { ++reit_drop; continue; }
                auto itp = cand_px.find(t);

                if (itp == cand_px.end() || itp->second < cfg.min_price)
                {
                    continue;
                }

                if (cfg.max_price > 0.0 && itp->second > cfg.max_price)
                {
                    continue;
                }

                if (!seen.insert(t).second)
                {
                    continue;
                }

                cand.push_back(t);
                cand_names[t] = itn->second;
            }

            LOG_INFO("[Main] 전 종목 확장: 신규 " + std::to_string(cand.size() - before_fm) +
                     "종목 union (시세없음 " + std::to_string(no_name) + ", 총 후보 " +
                     std::to_string(cand.size()) + ")");
        }

        if (cfg.union_refresh_sec > 0)
        {
            std::lock_guard<std::mutex> lk(g_pool_mu);
            g_pool.ymd             = ymd;
            g_pool.at              = std::time(nullptr);
            g_pool.cand            = cand;
            g_pool.names           = cand_names;
            g_pool.market          = cand_market;
            g_pool.have_market_map = have_market_map;
            g_pool.etf_drop        = etf_drop;
            g_pool.reit_drop       = reit_drop;
        }
    }

    if (!cfg.require_aligned)
    {
        // 프리필터 off — 기존 동작(후보 앞에서부터 max_register개).
        for (const auto& t : cand)
        {
            if (!market_allows(t))
            {
                continue;  // 시장 risk_off 게이트(코스닥 후보는 kosdaq_pass일 때만)
            }

            if ((int)out.size() >= cfg.max_register)
            {
                break;
            }

            out.push_back(t);

            if (out_names)
            {
                (*out_names)[t] = label_name(t);
            }
        }

        return out;
    }

    // 정배열 판정(SMA5>10>20>60)은 아래 캐시된 SMA로 직접 비교한다.
    //  DeviationScaleStrategy::is_aligned와 같은 규칙.

    // 2단: 정배열 프리필터 — 후보를 일봉으로 검사해 정배열=Y(≥60봉)만 통과.
    //      데이터부족(신규상장 <60봉) 종목은 여기서 자동 제외(예: 365660).
    //      일봉 조회 비용을 align_probe_max로 캡(초과분은 미검사로 로깅 — 무음 절단 금지).
    //      score_top_n>0이면 통과분을 (ticker, 점수)로 모아 3단에서 랭킹·절단.
    //  점수는 원자료를 바로 더하지 않는다. 추세·눌림·변동성은 단위도 일별 분산도 달라서
    //   그대로 더하면 그날 우연히 많이 벌어진 축이 점수를 지배한다. 통과 풀 안에서 각각
    //   z-score로 정규화하고 ±2σ에서 자른 뒤 가중합한다(스케일-프리 + 이상치 1종목 지배 차단).
    struct Feat { std::string ticker; double trend, pull, vol, score; };
    std::vector<Feat> passed;
    int probed = 0, aligned_cnt = 0, short_bars = 0, overext = 0;
    int fetched = 0, cache_hit = 0, refreshed = 0;
    int illiquid = 0;   // 거래대금 하한 미달로 버린 수

    // 장중 재기동 점검 대상 고르기 — 오늘의 일봉은 현재가가 종가 자리에 들어와 장중 내내 변한다.
    //  날짜만 보고 캐시를 히트시키면 기동 시각의 판정이 마감까지 얼어붙어 재스캔이 같은 종목만
    //  돌려준다. 그렇다고 매번 전량을 다시 조회할 수는 없다 — 3분봉 시세 폴링이 이미 REST 초당
    //  한도를 쓰고 있어(HTTP 500 재시도) 여기서 수백 건을 더 얹으면 발주 경로까지 밀린다.
    //  그래서 "가장 오래 안 본 순"으로 예산(align_refresh_max)만큼만 다시 본다. 재스캔이 반복되면
    //  후보 전체를 순회하게 되고, 한 바퀴에 걸리는 시간은 후보수/예산 × 재스캔주기다.
    std::unordered_set<std::string> refresh_set;

    if (cfg.align_refresh_max > 0)
    {
        const std::time_t now_t = std::time(nullptr);
        std::vector<std::pair<std::time_t, std::string>> stale;
        {
            std::lock_guard<std::mutex> lk(g_probe_mu);

            for (const auto& t : cand)
            {
                auto it = g_probe.find(t);

                if (it == g_probe.end() || it->second.ymd != ymd)
                {
                    continue;  // 미조회분은 어차피 미스
                }

                if (now_t - it->second.at < cfg.align_refresh_sec)
                {
                    continue;  // 아직 신선하다
                }

                stale.emplace_back(it->second.at, t);
            }
        }

        std::sort(stale.begin(), stale.end(),
                  [](const std::pair<std::time_t, std::string>& a,
                     const std::pair<std::time_t, std::string>& b) { return a.first < b.first; });
        const size_t take = std::min<size_t>(stale.size(), (size_t)cfg.align_refresh_max);

        for (size_t i = 0; i < take; ++i)
        {
            refresh_set.insert(stale[i].second);
        }

        if (!stale.empty())
        {
            LOG_INFO("[Main] DEVSCALE 일봉 재기동 점검: 대상 " + std::to_string(stale.size()) +
                     "종목 중 " + std::to_string(take) + "건 (예산 " +
                     std::to_string(cfg.align_refresh_max) + ", 신선도 " +
                     std::to_string(cfg.align_refresh_sec) + "초)");
        }
    }

    for (const auto& t : cand)
    {
        if (!market_allows(t))
        {
            continue;  // 시장 risk_off 게이트(코스닥 후보는 kosdaq_pass일 때만, 일봉 기동 점검 비용도 아낌)
        }

        // 유동성 하한 — 거래대금이 받침하지 못하는 종목은 체결이 안 되거나 슬리피지로 생익을 먹는다.
        //  시세 파일이 없어 거래대금을 모르는 후보는 통과시킨다(기존 동작 유지).
        if (cfg.min_turnover > 0.0)
        {
            auto itv = cand_val.find(t);

            if (itv != cand_val.end() && itv->second > 0.0 && itv->second < cfg.min_turnover)
            { ++illiquid; continue; }
        }

        // 스코어링 시엔 max_register 대신 align_probe_max까지 넓게 모아 랭킹(더 나은 상위 N).
        if (cfg.score_top_n <= 0 && (int)passed.size() >= cfg.max_register)
        {
            break;
        }

        DailyProbe pr;
        bool hit = false;
        {
            std::lock_guard<std::mutex> lk(g_probe_mu);
            auto it = g_probe.find(t);

            if (it != g_probe.end() && it->second.ymd == ymd &&
                refresh_set.find(t) == refresh_set.end()) { pr = it->second; hit = true; }
        }

        if (!hit && refresh_set.find(t) != refresh_set.end())
        {
            ++refreshed;
        }

        if (!hit)
        {
            // 캐시 미스만 REST를 쓴다. 상한도 실제 조회 수(fetched)에만 건다.
            if (fetched >= cfg.align_probe_max)
            {
                LOG_WARN("[Main] DEVSCALE 정배열 프리필터: 일봉 조회 상한(" +
                         std::to_string(cfg.align_probe_max) + ") 도달 — 남은 후보는 다음 재스캔에서 채움");
                break;
            }

            // 하루 첫 스캔은 수백 건이 연속으로 나간다. KIS 레이트 리밋 여유를 둔다
            //  (캐시 히트 경로에는 걸리지 않으므로 재스캔 지연에는 영향 없음).
            // 60ms에서는 초당한도(EGW00201) 거부가 09-08 하루 149건 났고 CANCEL뿐 아니라
            //  NEW에도 걸려 진입이 4초씩 밀렸다. 같은 날 주문 RTT p50이 09시 381ms에서
            //  10시 1870ms로 단조증가한 것도 계좌 단위 REST 누적 부하로 보여 150ms로 올린다.
            if (fetched > 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
            }

            auto d = c.get_daily_ohlcv(t, cfg.align_daily_n);
            ++fetched;
            pr.ymd  = ymd;
            pr.at   = std::time(nullptr);
            pr.bars = (int)d.size();

            if (pr.bars >= 60)
            {
                auto sma = [&](int n) { double v = 0.0; for (int i = 0; i < n; ++i) v += d[i].close; return v / n; };
                pr.s5 = sma(5); pr.s10 = sma(10); pr.s20 = sma(20); pr.s60 = sma(60);
                pr.r5 = d[4].close; pr.r10 = d[9].close;
                pr.r20 = d[19].close; pr.r60 = d[59].close;
                pr.close = d[0].close;
                // ATR(14) — True Range = max(고−저, |고−전일종가|, |저−전일종가|)의 14봉 평균.
                //  d[0]이 최신이므로 d[i+1]이 i의 전일. 종가로 나눠 종목 간 비교 가능한 비율로 만든다.
                double tr_sum = 0.0;
                int    tr_n   = 0;

                for (size_t i = 0; i + 1 < d.size() && tr_n < 14; ++i, ++tr_n)
                {
                    const double prev_c = d[i + 1].close;
                    const double hi = d[i].high, lo = d[i].low;
                    double tr = hi - lo;
                    const double a = std::fabs(hi - prev_c), b = std::fabs(lo - prev_c);

                    if (a > tr)
                    {
                        tr = a;
                    }

                    if (b > tr)
                    {
                        tr = b;
                    }

                    tr_sum += tr;
                }

                pr.atr_pct = (tr_n > 0 && pr.close > 0.0) ? (tr_sum / tr_n) / pr.close : 0.0;
            }

            std::lock_guard<std::mutex> lk(g_probe_mu);
            g_probe[t] = pr;
        }
        else
        {
            ++cache_hit;
        }

        ++probed;

        if (pr.bars < 60) { ++short_bars; continue; }
        // 오늘 가격을 최신 봉으로 접어 넣어 SMA를 다시 계산한다. 일봉 캐시는
        //  include_today=false라 전일치에서 멈춰 있고, 그대로 쓰면 정배열 판정이
        //  하루 종일 얼어붙어 재스캔이 같은 종목만 돌려준다. 랭킹 축이 실어오는
        //  현재가를 쓰므로 REST 추가 없이 2분마다 정배열·이격을 다시 판정한다.
        double px = pr.close;
        {
            auto itp = cand_px.find(t);

            if (itp != cand_px.end() && itp->second > 0.0)
            {
                px = itp->second;
            }
        }

        double s5 = pr.s5, s10 = pr.s10, s20 = pr.s20, s60 = pr.s60;

        if (px > 0.0 && pr.r60 > 0.0)
        {
            s5  = (pr.s5  *  5 - pr.r5  + px) /  5.0;
            s10 = (pr.s10 * 10 - pr.r10 + px) / 10.0;
            s20 = (pr.s20 * 20 - pr.r20 + px) / 20.0;
            s60 = (pr.s60 * 60 - pr.r60 + px) / 60.0;
        }

        if (!(s5 > s10 && s10 > s20 && s20 > s60))
        {
            continue;
        }

        double trend = s60 > 0.0 ? (s5 - s60) / s60 : 0.0;   // 추세강도(정배열 기울기)
        double pull = s20 > 0.0 ? (px - s20) / s20 : 0.0;     // 눌림깊이(음수=SMA20 아래)

        // 과확장 컷 — 일봉 이격이 상한 초과면 제외(존 밴드 진입 불가한 폭등주 슬롯 낭비 방지).
        if (cfg.max_dev_pct > 0.0 && pull > cfg.max_dev_pct)
        {
            ++overext;
            continue;
        }

        // 과확장 하한 — 밴드 아래(덜 벌어진 종목)는 눌림 슬리브 몫이다.
        if (cfg.min_dev_pct > 0.0 && pull < cfg.min_dev_pct)
        {
            ++overext;
            continue;
        }

        passed.push_back({t, trend, pull, pr.atr_pct, 0.0});
        ++aligned_cnt;
    }

    // 2.5단: 횡단면 정규화 → 종합 점수 하나. 이 점수가 등록 순서(=진입 우선순위)와
    //  종목별 비중 배수 두 가지를 모두 정한다.
    //    S = w_trend·z(추세) + w_pull·z(-눌림) - w_vol·z(변동성)
    //  변동성은 뺀다 — 추세·눌림이 같다면 덜 흔들리는 쪽이 낫다.
    {
        auto zscore = [&](double Feat::*field, bool invert, std::vector<double>& z)
        {
            const size_t n = passed.size();
            z.assign(n, 0.0);

            if (n < 2)
            {
                return;
            }

            double mean = 0.0;

            for (const auto& f : passed)
            {
                mean += f.*field;
            }

            mean /= static_cast<double>(n);
            double var = 0.0;

            for (const auto& f : passed) { const double d0 = f.*field - mean; var += d0 * d0; }
            var /= static_cast<double>(n);
            const double sd = std::sqrt(var);

            // 분산이 사실상 0이면(전 종목 동일) 정규화가 무의미 → 전부 0으로 두어 균등 폴백.
            if (!(sd > 1e-12))
            {
                return;
            }

            for (size_t i = 0; i < n; ++i)
            {
                double v = (passed[i].*field - mean) / sd;

                if (v > 2.0)
                {
                    v = 2.0;
                }

                if (v < -2.0)
                {
                    v = -2.0;
                }

                z[i] = invert ? -v : v;
            }
        };
        std::vector<double> zt, zp, zv;
        zscore(&Feat::trend, false, zt);
        zscore(&Feat::pull,  true,  zp);   // 눌림은 음수(SMA20 아래)일수록 좋다 → 부호 반전
        zscore(&Feat::vol,   false, zv);

        for (size_t i = 0; i < passed.size(); ++i)
        {
            passed[i].score = cfg.score_w_trend * zt[i] + cfg.score_w_pullback * zp[i]
                            - cfg.score_w_vol * zv[i];
        }
    }

    // 3단: 스코어 랭킹(옵션) — score_top_n>0이면 상위 N만, 아니면 통과 순서대로.
    if (cfg.score_top_n > 0)
    {
        std::sort(passed.begin(), passed.end(),
                  [](const Feat& a, const Feat& b) { return a.score > b.score; });
        int take_n = (int)passed.size() < cfg.score_top_n ? (int)passed.size() : cfg.score_top_n;

        for (int i = 0; i < take_n; ++i)
        {
            out.push_back(passed[i].ticker);

            if (out_names)
            {
                (*out_names)[passed[i].ticker]  = label_name(passed[i].ticker);
            }

            if (out_scores)
            {
                (*out_scores)[passed[i].ticker] = passed[i].score;
            }
        }

        LOG_INFO("[Main] DEVSCALE 횡단면 스코어: 정배열통과=" + std::to_string(passed.size()) +
                 " → 상위 " + std::to_string(take_n) + " 선정 (w_trend=" +
                 std::to_string(cfg.score_w_trend) + " w_pull=" + std::to_string(cfg.score_w_pullback) +
                 " w_supply=" + std::to_string(cfg.score_w_supply) + ")");
    }
    else
    {
        // score_top_n=0이어도 점수순으로 내보낸다. 등록 순서가 그대로 진입 우선순위가 되므로
        //  여기서 안 정렬하면 유니버스 파일 순서(시총·거래대금)가 우선순위를 먹는다.
        std::sort(passed.begin(), passed.end(),
                  [](const Feat& a, const Feat& b) { return a.score > b.score; });

        for (const auto& p : passed)
        {
            out.push_back(p.ticker);

            if (out_names)
            {
                (*out_names)[p.ticker]  = label_name(p.ticker);
            }

            if (out_scores)
            {
                (*out_scores)[p.ticker] = p.score;
            }
        }
    }

    LOG_INFO("[Main] DEVSCALE 정배열 프리필터: 후보=" + std::to_string(cand.size()) +
             " ETF드롭=" + std::to_string(etf_drop) +
             " 리츠드롭=" + std::to_string(reit_drop) +
             " 검사=" + std::to_string(probed) +
             " (일봉조회=" + std::to_string(fetched) +
             " 재기동 점검=" + std::to_string(refreshed) +
             " 캐시=" + std::to_string(cache_hit) + ")" +
             " 정배열=" + std::to_string(aligned_cnt) +
             " 데이터부족(<60봉)=" + std::to_string(short_bars) +
             " 과확장컷=" + std::to_string(overext) +
             " 거래대금미달=" + std::to_string(illiquid) +
             " 등록=" + std::to_string(out.size()));
    return out;
}

} // namespace universe
