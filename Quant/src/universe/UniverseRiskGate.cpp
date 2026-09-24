// 시장 위험회피 게이트 — 코스피·코스닥 지수 등락률을 래치(히스테리시스·최소 체류)에 넣어 시장별 신규 진입을
//  열지 닫을지 정한다. 스캔의 첫 단계라 닫히면 뒤 단계(후보 수집·일봉 조회)를 통째로 건너뛴다.
//  스캔 스레드 전용. [why D-033]

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

namespace universe::detail
{
namespace
{
IdxGateLatch g_kospi_latch;
IdxGateLatch g_kosdaq_latch;
} // namespace

std::mutex   g_index_latch_mutex;

bool latch_risk_off(IdxGateLatch& latch, double change, bool observed, double trip, double resume,
                    int dwell_sec, const char* label)
{
    // 관측 실패에는 판정하지 않는다. KisClient::get_index_price는 응답 파싱이 어긋나면
    //  (EGW00201 초당한도·HTTP 오류) 로그 없이 change_rate=0.0을 돌려준다. 0.0은 언제나
    //  "재개" 쪽으로만 틀리고, 그 오판이 래치를 풀면 체류가 그 상태를 dwell초 고정한다.
    //  지수 급락 구간은 초당한도가 가장 잘 터지는 구간이라 이 오판과 상관이 있다.
    if (!observed)
    {
        return latch.risk_off;
    }

    // resume이 trip보다 낮으면 히스테리시스가 뒤집힌다 — 설정 실수는 옛 동작(단일 임계)으로 접는다.
    if (resume < trip)
    {
        if (!latch.config_warned)
        {
            LOG_WARN(std::string("[Universe] ") + label + " 지수 게이트 재개 임계가 차단 임계보다 낮다"
                     " — 히스테리시스를 끄고 단일 임계로 판정한다 (차단 " +
                     std::to_string(trip * 100.0) + "% / 재개 " + std::to_string(resume * 100.0) + "%)");
            latch.config_warned = true;
        }

        resume = trip;
    }
    else if (latch.primed && !latch.config_warned &&
             (latch.seen_trip != trip || latch.seen_resume != resume))
    {
        // 래치는 프로세스 전역이고 슬리브마다 config가 따로 온다. 임계가 갈리면 먼저 발화한 쪽
        //  판정이 나머지 슬리브에도 그대로 걸린다는 뜻이라 한 번 알린다.
        LOG_WARN(std::string("[Universe] ") + label + " 지수 게이트 임계가 슬리브마다 다르다"
                 " — 래치는 전역이라 먼저 발화한 판정이 공유된다");
        latch.config_warned = true;
    }

    latch.seen_trip   = trip;
    latch.seen_resume = resume;

    bool want_off = latch.risk_off;

    if (change < trip)
    {
        want_off = true;
    }
    else if (change >= resume)
    {
        want_off = false;
    }

    if (want_off == latch.risk_off)
    {
        return latch.risk_off;
    }

    const auto now = std::chrono::steady_clock::now();

    // 체류는 재개 방향에만 건다. 08-21의 문제는 재개 쪽 떨림이었지 차단 지연이 아니었고,
    //  안전 게이트는 닫는 쪽이 언제나 즉시여야 한다.
    if (dwell_sec > 0 && latch.primed && !want_off)
    {
        const auto held =
            std::chrono::duration_cast<std::chrono::seconds>(now - latch.since).count();

        if (held < static_cast<long long>(dwell_sec))
        {
            return latch.risk_off;   // 체류 미달 — 이번 재스캔은 직전 상태를 그대로 쓴다
        }
    }

    latch.risk_off    = want_off;
    latch.primed = true;
    latch.since  = now;
    LOG_WARN(std::string("[Universe] ") + label + " 지수 게이트 " + (want_off ? "차단" : "재개") +
             " — 등락률 " + std::to_string(change * 100.0) + "%, 차단 " +
             std::to_string(trip * 100.0) + "% / 재개 " + std::to_string(resume * 100.0) + "%");
    return latch.risk_off;
}

bool MarketGate::allows(Market market) const
{
    if (market == Market::Kosdaq || market == Market::Unknown)
    {
        return kosdaq_pass;
    }

    return kospi_pass;
}

MarketGate build_market_gate(KisClient& kis, const DevScanCfg& config)
{
    MarketGate market_gate;
    // [wire] 조회가 어긋나면 KisClient가 로그 없이 IndexPrice{}를 돌려준다 — price>0이 관측
    //  성공의 유일한 표식이다. 지수 평보합도 price는 양수라 오탐이 없다.
    const auto kospi = kis.get_index_price("0001");
    market_gate.kospi_observation = kospi.price > 0.0;
    market_gate.kospi_change = kospi.change_rate / 100.0;   // [wire] KIS는 % 단위

    if (config.kosdaq_enabled)
    {
        const auto kosdaq = kis.get_index_price("1001");   // [wire] 코스닥 종합지수
        market_gate.kosdaq_observation = kosdaq.price > 0.0;
        market_gate.kosdaq_change = kosdaq.change_rate / 100.0;
    }

    std::lock_guard<std::mutex> lock(g_index_latch_mutex);
    const bool kospi_off = latch_risk_off(g_kospi_latch, market_gate.kospi_change, market_gate.kospi_observation,
                                          config.risk_off_index, config.risk_off_index_resume,
                                          config.risk_off_dwell_sec, "코스피");
    bool kosdaq_off = false;

    if (config.kosdaq_enabled)
    {
        kosdaq_off = latch_risk_off(g_kosdaq_latch, market_gate.kosdaq_change, market_gate.kosdaq_observation,
                                    config.risk_off_index_kosdaq, config.risk_off_index_kosdaq_resume,
                                    config.risk_off_dwell_sec, "코스닥");
    }

    market_gate.kospi_pass  = !kospi_off;
    market_gate.kosdaq_pass = config.kosdaq_enabled && !kospi_off && !kosdaq_off;
    return market_gate;
}
} // namespace universe::detail
