#pragma once
// 매크로 국면 파일(regime.json) → 진입정지(entry_halt)·강제청산(force_liquidate)·매수비율·전략 선택 국면 판정.
//  파일 읽기·로그·OrderGate 적용은 Engine(data_thread)이 하고, 여기는 관측값과 KST 시각을 받아
//  "게이트를 어떻게 바꿀지"만 답하는 상태기계다 — 시간 상자(D-033)의 하루 리셋·1회 로그 규칙을
//  I/O 없이 시험하려고 뗐다. data_thread 전용이라 동기화는 없다. [why D-060]
#include "core/Types.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>

#include <optional>
#include <string>
#include <string_view>

// regime.json 이 이 초(seconds)보다 오래되면 보조 프로세스가 죽은 것으로 보고 신뢰하지 않는다(페일세이프).
// config "regime_stale_sec" 로 덮어쓸 수 있고, 미지정 시 이 기본값을 쓴다.
inline constexpr int kDefaultRegimeStaleSec = 600;

// 매크로 진입정지의 유효 시간(개장 후 분). 0이면 만료 없음.
//  60분 만료는 입력 5개 중 4개가 간밤 미국 종가라 장중 내내 상수였을 때의 장치였다(D-033).
//  D-083부터 코스피·코스닥·선물·유가를 Yahoo 현재가로 3분마다 받고 장초 대비 방향표도 세므로
//  회복이 관측된다 — 시계로 풀 이유가 없어져 기본을 0으로 둔다. config로 되살릴 수 있다. [why D-083]
inline constexpr int kDefaultRegimeHaltExpireMin = 0;

// entry_scale이 없거나 무효일 때의 값(= 비율 제한 없음).
inline constexpr double kRegimeScaleFull = 1.0;

