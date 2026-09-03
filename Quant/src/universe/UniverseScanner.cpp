#include "universe/UniverseScanner.h"
#include "core/Types.h"
#include "utils/EtfFilter.h"
#include "utils/Logger.h"
#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace universe
{

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
                                       std::unordered_map<std::string, std::string>* out_names)
{
    std::vector<std::string> out;
    // 후보 수집 단계에서 티커→종목명을 함께 보관해, 최종 등록 확정 때 out_names에 채운다.
    std::unordered_map<std::string, std::string> cand_names;
    // 티커→시장("KOSPI"/"KOSDAQ") — universe_scan.json의 "market" 필드에서 채운다. 태그 없는
    //  후보(KIS 랭킹축 등)는 KOSPI로 간주(KIS 입력 시장코드(FID_INPUT_ISCD)=0000축이 코스피 위주).
    std::unordered_map<std::string, std::string> cand_market;
    auto label_name = [&](const std::string& t) -> std::string
    {
        auto it = cand_names.find(t);
        return it != cand_names.end() ? it->second : std::string();
    };
    auto market_of = [&](const std::string& t) -> std::string
    {
        auto it = cand_market.find(t);
        return (it != cand_market.end() && !it->second.empty()) ? it->second : std::string("KOSPI");
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
        return market_of(t) == "KOSDAQ" ? kosdaq_pass : kospi_pass;
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
    int etf_drop = 0;
    auto take = [&](const std::vector<KisClient::RankingStock>& rank)
    {
        for (const auto& r : rank)
        {
            if (r.price < cfg.min_price) continue;
            if (cfg.max_price > 0.0 && r.price > cfg.max_price) continue;
            if (etf_filter::is_etf_like(r.name, kEtfPrefixes, kEtfTokens)) { ++etf_drop; continue; }
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
                const auto arr = j.value("universe", nlohmann::json::array());
                int added_file = 0, dup = 0;
                for (const auto& e : arr)
                {
                    const std::string t = e.value("ticker", std::string());
                    if (t.empty()) continue;
                    const std::string nm = e.value("name", std::string());
                    if (etf_filter::is_etf_like(nm, kEtfPrefixes, kEtfTokens)) { ++etf_drop; continue; }
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

    // 일봉 정배열 판정(SMA5>10>20>60) — DeviationScaleStrategy::is_aligned와 동일 규칙.
    auto aligned_daily = [](const std::vector<MarketData>& d) -> bool
    {
        if ((int)d.size() < 60) return false;
        auto sma = [&](int n) { double s = 0.0; for (int i = 0; i < n; ++i) s += d[i].close; return s / n; };
        return sma(5) > sma(10) && sma(10) > sma(20) && sma(20) > sma(60);
    };

    // 2단: 정배열 프리필터 — 후보를 일봉으로 검사해 정배열=Y(≥60봉)만 통과.
    //      데이터부족(신규상장 <60봉) 종목은 여기서 자동 제외(예: 365660).
    //      일봉 조회 비용을 align_probe_max로 캡(초과분은 미검사로 로깅 — 무음 절단 금지).
    //      score_top_n>0이면 통과분을 (ticker, 점수)로 모아 3단에서 랭킹·절단.
    struct Scored { std::string ticker; double score; };
    std::vector<Scored> passed;
    int probed = 0, aligned_cnt = 0, short_bars = 0, overext = 0;
    for (const auto& t : cand)
    {
        if (!market_allows(t)) continue; // 시장 risk_off 게이트(코스닥 후보는 kosdaq_pass일 때만, 일봉 프로브 비용도 아낌)
        // 스코어링 시엔 max_register 대신 align_probe_max까지 넓게 모아 랭킹(더 나은 상위 N).
        if (cfg.score_top_n <= 0 && (int)passed.size() >= cfg.max_register) break;
        if (probed >= cfg.align_probe_max)
        {
            LOG_WARN("[Main] DEVSCALE 정배열 프리필터: 프로브 상한(" +
                     std::to_string(cfg.align_probe_max) + ") 도달 — 후보 " +
                     std::to_string((int)cand.size() - probed) + "개 미검사");
            break;
        }
        auto d = c.get_daily_ohlcv(t, cfg.align_daily_n);
        ++probed;
        if ((int)d.size() < 60) { ++short_bars; continue; }
        if (!aligned_daily(d)) continue;
        // 점수: 정배열 검사에 쓴 일봉 d 재활용(추가 REST 0). d[0]=최신.
        auto sma = [&](int n) { double s = 0.0; for (int i = 0; i < n; ++i) s += d[i].close; return s / n; };
        double s5 = sma(5), s20 = sma(20), s60 = sma(60);
        double trend = s60 > 0.0 ? (s5 - s60) / s60 : 0.0;   // 추세강도(정배열 기울기)
        double px = d[0].close;
        double pull = s20 > 0.0 ? (px - s20) / s20 : 0.0;     // 눌림깊이(음수=SMA20 아래)
        // 과확장 컷 — 일봉 이격이 상한 초과면 제외(존 밴드 진입 불가한 폭등주 슬롯 낭비 방지).
        if (cfg.max_dev_pct > 0.0 && pull > cfg.max_dev_pct)
        {
            ++overext;
            continue;
        }
        double score = cfg.score_w_trend * trend + cfg.score_w_pullback * (-pull) +
                       cfg.score_w_supply * 0.0;              // 수급항은 로거 데이터 확보 후
        passed.push_back({t, score});
        ++aligned_cnt;
    }

    // 3단: 스코어 랭킹(옵션) — score_top_n>0이면 상위 N만, 아니면 통과 순서대로.
    if (cfg.score_top_n > 0)
    {
        std::sort(passed.begin(), passed.end(),
                  [](const Scored& a, const Scored& b) { return a.score > b.score; });
        int take_n = (int)passed.size() < cfg.score_top_n ? (int)passed.size() : cfg.score_top_n;
        for (int i = 0; i < take_n; ++i)
        {
            out.push_back(passed[i].ticker);
            if (out_names) (*out_names)[passed[i].ticker] = label_name(passed[i].ticker);
        }
        LOG_INFO("[Main] DEVSCALE 횡단면 스코어: 정배열통과=" + std::to_string(passed.size()) +
                 " → 상위 " + std::to_string(take_n) + " 선정 (w_trend=" +
                 std::to_string(cfg.score_w_trend) + " w_pull=" + std::to_string(cfg.score_w_pullback) +
                 " w_supply=" + std::to_string(cfg.score_w_supply) + ")");
    }
    else
    {
        for (const auto& p : passed)
        {
            out.push_back(p.ticker);
            if (out_names) (*out_names)[p.ticker] = label_name(p.ticker);
        }
    }
    LOG_INFO("[Main] DEVSCALE 정배열 프리필터: 후보=" + std::to_string(cand.size()) +
             " ETF드롭=" + std::to_string(etf_drop) +
             " 검사=" + std::to_string(probed) + " 정배열=" + std::to_string(aligned_cnt) +
             " 데이터부족(<60봉)=" + std::to_string(short_bars) +
             " 과확장컷=" + std::to_string(overext) +
             " 등록=" + std::to_string(out.size()));
    return out;
}

} // namespace universe
