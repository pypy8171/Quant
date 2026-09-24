// 국면 선택 — regime.json 을 읽어 지금 국면을 정하고, 그 국면에 맞는 전략만 켠다.
//  Engine 클래스는 그대로다. Engine.cpp 가 4,600줄을 넘겨 열기 어려워져 이 갈래만 따로 낸 것이다
//  (헤더는 한 줄도 안 바뀐다 — 같은 Engine 의 멤버 함수 본체가 여기 있을 뿐이다).
//
//  ── 부르는 자리 ──────────────────────────────────────────────────────────
//  poll_regime_file()  : data_thread 가 매 사이클
//  apply_regime_selection() : poll_regime_file() 과 전략 적재가 부른다
//  set_regime_file()   : 기동 설정

#include "core/Engine.h"
#include "utils/Logger.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string_view>
#include <nlohmann/json.hpp>

// G1: 국면 r에 맞춰 전략 활성셋을 재선택한다.
//  strategy_.has_regime_map이면 국면별 id 목록이 권위적 선택자('*' 접두 매칭으로 스캐너 동적 id 포함),
//  아니면 기존 per-strategy active_regimes 폴백. 선택 결정(활성/비활성 목록)은 국면 변화 또는
//  force_log 시 [RegimeSelect]로 기록 → "왜 이 전략을 켰나"가 로그에 남는다.
//
//  ── 입력은 regime.json 라벨 하나다(D-084) ────────────────────────────────
//  poll_regime_file()이 RISK_ON→BULL·NEUTRAL·RISK_OFF→BEAR로 옮겨 라벨이 바뀐 회차에 부른다.
//  같은 파일이 entry_halt·매수비율·강제청산도 내므로 "무엇을 살까"와 "지금 사도 되나"가 한 입력에서
//  나온다. 코스피 200MA·정배열로 따로 판정하던 축은 이 스위치 말고 하는 일이 없어 지웠다. [why D-084][why D-085]
void Engine::apply_regime_selection(Regime regime, bool force_log)
{
    // id 매칭: 목록 항목이 '*'로 끝나면 접두 매칭, 아니면 정확히 일치.
    auto matches = [](const std::string& id, const std::vector<std::string>& selected)
    {
        for (const auto& selected : selected)
        {
            if (!selected.empty() && selected.back() == '*')
            {
                if (id.starts_with(std::string_view(selected).substr(0, selected.size() - 1)))
                {
                    return true;
                }
            }
            else if (id == selected)
            {
                return true;
            }
        }

        return false;
    };

    const std::vector<std::string>* selected = nullptr;

    if (strategy_.has_regime_map)
    {
        auto iterator = strategy_.regime_map.find(regime);

        if (iterator != strategy_.regime_map.end())
        {
            selected = &iterator->second; // 없는 국면 키 = 아무 전략도 활성 안 함(전량 비활성)
        }
    }

    auto append = [](std::string& csv, const std::string& id)
    {
        if (!csv.empty())
        {
            csv += ", ";
        }

        csv += id;
    };

    std::string active_ids, inactive_ids;

    for (auto& strategy : strategy_.list)
    {
        bool on;

        if (strategy_.has_regime_map)
        {
            on = selected && matches(strategy->id(), *selected);
        }
        else
        {
            // per-strategy 폴백도 같은 입력(r)으로 판정한다. [why D-084]
            const auto& active_regimes = strategy->active_regimes();
            on            = std::find(active_regimes.begin(), active_regimes.end(), regime) != active_regimes.end();
        }

        strategy->set_active(on);
        append(on ? active_ids : inactive_ids, strategy->id());
    }

#ifdef HAS_ZMQ
    if (zmq_bridge_)
    {
        // FILL 페이로드가 그때그때 이 라벨을 실어 DB의 regime 열을 채운다(주문 시점이 아니라
        // publish 시점 기준 — 국면 전환 중 걸친 체결은 오차가 있을 수 있으나 근사로 충분).
        zmq_bridge_->set_regime(regime);
    }
#endif

    if (force_log || regime != strategy_.last_selected_regime)
    {
        // 국면은 regime.json 라벨로 적는다(RISK_ON·NEUTRAL·RISK_OFF) — 매매일지·대시보드가 이 값을 읽는다. [why D-085]
        const std::string line = std::string("[RegimeSelect] 국면=").append(regime_file::label_of(regime)) + " → 활성=[" + active_ids +
                                 "] 비활성=[" + inactive_ids + "]" +
                                 (strategy_.has_regime_map ? "" : " (per-strategy 폴백)");

        // 맵이 있는데 하나도 안 켜지면 오타(접두어·대소문자)나 빈 항목일 수 있어 WARN으로 올린다.
        if (strategy_.has_regime_map && active_ids.empty() && !inactive_ids.empty())
        {
            LOG_WARN(line + " — 맵이 등록 전략과 하나도 안 맞음(신규 진입 전면 차단 상태)");
        }
        else
        {
            LOG_INFO(line);
        }
    }

    strategy_.last_selected_regime = regime;
}

