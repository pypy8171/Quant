#pragma once
#include "core/Types.h"
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

class IMarketDataSource;

// ─────────────────────────────────────────────────────────────────────────────
// RegimeController — 시장 국면(BULL/NEUTRAL/BEAR)을 지수 일봉으로 수치 판정. data_thread 단일 호출자.
//
//  v0: 가격 2축만 (코스피 지수 일봉)
//    (1) 지수 종가 > 200일선  ? +1 : -1
//    (2) 정배열(ma20>ma60>ma120) ? +1 : 역배열 -1 : 혼조 0
//    score ∈ {-2..+2} → BULL(+2) / BEAR(-2) / NEUTRAL
//
//  - fold_today(기본 on): 확정 일봉은 하루 1회 받아 두고, 재평가마다 지수 현재값을 오늘 봉으로 접어 다시 판정한다.
//    장중 전환은 다른 국면이 confirm_n회 연속일 때만. 끄면 전일 확정봉으로 하루 1회 판정(옛 동작) [why D-076].
//  - 조회 실패 시 직전 국면 유지, 연속 fail_fallback_n회 실패 시 NEUTRAL fallback.
//  - compute_score/classify는 KIS 없이 단위 테스트 가능한 순수 함수. evaluate()는 IMarketDataSource로 지수
//    일봉을 받으므로 가짜 소스로 시험한다(D-066).
// ─────────────────────────────────────────────────────────────────────────────
class RegimeController
{
public:
    struct Config
    {
        std::string index_code = "0001";   // 코스피 종합 (지수 일봉 TR)
        int ma_long   = 200;               // 축1: 200일 이동평균(장기 추세선). 지수 종가와 비교
        // 축2 정배열/역배열 판정에 쓰는 세 이평. 이름(short/mid)이 아니라 값 20<60<120 순서로 본다.
        int ma_mid    = 60;
        int ma_short  = 20;
        int ma_align3 = 120;               // 정배열 축의 세 번째(가장 긴) 이평
        int score_bull_threshold =  2;     // v0: 2축이라 ±2
        int score_bear_threshold = -2;
        int fail_fallback_n      =  3;     // 연속 N회 조회실패 → NEUTRAL fallback
        bool fold_today          = true;   // 지수 현재값을 오늘 봉으로 접어 장중 재판정. false면 전일 확정봉·하루 1회
        int  confirm_n           =  2;     // 장중 국면 전환에 필요한 연속 동일 판정 횟수(재평가 주기 단위)
        // 당일 급락 강제 BEAR. 지수가 전일 종가 대비 이만큼(%) 빠지면 점수와 무관하게 BEAR, 되돌림이
        //  release 안으로 들어오면 푼다(둘 사이는 유지). 0이면 끔. 200일선·정배열은 하루 −3%를 못 본다 —
        //  09-14 코스피 −3.3%인 날 NEUTRAL이 83회 찍혔다. [why D-083]
        double day_drop_bear_pct    = 2.0;
        double day_drop_release_pct = 1.5;
    };

    // GCC: 중첩 Config의 멤버 기본값 초기화(NSDMI, Non-Static Data Member Initializer)를
    //   바깥 클래스 완성 전 default 인자(=Config())로 쓰면 거부한다.
    // default 생성자 분리 + Config 인자 생성자로 회피 (MSVC/GCC 공통 컴파일).
    RegimeController() = default;
    explicit RegimeController(Config cfg) : cfg_(cfg) {}
    // 판정 스냅샷을 뮤텍스로 지킨다 — 복사 대상이 아니다.
    RegimeController(const RegimeController&)            = delete;
    RegimeController& operator=(const RegimeController&) = delete;
    void set_source(IMarketDataSource* s) { source_ = s; } // 지수 일봉을 읽을 곳. 라이브는 KisClient

    // ⚠ 계약: evaluate()는 data_thread만 호출한다(개장 전환 1회 + regime_reeval_sec 주기). fail_streak_·
    //   pending_·day_bars_가 비원자적이라 다른 스레드가 같이 부르면 카운팅이 깨진다 (W2).
    RegimeSnapshot evaluate();                  // 산출+상태갱신+스냅샷 반환. 반환 regime은 확인을 거친 유효 국면
    Regime         current() const { return current_.load(); }
    RegimeSnapshot last_snapshot() const;
    bool           is_active_for(const std::vector<Regime>& active) const;

    // ── 순수 로직 (KIS 불필요 — 헤더 inline으로 단위 테스트가 KIS 링크 없이 가능) ──
    static int compute_score(const RegimeSnapshot& s, const Config& /*cfg*/)
    {
        int score = s.above_ma200 ? +1 : -1;            // 축1: 200일선

        if (s.aligned_bull)
        {
            score += 1;  // 축2: 정배열/역배열/혼조
        }
        else if (s.aligned_bear)
        {
            score -= 1;
        }

        return score;
    }

    static Regime classify(int score, const Config& cfg)
    {
        if (score >= cfg.score_bull_threshold)
        {
            return Regime::BULL;
        }

        if (score <= cfg.score_bear_threshold)
        {
            return Regime::BEAR;
        }

        return Regime::NEUTRAL;
    }

private:
    Config cfg_;
    IMarketDataSource* source_ = nullptr;       // non-owning (Engine 수명관리)
    std::atomic<Regime> current_{Regime::UNKNOWN};
    mutable std::mutex snap_mtx_;
    RegimeSnapshot last_;
    int fail_streak_ = 0;
    std::vector<MarketData> day_bars_;          // 오늘 받아 둔 확정 일봉(최신→과거). 키는 day_bars_date_
    std::string             day_bars_date_;
    Regime pending_   = Regime::UNKNOWN;        // 확인 대기 중인 다른 국면과 연속 횟수
    int    pending_n_ = 0;
    bool   day_drop_on_ = false;                // 당일 급락 강제 BEAR가 걸려 있나(release까지 유지)
};

std::string to_string(Regime r);
