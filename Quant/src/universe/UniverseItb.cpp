// ITB(장중 돌파) 유니버스 — 코스피 지수 게이트를 통과하면 KIS 등락률 순위에서 후보를 고른다. DevScale 스캔과
//  래치 규칙은 같이 쓰되 래치 객체는 따로 둔다. 기동 시 한 번 부른다. [why D-033]

#include "detail/Pipeline.h"
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
#include <vector>

namespace universe
{
using namespace detail;

namespace
{
IdxGateLatch g_itb_kospi_latch;   // ITB 슬리브 전용 — 임계가 DevScale과 달라 래치를 나눈다
} // namespace

std::vector<ItbCandidate> scan_itb(KisClient& scan_kis, const ItbScanCfg& config)
{
    std::vector<ItbCandidate> out;

    // 레짐 게이트: 코스피(0001) 당일 등락률이 risk_off 이하면 신규매수 유니버스 전면 스킵.
    //  판정은 DevScale과 같은 래치(latch_risk_off)로 하되 래치 객체는 따로 둔다 — 임계가
    //  다른 슬리브가 한 래치를 나눠 쓰면 먼저 발화한 쪽 판정이 다른 쪽에 걸린다. 지금은 기동 시
    //  1회 호출이라 체류·재개가 작동할 일이 없고, 재스캔 잡이 붙는 날 그대로 살아난다. [why D-033]
    auto kospi = scan_kis.get_index_price("0001");
    double index_change = kospi.change_rate / 100.0; // KIS는 % 단위

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
        std::lock_guard<std::mutex> lock(g_index_latch_mutex);
        risk_off = latch_risk_off(g_itb_kospi_latch, index_change, /*observed=*/true, config.risk_off_index,
                                  config.risk_off_index_resume, config.risk_off_dwell_sec, "코스피(ITB)");
    }

    if (risk_off)
    {
        LOG_WARN("[Main] universe_from_scan: 레짐 위험회피(코스피 " +
                 std::to_string(kospi.change_rate) + "%, 차단 " +
                 std::to_string(config.risk_off_index * 100.0) + "% / 재개 " +
                 std::to_string(config.risk_off_index_resume * 100.0) + "%) — 신규매수 유니버스 미등록");
        return out;
    }

    auto rank = scan_kis.fetch_value_ranking(config.scan_top_n, "J");
    int added = 0;

    for (auto& ranked : rank)
    {
        if (added >= config.max_register)
        {
            break;
        }

        double change = ranked.change_rate / 100.0; // % → 비율

        // 필터①: 등락률 밴드(강세 모멘텀, 급등 추격 배제)
        if (change < config.change_min || change > config.change_max)
        {
            continue;
        }

        // 필터②: 최소가(동전주·호가스프레드 배제)
        if (ranked.price < config.min_price)
        {
            continue;
        }

        // 필터③: 수급(option) — 외국인 T-1 확정 순매수 > 0 (후보 소수에만 조회)
        if (config.standard_deviation_filter)
        {
            auto true_range = scan_kis.get_investor_trend(ranked.ticker);

            if (true_range.foreign_net <= 0)
            {
                LOG_INFO("[Main]   - ITB 스캔 제외 " + ranked.ticker + " 외국인순매수<=0");
                continue;
            }
        }

        // 통과 → 신규 진입 유니버스로 등록(당일 시가 기준점 주입).
        //  ⚠️ 기준점은 랭킹 스냅샷 현재가(r.price)가 아니라 실제 당일 시가여야 함.
        //  갭업일엔 스냅샷=장중 고점 근처라 기준점이 고점에 고정되어 돌파 진입이 영구 차단됨.
        //  inquire-price(FHKST01010100)의 stck_oprc로 진짜 시가를 조회, 0이면 r.price 폴백.
        double day_open = scan_kis.get_fundamentals(ranked.ticker).open;

        if (day_open <= 0.0)
        {
            day_open = ranked.price;
        }

        LOG_INFO("[Main]   + ITB 스캔 " + ranked.ticker + " " + ranked.name + " (등락 " +
                 std::to_string(ranked.change_rate) + "% 가격 " +
                 std::to_string(static_cast<long long>(ranked.price)) + " 시가 기준점 " +
                 std::to_string(static_cast<long long>(day_open)) + " 거래대금 " +
                 std::to_string(static_cast<long long>(ranked.trade_value)) + ")");
        out.push_back({std::move(ranked.ticker), std::move(ranked.name), day_open});   // rank는 여기서 끝난다
        ++added;
    }

    LOG_INFO("[Main] universe_from_scan: 후보 " + std::to_string(rank.size()) +
             "종목 중 " + std::to_string(added) + "종목 등록");
    return out;
}
} // namespace universe