// 파일 관측만 한다(존재·나이·파싱). 판정은 RegimeFileJudge::step이 하고, 여기서는 그 결과를
//  OrderGate·force_liquidate_에 옮기고 로그 문구를 붙인다. 원자적 write라 정상은 완전한 json이고
//  부분/손상은 kUnreadable로 조용히 넘긴다.
static regime_file::Observation observe_regime_file(const std::string& path, int stale_sec)
{
    regime_file::Observation observation;
    std::error_code error_code;

    if (!std::filesystem::exists(path, error_code) || error_code)
    {
        return observation; // kMissing
    }

    // 갱신 지연: 보조 프로세스가 죽어 파일이 오래되면 신뢰 불가. 수정 시각을 못 읽으면 나이를 모르니 갱신된 것으로 본다.
    auto modified_time = std::filesystem::last_write_time(path, error_code);

    if (!error_code)
    {
        observation.age_sec = std::chrono::duration_cast<std::chrono::seconds>(
                        std::filesystem::file_time_type::clock::now() - modified_time).count();

        if (observation.age_sec > stale_sec)
        {
            observation.state = regime_file::FileState::kStale;
            return observation;
        }
    }

    observation.state = regime_file::FileState::kUnreadable;

    try
    {
        std::ifstream file(path);

        if (!file)
        {
            return observation;
        }

        nlohmann::json doc;
        file >> doc;
        observation.snapshot  = regime_file::parse_snapshot(doc);
        observation.state = regime_file::FileState::kFresh;
    }
    catch (const std::exception&)
    {
    }

    return observation;
}

void Engine::poll_regime_file()
{
    if (regime_file_.empty())
    {
        return; // 기능 미가동(기본)
    }

    const regime_file::Observation observation = observe_regime_file(regime_file_, regime_file_judge_.stale_sec());
    const struct tm kst = ::kst::to_tm(std::time(nullptr));
    // 09:00 기준 분(is_kr_market_open과 같은 눈금). 개장 전은 음수라 안 걸린다.
    const regime_file::KstClock clock{kst.tm_yday, ::kst::minute_of_day(kst) - ::kst::kKrMarketOpenMinute};
    const regime_file::Outcome  out = regime_file_judge_.step(observation, clock);

    if (out.log_expiry)
    {
        LOG_WARN("[Regime] 매크로 진입정지 만료 — 개장 후 " + std::to_string(clock.minutes_after_open) +
                 "분 경과. 이 축은 장중 갱신되지 않으므로 오늘 남은 시간의 신규진입 판단은 "
                 "유니버스 지수 게이트와 종목 정배열에 맡긴다");
    }

    if (out.log_stale)
    {
        LOG_WARN("[Regime] regime.json " + std::to_string(observation.age_sec) + "s 경과(> " +
                 std::to_string(regime_file_judge_.stale_sec()) +
                 "s) — 보조 프로세스 중단 의심, 게이트 신규 변경 보류(현 halt 유지)");
    }

    // 신규진입 정지를 내는 곳은 이 함수뿐이라 소유권이 단순하다.
    if (out.entry_halt)
    {
        request_entry_halt(*out.entry_halt);
    }

    if (out.log_halt_transition)
    {
        LOG_WARN(std::string("[Regime] 신규진입 ") +
                 (*out.entry_halt ? "정지(ENTRY_HALT ON)" : "재개(ENTRY_HALT OFF)") +
                 " — regime=" + observation.snapshot.regime + " score=" + std::to_string(observation.snapshot.risk_score));
    }

    // 전략 선택 — 라벨이 바뀐 회차에만. 같은 data_thread라 apply_regime_selection의 strategy_.list 순회와 겹치지 않는다.
    if (out.selection)
    {
        apply_regime_selection(*out.selection, /*force_log=*/false);
    }

    // 비율은 halt와 같은 소유권(이 함수만 낸다). 전략은 다음 계획 회차에 분할 단계 명목에 곱한다.
    if (out.entry_scale)
    {
        request_entry_scale(*out.entry_scale);
    }

    if (out.log_scale_change)
    {
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "%.1f", *out.entry_scale);
        LOG_INFO(std::string("[Regime] 매수비율 ") + buffer + " — regime=" + observation.snapshot.regime +
                 " score=" + std::to_string(observation.snapshot.risk_score));
    }

    // force_liquidate 배선(G3): 플래그만 세우고 실제 매도는 pipeline_.requests 단일 생산자인
    //  strategy_thread가 낸다(SPSC 준수). 여기(data_thread)는 원자 플래그 토글과 1회 로그뿐이다.
    if (out.log_liquidation_on)
    {
        LOG_ERROR("[Regime] force_liquidate=TRUE (극단 위험회피) — 보유 전량 강제청산 요청, "
                  "strategy_thread가 시장가 매도 발주");
    }

    if (out.log_liquidation_off)
    {
        LOG_WARN("[Regime] force_liquidate 해제 — 강제청산 중단");
    }

    if (out.force_liquidate)
    {
        force_liquidate_.store(*out.force_liquidate, std::memory_order_relaxed);
    }
}

void Engine::set_regime_file(const std::string& path, int stale_sec)
{
    regime_file_ = path;
    regime_file_judge_.set_stale_sec(stale_sec);
}
