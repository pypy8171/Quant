#pragma once
// 매크로 국면 파일(regime.json) → 진입정지(entry_halt)·강제청산(force_liquidate) 판정.
//  파일 읽기·로그·OrderGate 적용은 Engine(data_thread)이 하고, 여기는 관측값과 KST 시각을 받아
//  "게이트를 어떻게 바꿀지"만 답하는 상태기계다 — 시간 상자(D-033)의 하루 리셋·1회 로그 규칙을
//  I/O 없이 시험하려고 뗐다. data_thread 전용이라 동기화는 없다. [why D-060]
#include <nlohmann/json.hpp>

#include <optional>
#include <string>

// regime.json 이 이 초(sec)보다 오래되면 보조 프로세스가 죽은 것으로 보고 신뢰하지 않는다(페일세이프).
// config "regime_stale_sec" 로 덮어쓸 수 있고, 미지정 시 이 기본값을 쓴다.
inline constexpr int kDefaultRegimeStaleSec = 600;

// 매크로 진입정지의 유효 시간(개장 후 분). 0이면 만료 없음(옛 동작).
//  이 축의 입력 5개 중 4개가 간밤 미국 종가라 KST 장중 내내 상수다 — 회복을 관측할 수
//  없는 신호에 장중 거부권을 계속 주지 않는다. [why D-033]
inline constexpr int kDefaultRegimeHaltExpireMin = 60;

namespace regime_bridge
{
// [wire] regime.json 본문 — PYQuant/tools/macro_regime_feed.py가 임시 파일 뒤 이름 바꾸기로 쓴다.
struct Snapshot
{
    bool        valid           = false; // false면 보조 프로세스가 데이터 부족으로 판정 보류 → 게이트 불변
    bool        entry_halt      = false;
    bool        force_liquidate = false;
    std::string regime          = "?";
    int         risk_score      = 0;
};

// 키가 없거나 형이 다르면 기본값. 보조 프로세스가 "true" 문자열을 쓰는 실수를 예외 대신 "없음"으로 받는다.
inline Snapshot parse_snapshot(const nlohmann::json& j)
{
    auto flag = [&j](const char* key) {
        auto it = j.find(key);
        return it != j.end() && it->is_boolean() && it->get<bool>();
    };
    Snapshot s;
    s.valid           = flag("valid");
    s.entry_halt      = flag("entry_halt");
    s.force_liquidate = flag("force_liquidate");
    auto r = j.find("regime");

    if (r != j.end() && r->is_string())
    {
        s.regime = r->get<std::string>();
    }

    auto sc = j.find("risk_score");

    if (sc != j.end() && sc->is_number())
    {
        s.risk_score = sc->get<int>();
    }

    return s;
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
    Snapshot  snap;        // kFresh일 때만 뜻이 있다
};

// 판정에 필요한 KST 두 값. Engine이 utc_plus_hours(9)로 채운다.
struct KstClock
{
    int yday               = 0;    // tm_yday — 만료 상태를 하루 단위로 되돌리는 기준
    int minutes_after_open = -540; // 09:00 기준 분(is_kr_market_open과 같은 축). 개장 전은 음수
};

// 한 번의 폴링이 바깥에 요구하는 것. 값이 없는 optional은 "그대로 둔다"는 뜻이다 —
//  파일이 없거나 stale·무효면 force_liquidate 플래그도 이전 값을 유지한다.
struct Outcome
{
    std::optional<bool> entry_halt;      // OrderGate::set_entry_halt 호출이 필요할 때만
    std::optional<bool> force_liquidate; // 파일이 신선·유효할 때만
    bool log_expiry          = false;    // 시간 상자 만료 — 하루 1회
    bool log_stale           = false;    // stale 진입 1회
    bool log_halt_transition = false;    // entry_halt 전이(값은 entry_halt)
    bool log_liq_on          = false;    // force_liquidate 켜짐 1회
    bool log_liq_off         = false;    // force_liquidate 꺼짐 1회
};

class RegimeFileBridge
{
public:
    RegimeFileBridge() = default;

    void set_stale_sec(int s)
    {
        if (s > 0)
        {
            stale_sec_ = s;
        }
    }

    void set_halt_expire_min(int m) { halt_expire_min_ = m; }
    int  stale_sec() const { return stale_sec_; }
    bool halt_on() const { return halt_on_; }

    Outcome step(const Observation& o, const KstClock& clk)
    {
        Outcome out;

        // 시간 상자는 파일을 보기 전에, 한 곳에서 집행한다. 이 아래로는 halt를 건 채 빠져나가는
        //  경로가 넷이다(파일 없음·stale·읽기 실패·판정 보류). 분기마다 같은 해제를 적으면 한 곳은
        //  빠지고 그날 halt가 안 풀린다 — 09-10의 실패 모양이었다.
        //  force_liquidate 중에는 풀지 않는다(극단 위험회피를 시계로 풀지 않는다). [why D-033]
        if (halt_on_ && !liq_warned_ && time_box_passed(clk))
        {
            out.log_expiry  = mark_expired();
            out.entry_halt  = false;
            halt_on_        = false;
        }

        switch (o.state)
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

        if (!o.snap.valid)
        {
            return out;
        }

        const bool liq  = o.snap.force_liquidate;
        bool       halt = o.snap.entry_halt || liq; // 청산 중엔 신규 진입도 반드시 정지

        if (halt && !liq && time_box_passed(clk))
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

        if (liq && !liq_warned_)
        {
            out.log_liq_on = true;
            liq_warned_    = true;
        }

        if (!liq && liq_warned_)
        {
            out.log_liq_off = true;
            liq_warned_     = false;
        }

        out.force_liquidate = liq;
        return out;
    }

private:
    // 개장 후 N분이 지났는가. 날짜가 바뀌면 만료 상태를 되돌린다.
    bool time_box_passed(const KstClock& clk)
    {
        if (halt_expire_min_ <= 0)
        {
            return false;
        }

        if (clk.yday != expire_yday_)
        {
            expire_yday_ = clk.yday;
            expired_     = false;
        }

        // [inv] 09:00~15:30 안에서만 만료가 성립한다 — 파장 뒤·개장 전은 걸리지 않는다.
        return clk.minutes_after_open >= halt_expire_min_ && clk.minutes_after_open < 390;
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
    bool stale_warned_    = false;
    int  expire_yday_     = -1;
    bool expired_         = false; // 오늘 이미 만료시켰나(로그 1회화 겸용)
    bool liq_warned_      = false; // force_liquidate 전이 로그 1회화
};
} // namespace regime_bridge
