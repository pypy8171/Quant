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
// ── 일봉 정배열 프로브 캐시 ────────────────────────────────────────────────
//  일봉은 장중에 바뀌지 않는다(D-005로 당일봉은 어차피 절단). 그런데 재스캔마다
//  후보 전체의 일봉을 다시 조회하면 REST 비용이 rescan_interval에 비례해 늘어,
//  후보 풀을 넓히는 것 자체가 불가능했다(align_probe_max=150이 그 때문의 캡).
//  날짜가 같으면 재사용해 하루 1회로 고정한다 → 풀 확대의 비용이 O(종목수/일).
struct DailyProbe
{
    std::string ymd;                       // 조회 시각의 로컬 날짜(YYYYMMDD)
    int    bars  = 0;                      // 확보 봉수(<60이면 판정 불가)
    double s5 = 0.0, s10 = 0.0, s20 = 0.0, s60 = 0.0;
    double close = 0.0;                    // 최신 종가(d[0])
    double atr_pct = 0.0;                  // ATR(14)/종가 — 변동성. 정배열 판정용 일봉 재활용(추가 REST 0)
};
std::unordered_map<std::string, DailyProbe> g_probe;
std::mutex g_probe_mu;

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
            break;
        double chg = r.change_rate / 100.0; // % → 비율
        // 필터①: 등락률 밴드(강세 모멘텀, 급등 추격 배제)
        if (chg < cfg.chg_min || chg > cfg.chg_max)
            continue;
        // 필터②: 최소가(동전주·호가스프레드 배제)
        if (r.price < cfg.min_price)
            continue;
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
            day_open = r.price;
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
    // 후보 수집 단계에서 티커→종목명을 함께 보관해, 최종 등록 확정 때 out_names에 채운다.
    std::unordered_map<std::string, std::string> cand_names;
    // 티커→시장("KOSPI"/"KOSDAQ"). 두 곳에서 채운다.
    //   (1) universe_scan.json "market_map" — 스냅샷 전종목 사전. KIS 랭킹축으로만 들어온
    //       티커의 시장도 여기서 해석된다.
    //   (2) 같은 파일 "universe[].market" — top-N 항목의 태그(값은 (1)과 동일).
    //  market_map이 있는데도 사전에 없는 티커는 코스피·코스닥 보통주가 아니다(ETN 등) → UNKNOWN.
    //  market_map이 없는 구 파일에서는 태그 없는 후보를 KOSPI로 간주해 기존 동작을 유지한다.
    std::unordered_map<std::string, std::string> cand_market;
    bool have_market_map = false;
    auto label_name = [&](const std::string& t) -> std::string
    {
        auto it = cand_names.find(t);
        return it != cand_names.end() ? it->second : std::string();
    };
    auto market_of = [&](const std::string& t) -> std::string
    {
        auto it = cand_market.find(t);
        if (it != cand_market.end() && !it->second.empty()) return it->second;
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
        if (mk == "KOSDAQ")  return kosdaq_pass;
        if (mk == "UNKNOWN") return kosdaq_pass; // 시장 미상 — 코스닥과 같은 보수 판정(닫혀 있으면 드롭)
        return kospi_pass;
    };
    if (!kospi_pass && !kosdaq_pass)
    {
        // 모든 시장이 위험회피 → 후보 수집·일봉 프로브 전부 생략(기존 조기 스킵과 동일 비용).
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
            if (r.price < cfg.min_price) continue;
            if (cfg.max_price > 0.0 && r.price > cfg.max_price) continue;
            if (etf_filter::is_etf_like(r.name, kEtfPrefixes, kEtfTokens)) { ++etf_drop; continue; }
            if (etf_filter::is_reit_like(r.name, kReitSuffixes, kReitExacts)) { ++reit_drop; continue; }
            if (!seen.insert(r.ticker).second) continue;
            cand.push_back(r.ticker);
            cand_names[r.ticker] = r.name; // 종목명 보관(로그 라벨용)
        }
    };
    // 4축(우선) — data.go.kr 시총∪거래대금 유니버스 피드. 후보 풀 "맨 앞"에 넣어 프로브 우선순위 확보.
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
                        if (it.value().is_string()) cand_market[it.key()] = it.value().get<std::string>();
                    have_market_map = !cand_market.empty();
                }
                const auto arr = j.value("universe", nlohmann::json::array());
                int added_file = 0, dup = 0;
                for (const auto& e : arr)
                {
                    const std::string t = e.value("ticker", std::string());
                    if (t.empty()) continue;
                    const std::string nm = e.value("name", std::string());
                    if (etf_filter::is_etf_like(nm, kEtfPrefixes, kEtfTokens)) { ++etf_drop; continue; }
                    if (etf_filter::is_reit_like(nm, kReitSuffixes, kReitExacts)) { ++reit_drop; continue; }
                    const double px = e.value("close", 0.0);
                    // close(0=미제공)면 가격필터는 뒤 정배열 프리필터의 일봉이 대신 검증.
                    if (px > 0.0 && px < cfg.min_price) continue;
                    if (cfg.max_price > 0.0 && px > cfg.max_price) continue;
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

    if (!cfg.require_aligned)
    {
        // 프리필터 off — 기존 동작(후보 앞에서부터 max_register개).
        for (const auto& t : cand)
        {
            if (!market_allows(t)) continue; // 시장 risk_off 게이트(코스닥 후보는 kosdaq_pass일 때만)
            if ((int)out.size() >= cfg.max_register) break;
            out.push_back(t);
            if (out_names) (*out_names)[t] = label_name(t);
        }
        return out;
    }

    // 정배열 판정(SMA5>10>20>60)은 아래 캐시된 SMA로 직접 비교한다.
    //  DeviationScaleStrategy::is_aligned와 같은 규칙.
    const std::string ymd = local_ymd();

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
    int fetched = 0, cache_hit = 0;
    for (const auto& t : cand)
    {
        if (!market_allows(t)) continue; // 시장 risk_off 게이트(코스닥 후보는 kosdaq_pass일 때만, 일봉 프로브 비용도 아낌)
        // 스코어링 시엔 max_register 대신 align_probe_max까지 넓게 모아 랭킹(더 나은 상위 N).
        if (cfg.score_top_n <= 0 && (int)passed.size() >= cfg.max_register) break;
        DailyProbe pr;
        bool hit = false;
        {
            std::lock_guard<std::mutex> lk(g_probe_mu);
            auto it = g_probe.find(t);
            if (it != g_probe.end() && it->second.ymd == ymd) { pr = it->second; hit = true; }
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
            if (fetched > 0) std::this_thread::sleep_for(std::chrono::milliseconds(60));
            auto d = c.get_daily_ohlcv(t, cfg.align_daily_n);
            ++fetched;
            pr.ymd  = ymd;
            pr.bars = (int)d.size();
            if (pr.bars >= 60)
            {
                auto sma = [&](int n) { double v = 0.0; for (int i = 0; i < n; ++i) v += d[i].close; return v / n; };
                pr.s5 = sma(5); pr.s10 = sma(10); pr.s20 = sma(20); pr.s60 = sma(60);
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
                    if (a > tr) tr = a;
                    if (b > tr) tr = b;
                    tr_sum += tr;
                }
                pr.atr_pct = (tr_n > 0 && pr.close > 0.0) ? (tr_sum / tr_n) / pr.close : 0.0;
            }
            std::lock_guard<std::mutex> lk(g_probe_mu);
            g_probe[t] = pr;
        }
        else ++cache_hit;
        ++probed;
        if (pr.bars < 60) { ++short_bars; continue; }
        if (!(pr.s5 > pr.s10 && pr.s10 > pr.s20 && pr.s20 > pr.s60)) continue;
        double s5 = pr.s5, s20 = pr.s20, s60 = pr.s60;
        double trend = s60 > 0.0 ? (s5 - s60) / s60 : 0.0;   // 추세강도(정배열 기울기)
        double px = pr.close;
        double pull = s20 > 0.0 ? (px - s20) / s20 : 0.0;     // 눌림깊이(음수=SMA20 아래)
        // 과확장 컷 — 일봉 이격이 상한 초과면 제외(존 밴드 진입 불가한 폭등주 슬롯 낭비 방지).
        if (cfg.max_dev_pct > 0.0 && pull > cfg.max_dev_pct)
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
            if (n < 2) return;
            double mean = 0.0;
            for (const auto& f : passed) mean += f.*field;
            mean /= static_cast<double>(n);
            double var = 0.0;
            for (const auto& f : passed) { const double d0 = f.*field - mean; var += d0 * d0; }
            var /= static_cast<double>(n);
            const double sd = std::sqrt(var);
            // 분산이 사실상 0이면(전 종목 동일) 정규화가 무의미 → 전부 0으로 두어 균등 폴백.
            if (!(sd > 1e-12)) return;
            for (size_t i = 0; i < n; ++i)
            {
                double v = (passed[i].*field - mean) / sd;
                if (v > 2.0) v = 2.0;
                if (v < -2.0) v = -2.0;
                z[i] = invert ? -v : v;
            }
        };
        std::vector<double> zt, zp, zv;
        zscore(&Feat::trend, false, zt);
        zscore(&Feat::pull,  true,  zp);   // 눌림은 음수(SMA20 아래)일수록 좋다 → 부호 반전
        zscore(&Feat::vol,   false, zv);
        for (size_t i = 0; i < passed.size(); ++i)
            passed[i].score = cfg.score_w_trend * zt[i] + cfg.score_w_pullback * zp[i]
                            - cfg.score_w_vol * zv[i];
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
            if (out_names)  (*out_names)[passed[i].ticker]  = label_name(passed[i].ticker);
            if (out_scores) (*out_scores)[passed[i].ticker] = passed[i].score;
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
            if (out_names)  (*out_names)[p.ticker]  = label_name(p.ticker);
            if (out_scores) (*out_scores)[p.ticker] = p.score;
        }
    }
    LOG_INFO("[Main] DEVSCALE 정배열 프리필터: 후보=" + std::to_string(cand.size()) +
             " ETF드롭=" + std::to_string(etf_drop) +
             " 리츠드롭=" + std::to_string(reit_drop) +
             " 검사=" + std::to_string(probed) +
             " (일봉조회=" + std::to_string(fetched) +
             " 캐시=" + std::to_string(cache_hit) + ")" +
             " 정배열=" + std::to_string(aligned_cnt) +
             " 데이터부족(<60봉)=" + std::to_string(short_bars) +
             " 과확장컷=" + std::to_string(overext) +
             " 등록=" + std::to_string(out.size()));
    return out;
}

} // namespace universe