namespace regime_file
{
// [wire] regime.json 본문 — PYQuant/tools/macro_regime_feed.py가 임시 파일 뒤 이름 바꾸기로 쓴다.
struct Snapshot
{
    bool        valid           = false; // false면 보조 프로세스가 데이터 부족으로 판정 보류 → 게이트 불변
    bool        entry_halt      = false;
    bool        force_liquidate = false;
    std::string regime          = "?";
    int         risk_score      = 0;
    // 매수 명목 비율(0~1). 보조 프로세스가 점수를 옮긴 값. 키가 없거나 null이면 비어 있다(→ 1.0).
    std::optional<double> entry_scale;
};

// 키가 없거나 형이 다르면 기본값. 보조 프로세스가 "true" 문자열을 쓰는 실수를 예외 대신 "없음"으로 받는다.
inline Snapshot parse_snapshot(const nlohmann::json& document)
{
    auto flag = [&document](const char* key) {
        auto iterator = document.find(key);
        return iterator != document.end() && iterator->is_boolean() && iterator->get<bool>();
    };
    Snapshot snapshot;
    snapshot.valid           = flag("valid");
    snapshot.entry_halt      = flag("entry_halt");
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

// 파일 라벨 → 전략 선택 국면. RISK_OFF는 파일 쪽에서 entry_halt와 같은 문턱(score ≤ halt)이라
//  BEAR 집합은 halt 위에 얹히는 셈이고, 실제 선택은 RISK_ON/NEUTRAL 사이에서 갈린다. 모르는 라벨·
//  UNKNOWN(판정 보류)은 UNKNOWN — 호출자는 이전 선택을 유지한다. [why D-084]
inline Regime selection_of(const std::string& label)
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

// 선택 국면 → 파일 라벨(selection_of의 역방향). 로그·일지는 파일 라벨로 적는다. 리터럴이라 수명은 정적이다. [why D-085]
inline std::string_view label_of(Regime regime)
{
    switch (regime)
    {
    case Regime::BULL:    return "RISK_ON";
    case Regime::NEUTRAL: return "NEUTRAL";
    case Regime::BEAR:    return "RISK_OFF";
    default:              return "UNKNOWN";
    }
}

enum class FileState
{
    kMissing,    // 파일 없음 → 게이트 불변
    kStale,      // 너무 오래됨 → 새 halt를 걸지 않는다(현 halt 유지)
    kUnreadable, // 열기 실패·부분 쓰기·손상 JSON → 조용히 무시
    kFresh
};

struct Observation
{
    FileState state   = FileState::kMissing;
    long long age_sec = 0; // kStale일 때 경고 문구용
    Snapshot  snapshot;        // kFresh일 때만 뜻이 있다
};

// 판정에 필요한 KST 두 값. Engine이 utc_plus_hours(9)로 채운다.
struct KstClock
{
    int yesterday               = 0;    // tm_yday — 만료 상태를 하루 단위로 되돌리는 기준
    int minutes_after_open = -540; // 09:00 기준 분(is_kr_market_open과 같은 축). 개장 전은 음수
};

// 한 번의 폴링이 바깥에 요구하는 것. 값이 없는 optional은 "그대로 둔다"는 뜻이다 —
//  파일이 없거나 stale·무효면 force_liquidate 플래그도 이전 값을 유지한다.
struct Outcome
{
    std::optional<bool> entry_halt;      // OrderGate::set_entry_halt 호출이 필요할 때만
    std::optional<bool> force_liquidate; // 파일이 신선·유효할 때만
    // OrderGate::set_entry_scale 호출이 필요할 때만(값이 바뀐 회차). halt·청산이면 0, 만료면 1.
    std::optional<double> entry_scale;
    // Engine::apply_regime_selection 호출이 필요할 때만(라벨이 바뀐 회차). stale·무효·모르는 라벨은 비어 있다.
    std::optional<Regime> selection;
    bool log_expiry          = false;    // 시간 상자 만료 — 하루 1회
    bool log_stale           = false;    // stale 진입 1회
    bool log_halt_transition = false;    // entry_halt 전이(값은 entry_halt)
    bool log_liquidation_on          = false;    // force_liquidate 켜짐 1회
    bool log_liquidation_off         = false;    // force_liquidate 꺼짐 1회
    bool log_scale_change    = false;    // entry_scale 변경(값은 entry_scale)
};

class RegimeFileJudge
{
public:
    RegimeFileJudge() = default;

    void set_stale_sec(int stale_sec)
    {
        if (stale_sec > 0)
        {
            stale_sec_ = stale_sec;
        }
    }

    void set_halt_expire_min(int halt_expire_min) { halt_expire_min_ = halt_expire_min; }
    int  stale_sec() const { return stale_sec_; }
    bool halt_on() const { return halt_on_; }
    double scale_now() const { return scale_now_; }
    Regime selection_now() const { return selection_now_; }

    Outcome step(const Observation& observation, const KstClock& clock)
    {
        Outcome out;

        // 시간 상자는 파일을 보기 전에, 한 곳에서 집행한다. 이 아래로는 halt를 건 채 빠져나가는
        //  경로가 넷이다(파일 없음·stale·읽기 실패·판정 보류). 분기마다 같은 해제를 적으면 한 곳은
        //  빠지고 그날 halt가 안 풀린다 — 09-10의 실패 모양이었다.
        //  force_liquidate 중에는 풀지 않는다(극단 위험회피를 시계로 풀지 않는다). [why D-033]
        if (halt_on_ && !liquidation_warned_ && time_box_passed(clock))
        {
            out.log_expiry  = mark_expired();
            out.entry_halt  = false;
            halt_on_        = false;
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
            out.selection  = selected_regime;
            selection_now_ = selected_regime;
        }

        const bool liquidation  = observation.snapshot.force_liquidate;
        bool       halt = observation.snapshot.entry_halt || liquidation; // 청산 중엔 신규 진입도 반드시 정지

        if (halt && !liquidation && time_box_passed(clock))
        {
            out.log_expiry = mark_expired() || out.log_expiry;
            halt           = false;
        }

        if (halt != halt_on_)
        {
            out.entry_halt          = halt;
            out.log_halt_transition = true;
            halt_on_                = halt;
        }

        // 비율은 halt·청산이면 0(파일이 뭐라 하든), 아니면 파일 값(없으면 1). 0.1 단위로 끊어 3분마다
        //  미세하게 흔들려 전략이 분할 매수를 다시 까는 일을 막는다. [why D-083]
        double scale = halt ? 0.0 : observation.snapshot.entry_scale.value_or(kRegimeScaleFull);
        scale        = std::round(scale * 10.0) / 10.0;
        set_scale(out, scale);

        if (liquidation && !liquidation_warned_)
        {
            out.log_liquidation_on = true;
            liquidation_warned_    = true;
        }

        if (!liquidation && liquidation_warned_)
        {
            out.log_liquidation_off = true;
            liquidation_warned_     = false;
        }

        out.force_liquidate = liquidation;
        return out;
    }

private:
    // 비율 전이 — 값이 바뀐 회차에만 Outcome에 싣고 로그를 켠다.
    void set_scale(Outcome& out, double scale)
    {
        if (scale == scale_now_)
        {
            return;
        }

        scale_now_           = scale;
        out.entry_scale      = scale;
        out.log_scale_change = true;
    }

    // 개장 후 N분이 지났는가. 날짜가 바뀌면 만료 상태를 되돌린다.
    bool time_box_passed(const KstClock& clock)
    {
        if (halt_expire_min_ <= 0)
        {
            return false;
        }

        if (clock.yesterday != expire_yday_)
        {
            expire_yday_ = clock.yesterday;
            expired_     = false;
        }

        // [inv] 09:00~15:30 안에서만 만료가 성립한다 — 파장 뒤·개장 전은 걸리지 않는다.
        return clock.minutes_after_open >= halt_expire_min_ && clock.minutes_after_open < 390;
    }

    // 만료 로그는 하루 한 번. 처음 만료시킨 호출만 true.
    bool mark_expired()
    {
        if (expired_)
        {
            return false;
        }

        expired_ = true;
        return true;
    }

    int  stale_sec_       = kDefaultRegimeStaleSec;
    int  halt_expire_min_ = kDefaultRegimeHaltExpireMin;
    bool halt_on_         = false; // 우리가 현재 건 halt(전이 시에만 set·로그)
    double scale_now_     = kRegimeScaleFull; // 우리가 현재 건 비율(전이 시에만 set·로그)
    Regime selection_now_ = Regime::UNKNOWN;  // 마지막으로 바깥에 실은 전략 선택 국면(전이 시에만)
    bool stale_warned_    = false;
    int  expire_yday_     = -1;
    bool expired_         = false; // 오늘 이미 만료시켰나(로그 1회화 겸용)
    bool liquidation_warned_      = false; // force_liquidate 전이 로그 1회화
};
} // namespace regime_file
