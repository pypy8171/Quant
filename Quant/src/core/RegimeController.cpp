#include "core/RegimeController.h"
#include "api/IMarketDataSource.h"
#include "core/KstTime.h"
#include "utils/Logger.h"
#include <chrono>
#include <cstdio>
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
// 로그용 "+1.2%" — std::to_string(double)은 소수 6자리가 붙는다.
std::string fmt_pct(double v)
{
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%+.1f%%", v);
    return buf;
}

// time_point → "YYYYMMDD" (UTC 날짜). 지수 일봉의 timestamp는 그 날짜의 UTC 정오다(KisIndex.cpp).
std::string ymd_of(std::chrono::system_clock::time_point tp)
{
    return kst::format_ymd(std::chrono::year_month_day{std::chrono::floor<std::chrono::days>(tp)});
}

// KST 기준 오늘 "YYYYMMDD"
std::string today_kst()
{
    return kst::ymd(std::time(nullptr));
}
} // namespace

// ─── 국면 판정 ───────────────────────────────────────────────────────────────
RegimeSnapshot RegimeController::evaluate()
{
    const std::string today = today_kst();
    bool last_ok = false;   // 오늘 이미 성공한 판정이 있어야 "전환"이고, 그때만 확인 횟수를 센다
    {
        std::lock_guard<std::mutex> lk(snap_mtx_);
        last_ok = (last_.date == today && last_.index_close > 0.0);

        // fold_today가 꺼져 있으면 입력이 전일 확정봉뿐이라 장중에 상수다 — 같은 날 성공 판정은 그대로 돌려준다.
        //  실패한 판정(index_close=0)은 캐시하지 않아 다음 주기에 다시 시도한다.
        if (!cfg_.fold_today && last_.date == today && last_.index_close > 0.0)
        {
            return last_;
        }
    }

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
            {
                out = Regime::NEUTRAL;   // 첫 실패 등 직전 국면 없음 → NEUTRAL 안전판 (W1)
            }

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

    if (!source_)
    {
        return on_fail("시세 소스 없음");
    }

    // 확정 일봉은 하루 한 번만 받는다 — 1회 조회가 GET 5회(날짜창을 밀며 누적)라 재평가마다 받으면 그만큼이 그냥 나간다.
    //  장중에 바뀌는 건 오늘 봉 하나뿐이고 그건 아래서 지수 현재값으로 접는다.
    if (day_bars_date_ != today || day_bars_.empty())
    {
        auto bars = source_->get_index_daily_ohlcv(cfg_.index_code, cfg_.ma_long + 10);

        if (bars.empty())
        {
            return on_fail("지수 일봉 응답 없음");
        }

        day_bars_      = std::move(bars);
        day_bars_date_ = today;
    }

    const auto& bars = day_bars_;

    // 당일봉(bars[0])은 REST 일봉 응답의 미완성 값을 쓰지 않는다. fold_today면 지수 현재값으로 오늘 봉을 만들어
    //  맨 앞에 두고, 아니면 전일 확정봉까지만 본다 [why D-076].
    const int start = (ymd_of(bars[0].timestamp) == today) ? 1 : 0;
    std::vector<double> closes;   // [0]=가장 최근(오늘 또는 전일) → 과거
    closes.reserve(bars.size() + 1);

    if (cfg_.fold_today)
    {
        const IndexPrice ip = source_->get_index_price(cfg_.index_code);

        if (ip.price <= 0.0)
        {
            return on_fail("지수 현재값 없음");
        }

        closes.push_back(ip.price);
    }

    for (std::size_t i = static_cast<std::size_t>(start); i < bars.size(); ++i)
    {
        closes.push_back(bars[i].close);
    }

    if (static_cast<int>(closes.size()) < cfg_.ma_long)
    {
        day_bars_.clear();   // 짧은 응답은 캐시하지 않는다 — 다음 주기에 다시 받는다
        return on_fail("확정봉 부족 " + std::to_string(closes.size()) + "/" + std::to_string(cfg_.ma_long));
    }

    auto sma = [&](int n) -> double {
        double sum = 0.0;

        for (int i = 0; i < n; ++i)
        {
            sum += closes[static_cast<std::size_t>(i)];
        }

        return sum / n;
    };

    RegimeSnapshot s;
    s.date        = today;
    s.index_close = closes[0];                  // fold_today면 지수 현재값, 아니면 가장 최근 확정 종가
    s.ma200 = sma(cfg_.ma_long);
    s.ma20  = sma(cfg_.ma_short);
    s.ma60  = sma(cfg_.ma_mid);
    s.ma120 = sma(cfg_.ma_align3);

    if (s.index_close <= 0.0 || s.ma200 <= 0.0 ||  // 과도기 비정상값 방어 (W3/S-1)
        s.ma20 <= 0.0 || s.ma60 <= 0.0 || s.ma120 <= 0.0)
    {
        return on_fail("지수 종가/MA 비정상값(close=" + std::to_string(static_cast<int>(s.index_close)) + ")");
    }

    s.above_ma200  = s.index_close > s.ma200;
    s.aligned_bull = (s.ma20 > s.ma60) && (s.ma60 > s.ma120);
    s.aligned_bear = (s.ma20 < s.ma60) && (s.ma60 < s.ma120);
    s.score  = compute_score(s, cfg_);
    s.regime = classify(s.score, cfg_);
    s.timestamp = std::chrono::system_clock::now();
    fail_streak_ = 0;

    // 당일 급락 강제 BEAR. closes[1]은 전일 확정 종가(당일접음일 때). 들어갈 때 −bear, 나올 때 −release로
    //  되돌림이 그 사이에 머물면 BEAR를 유지한다 — 문턱 하나면 −2.0% 언저리에서 주기마다 뒤집힌다. [why D-083]
    if (cfg_.fold_today && cfg_.day_drop_bear_pct > 0.0 && closes.size() >= 2 && closes[1] > 0.0)
    {
        s.day_pct = (closes[0] / closes[1] - 1.0) * 100.0;

        if (day_drop_on_ && s.day_pct > -cfg_.day_drop_release_pct)
        {
            day_drop_on_ = false;
        }
        else if (!day_drop_on_ && s.day_pct <= -cfg_.day_drop_bear_pct)
        {
            day_drop_on_ = true;
        }

        if (day_drop_on_)
        {
            s.day_drop = true;
            s.regime   = Regime::BEAR;
        }
    }
    else
    {
        day_drop_on_ = false;
    }

    // 장중 전환 확인. 오늘 봉을 접으면 지수가 이평 근처에서 흔들릴 때 판정이 주기마다 뒤집힐 수 있다 —
    //  다른 국면이 confirm_n회 연속일 때만 바꾼다. 그날 첫 성공 판정(실패 뒤 복구 포함)과 fold_today가 꺼진 경우는
    //  바로 확정한다.
    const Regime cur = current_.load();
    const Regime raw = s.regime;

    if (cfg_.fold_today && last_ok && cur != Regime::UNKNOWN && raw != cur)
    {
        if (raw == pending_)
        {
            ++pending_n_;
        }
        else
        {
            pending_   = raw;
            pending_n_ = 1;
        }

        if (pending_n_ < cfg_.confirm_n)
        {
            s.regime = cur;   // 보류 — 점수는 그대로 남기고 국면만 직전 값
            LOG_INFO("[Regime] 전환 보류 " + to_string(cur) + "→" + to_string(raw) + " (" +
                     std::to_string(pending_n_) + "/" + std::to_string(cfg_.confirm_n) + ", score=" +
                     std::to_string(s.score) + " close=" + std::to_string(static_cast<int>(s.index_close)) + ")");
        }
        else
        {
            pending_   = Regime::UNKNOWN;
            pending_n_ = 0;
        }
    }
    else
    {
        pending_   = Regime::UNKNOWN;
        pending_n_ = 0;
    }

    current_.store(s.regime);
    {
        std::lock_guard<std::mutex> lk(snap_mtx_);
        last_ = s;
    }

    if (s.regime == raw)
    {
        LOG_INFO("[Regime] " + to_string(s.regime) + " score=" + std::to_string(s.score) +
                 " (close=" + std::to_string(static_cast<int>(s.index_close)) +
                 (cfg_.fold_today ? " 당일접음" : " 전일확정") +
                 " ma200=" + std::to_string(static_cast<int>(s.ma200)) +
                 " above200=" + (s.above_ma200 ? "Y" : "N") +
                 " 정배열=" + (s.aligned_bull ? "Y" : (s.aligned_bear ? "역배열" : "혼조")) +
                 (cfg_.fold_today ? " 당일=" + fmt_pct(s.day_pct) : "") +
                 (s.day_drop ? " 급락강제BEAR" : "") + ")");
    }

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
    {
        if (r == cur)
        {
            return true;
        }
    }

    return false;
}
