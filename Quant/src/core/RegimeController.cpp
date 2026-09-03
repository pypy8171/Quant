#include "core/RegimeController.h"
#include "api/KisClient.h"
#include "utils/Logger.h"
#include <chrono>
#include <ctime>

// ─── Regime → 문자열 (로깅/직렬화) ──────────────────────────────────────────
std::string to_string(Regime r)
{
    switch (r)
    {
    case Regime::BULL:    return "BULL";
    case Regime::NEUTRAL: return "NEUTRAL";
    case Regime::BEAR:    return "BEAR";
    default:              return "UNKNOWN";
    }
}

namespace
{
constexpr int kKstOffsetSec = 9 * 3600; // KST = UTC+9

// time_point → "YYYYMMDD" (UTC 고정 — 서버 TZ 독립)
std::string ymd_of(std::chrono::system_clock::time_point tp)
{
    time_t t = std::chrono::system_clock::to_time_t(tp);
    struct tm tmv{};
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    char buf[9];
    std::strftime(buf, sizeof(buf), "%Y%m%d", &tmv);
    return std::string(buf);
}

// KST 기준 오늘 "YYYYMMDD"
std::string today_kst()
{
    time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()) + kKstOffsetSec;
    struct tm tmv{};
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    char buf[9];
    std::strftime(buf, sizeof(buf), "%Y%m%d", &tmv);
    return std::string(buf);
}
} // namespace

// ─── 장 시작 1회 국면 판정 ──────────────────────────────────────────────────
RegimeSnapshot RegimeController::evaluate()
{
    const std::string today = today_kst();

    auto on_fail = [&](const std::string& why) -> RegimeSnapshot {
        ++fail_streak_;
        Regime out;
        if (fail_streak_ >= cfg_.fail_fallback_n)
        {
            out = Regime::NEUTRAL;   // 연속 N회 실패 → NEUTRAL fallback
            LOG_WARN("[Regime] 조회 실패 " + std::to_string(fail_streak_) +
                     "회 — NEUTRAL fallback (" + why + ")");
        }
        else
        {
            out = current_.load();   // 직전 확정 국면 유지
            if (out == Regime::UNKNOWN)
                out = Regime::NEUTRAL;   // 첫 실패 등 직전 국면 없음 → NEUTRAL 안전판 (W1)
            LOG_WARN("[Regime] 조회 실패(" + std::to_string(fail_streak_) + "/" +
                     std::to_string(cfg_.fail_fallback_n) + ") — 직전 국면 유지: " +
                     to_string(out) + " (" + why + ")");
        }
        current_.store(out);
        RegimeSnapshot s;
        s.date = today;
        s.regime = out;
        s.timestamp = std::chrono::system_clock::now();
        {
            std::lock_guard<std::mutex> lk(snap_mtx_);
            last_ = s;
        }
        return s;
    };

    if (!kis_)
        return on_fail("KisClient 없음");

    // 200일선 + 당일봉 제외 버퍼 확보
    auto bars = kis_->get_index_daily_ohlcv(cfg_.index_code, cfg_.ma_long + 10);
    if (bars.empty())
        return on_fail("지수 일봉 응답 없음");

    // 당일 미완성봉(bars[0]) 제외 — 전일 확정봉 기준 (C4)
    int start = (ymd_of(bars[0].timestamp) == today) ? 1 : 0;
    int usable = static_cast<int>(bars.size()) - start;
    if (usable < cfg_.ma_long)
        return on_fail("확정봉 부족 " + std::to_string(usable) + "/" + std::to_string(cfg_.ma_long));

    auto sma = [&](int n) -> double {
        double sum = 0.0;
        for (int i = start; i < start + n; ++i) sum += bars[i].close;
        return sum / n;
    };

    RegimeSnapshot s;
    s.date        = today;
    s.index_close = bars[start].close;                  // 가장 최근 확정 종가
    s.ma200 = sma(cfg_.ma_long);
    s.ma20  = sma(cfg_.ma_short);
    s.ma60  = sma(cfg_.ma_mid);
    s.ma120 = sma(cfg_.ma_align3);
    if (s.index_close <= 0.0 || s.ma200 <= 0.0 ||  // 과도기 비정상값 방어 (W3/S-1)
        s.ma20 <= 0.0 || s.ma60 <= 0.0 || s.ma120 <= 0.0)
        return on_fail("지수 종가/MA 비정상값(close=" + std::to_string(static_cast<int>(s.index_close)) + ")");
    s.above_ma200  = s.index_close > s.ma200;
    s.aligned_bull = (s.ma20 > s.ma60) && (s.ma60 > s.ma120);
    s.aligned_bear = (s.ma20 < s.ma60) && (s.ma60 < s.ma120);
    s.score  = compute_score(s, cfg_);
    s.regime = classify(s.score, cfg_);
    s.timestamp = std::chrono::system_clock::now();

    fail_streak_ = 0;
    current_.store(s.regime);
    {
        std::lock_guard<std::mutex> lk(snap_mtx_);
        last_ = s;
    }
    LOG_INFO("[Regime] " + to_string(s.regime) + " score=" + std::to_string(s.score) +
             " (close=" + std::to_string(static_cast<int>(s.index_close)) +
             " ma200=" + std::to_string(static_cast<int>(s.ma200)) +
             " above200=" + (s.above_ma200 ? "Y" : "N") +
             " 정배열=" + (s.aligned_bull ? "Y" : (s.aligned_bear ? "역배열" : "혼조")) + ")");
    return s;
}

RegimeSnapshot RegimeController::last_snapshot() const
{
    std::lock_guard<std::mutex> lk(snap_mtx_);
    return last_;
}

bool RegimeController::is_active_for(const std::vector<Regime>& active) const
{
    Regime cur = current_.load();
    for (Regime r : active)
        if (r == cur) return true;
    return false;
}
