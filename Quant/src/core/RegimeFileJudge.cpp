#include "core/RegimeFileJudge.h"

namespace regime_file
{
Snapshot parse_snapshot(const nlohmann::json& document)
{
    auto flag = [&document](const char* key)
    {
        auto iterator = document.find(key);
        return iterator != document.end() && iterator->is_boolean() && iterator->get<bool>();
    };
    Snapshot snapshot;
    snapshot.valid = flag("valid");
    snapshot.entry_halt = flag("entry_halt");
    snapshot.force_liquidate = flag("force_liquidate");
    auto found = document.find("regime");

    if (found != document.end() && found->is_string())
    {
        snapshot.regime = found->get<std::string>();
    }

    auto score_node = document.find("risk_score");

    if (score_node != document.end() && score_node->is_number())
    {
        snapshot.risk_score = score_node->get<int>();
    }

    auto entry_scale_node = document.find("entry_scale");

    if (entry_scale_node != document.end() && entry_scale_node->is_number())
    {
        snapshot.entry_scale = std::clamp(entry_scale_node->get<double>(), 0.0, 1.0);
    }

    return snapshot;
}

Regime selection_of(const std::string& label)
{
    if (label == "RISK_ON")
    {
        return Regime::BULL;
    }

    if (label == "NEUTRAL")
    {
        return Regime::NEUTRAL;
    }

    if (label == "RISK_OFF")
    {
        return Regime::BEAR;
    }

    return Regime::UNKNOWN;
}

std::string_view label_of(Regime regime)
{
    switch (regime)
    {
    case Regime::BULL:
        return "RISK_ON";
    case Regime::NEUTRAL:
        return "NEUTRAL";
    case Regime::BEAR:
        return "RISK_OFF";
    default:
        return "UNKNOWN";
    }
}

void RegimeFileJudge::set_stale_sec(int stale_sec)
{
    if (stale_sec > 0)
    {
        stale_sec_ = stale_sec;
    }
}

Outcome RegimeFileJudge::step(const Observation& observation, const KstClock& clock)
{
    Outcome out;

    // 시간 상자는 파일을 보기 전에, 한 곳에서 집행한다. 이 아래로는 halt를 건 채 빠져나가는
    //  경로가 넷이다(파일 없음·stale·읽기 실패·판정 보류). 분기마다 같은 해제를 적으면 한 곳은
    //  빠지고 그날 halt가 안 풀린다 — 09-10의 실패 모양이었다.
    //  force_liquidate 중에는 풀지 않는다(극단 위험회피를 시계로 풀지 않는다). [why D-033]
    if (halt_on_ && !liquidation_warned_ && time_box_passed(clock))
    {
        out.log_expiry = mark_expired();
        out.entry_halt = false;
        halt_on_ = false;
        set_scale(out, kRegimeScaleFull);
    }

    switch (observation.state)
    {
    case FileState::kMissing:
    case FileState::kUnreadable:
        return out;
    case FileState::kStale:
        if (!stale_warned_)
        {
            out.log_stale = true;
            stale_warned_ = true;
        }

        return out;
    case FileState::kFresh:
        stale_warned_ = false;
        break;
    }

    if (!observation.snapshot.valid)
    {
        return out;
    }

    // 전략 선택 축 — 라벨이 바뀐 회차에만 싣는다. 모르는 라벨은 이전 선택 유지. [why D-084]
    const Regime selected_regime = selection_of(observation.snapshot.regime);

    if (selected_regime != Regime::UNKNOWN && selected_regime != selection_now_)
    {
        out.selection = selected_regime;
        selection_now_ = selected_regime;
    }

    const bool liquidation = observation.snapshot.force_liquidate;
    bool halt = observation.snapshot.entry_halt || liquidation; // 청산 중엔 신규 진입도 반드시 정지

    if (halt && !liquidation && time_box_passed(clock))
    {
        out.log_expiry = mark_expired() || out.log_expiry;
        halt = false;
    }

    if (halt != halt_on_)
    {
        out.entry_halt = halt;
        out.log_halt_transition = true;
        halt_on_ = halt;
    }

    // 비율은 halt·청산이면 0(파일이 뭐라 하든), 아니면 파일 값(없으면 1). 0.1 단위로 끊어 3분마다
    //  미세하게 흔들려 전략이 분할 매수를 다시 까는 일을 막는다. [why D-083]
    double scale = halt ? 0.0 : observation.snapshot.entry_scale.value_or(kRegimeScaleFull);
    scale = std::round(scale * 10.0) / 10.0;
    set_scale(out, scale);

    if (liquidation && !liquidation_warned_)
    {
        out.log_liquidation_on = true;
        liquidation_warned_ = true;
    }

    if (!liquidation && liquidation_warned_)
    {
        out.log_liquidation_off = true;
        liquidation_warned_ = false;
    }

    out.force_liquidate = liquidation;
    return out;
}

void RegimeFileJudge::set_scale(Outcome& out, double scale)
{
    if (scale == scale_now_)
    {
        return;
    }

    scale_now_ = scale;
    out.entry_scale = scale;
    out.log_scale_change = true;
}

bool RegimeFileJudge::time_box_passed(const KstClock& clock)
{
    if (halt_expire_min_ <= 0)
    {
        return false;
    }

    if (clock.yesterday != expire_yday_)
    {
        expire_yday_ = clock.yesterday;
        expired_ = false;
    }

    // [inv] 09:00~15:30 안에서만 만료가 성립한다 — 파장 뒤·개장 전은 걸리지 않는다.
    return clock.minutes_after_open >= halt_expire_min_ && clock.minutes_after_open < kst::kKrRegularSessionMinutes;
}

bool RegimeFileJudge::mark_expired()
{
    if (expired_)
    {
        return false;
    }

    expired_ = true;
    return true;
}

} // namespace regime_file
