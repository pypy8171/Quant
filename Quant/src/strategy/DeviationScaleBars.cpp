// strategy/DeviationScaleBars.cpp — 이격 분할 전략의 봉 준비: 체결 집계기·판단 봉·프리페치(일봉·분봉·자본)·봉 시각.
//  관련 결정: D-035·D-074(자본 조회 한도)·D-069·D-072(체결 봉)·D-115(프리페치 풀).
#include "strategy/DeviationScaleStrategy.h"
#include <condition_variable>
#include <map>

namespace
{
// 하루 분 수 — 날짜+시각을 분 일련번호 하나로 접을 때 쓴다.
constexpr int kMinutesPerDay = 1440;

// 기준자본(총평가금) 캐시 — 계좌마다 한 칸. 같은 계좌의 종목 전략들이 나눠 쓴다(fetch_equity 헤더 주석).
struct EquityEntry
{
    std::string                           fetched_date;   // 값을 받은 날(YYYYMMDD)
    double                                equity = 0.0;   // 원. 0=아직 없음
    std::chrono::steady_clock::time_point retry_after{};  // 실패 쿨다운이 끝나는 시각
    bool                                  fetching = false; // [inv] true인 동안 이 계좌를 조회하는 스레드는 하나뿐이다
};

struct EquityCache
{
    std::mutex                         mutex;
    std::condition_variable            fetched; // 조회가 끝나면 기다리던 같은 계좌 스레드를 깨운다
    std::map<std::string, EquityEntry> by_account;
};

EquityCache g_equity_cache;
} // namespace

void DeviationScaleStrategy::feed_bar_aggregator(const TradeData& trade)
{
    // ── 봉 집계: 스로틀 앞이다 — 모든 틱을 먹어야 고가·저가·거래량이 맞다 ──────────
    //  REST 대체 틱(폴백·구독 상한 넘침)은 넣지 않는다. 체결량이 없어 봉이 비고, 주기가 초 단위라
    //  고저가 빠진다. 그동안은 REST 봉을 쓰고, WS 틱이 돌아오면 REST로 다시 시드해 빈 자리를 메운다.
    if (websocket_bars_)
    {
        const bool live = !poller::is_rest_tick(trade);

        if (live != websocket_live_)
        {
            websocket_live_ = live;
            reseed_pending_ = true;
            LOG_INFO("[" + id() + "] 봉 출처 전환 src=" + (live ? "ws" : "rest") +
                     (live ? " — 체결 틱을 봉으로 모은다, REST 봉은 시드" : " — REST 대체 틱, REST 봉으로 판단"));
        }

        if (live)
        {
            aggregator_.on_tick(trade);
        }
    }
}

std::optional<DeviationScaleStrategy::DecisionBars>
DeviationScaleStrategy::load_decision_bars(const TradeData& trade, double current_price)
{
    // ── 프리페치 스냅샷 스냅(일봉·자본·3분봉). 아직 준비 전이면 다음 하트비트 대기 ──
    //  무거운 REST는 공용 프리페치 풀이 미리 당겨둔다. 여기선 락을 짧게 잡고 포인터만 잡는다.
    BarSnapshot snapshot_bars;
    int bars_bucket = -1;
    uint64_t bars_version = 0;
    {
        std::lock_guard<std::mutex> lock(snap_mutex_);

        if (!snapshot_.daily)
        {
            return std::nullopt; // 일봉 미준비 — 프리페치 대기
        }

        daily_ = snapshot_.daily; // 포인터 하나 — 벡터 복사가 아니다
        equity_ = snapshot_.equity;
        snapshot_bars = snapshot_.bars;
        bars_bucket = snapshot_.bars_bucket;
        bars_version = snapshot_.bars_version;
    }

    // 판단 봉은 아래에서 접거나 진행 봉 종가를 덮으므로 이 평가가 소유해야 한다. 체결이 살아 있으면
    //  집계기에서 새로 만들어지고, 그 경로에서는 스냅샷을 아예 복사하지 않는다.
    DecisionBars decision_bars;
    std::vector<MarketData>& bars = decision_bars.bars;

    const bool local_bars = websocket_bars_ && websocket_live_;
    decision_bars.local = local_bars;

    if (websocket_bars_)
    {
        // 날짜가 바뀌면 집계기를 비운다 — REST 분봉은 당일치만 돌려주므로 어제 봉이 SMA에 섞이면 뜻이 다르다.
        std::string today = kst_ymd();

        if (aggregator_day_ != today)
        {
            aggregator_.clear(symbol_id_);
            aggregator_day_ = std::move(today);
            reseed_pending_ = true;
        }

        // 새 REST 스냅샷(1분봉)은 한 번만 시드한다. 닫힌 자리는 REST가 이기고 빈 자리는 채워지므로,
        //  폴백 동안 못 본 분·구독 뒤 늦게 붙은 종목의 앞 분이 여기서 메워진다.
        if (snapshot_bars && !snapshot_bars->empty() && bars_version != seeded_version_)
        {
            seed_aggregator(*snapshot_bars, bars_version, local_bars);
        }

        // 분이 지난 진행 봉은 시계로 닫는다 — WS 틱은 on_tick이 이미 닫았고, REST 대체 틱 동안 남은 로컬
        //  진행 봉이 여기서 확정된다. 틱 수신 시각이 시계다(체결 시각은 hhmmss뿐이라 날짜가 없다). [why D-074]
        aggregator_.close_stale(symbol_id_, std::chrono::system_clock::to_time_t(trade.timestamp));

        // 판단 봉은 1분봉을 interval_min으로 접은 것이다 — 틱이 살아 있으면 집계기 스냅샷([0]=진행 중 분),
        //  REST 대체 틱이면 REST 1분봉. 두 길이 같은 resample을 지나므로 자리·계산이 같다. [why D-072]
        std::vector<MarketData> local = bars::resample(aggregator_.snapshot(symbol_id_, 0), parameters_.interval_min,
                                                       parameters_.simple_moving_average_period + 1);

        // 다음 REST 조회를 받을지 프리페치 스레드에 알린다. 워밍업(SMA 창 + 진행 봉)이 끝나고 틱이 살아 있으면
        //  REST는 쉰다.
        const bool want_seed = !websocket_live_ || reseed_pending_ ||
                               static_cast<int>(local.size()) < parameters_.simple_moving_average_period + 1;
        seed_wanted_.store(want_seed, std::memory_order_relaxed);

        if (local_bars)
        {
            bars = std::move(local); // 종가는 이미 방금 틱
        }
        else if (snapshot_bars)
        {
            bars =
                bars::resample(*snapshot_bars, parameters_.interval_min, parameters_.simple_moving_average_period + 1);
        }
    }
    else if (snapshot_bars)
    {
        // bar_source=rest — 아래 진행 봉 종가 덮어쓰기가 값을 고치므로 이 경로만 스냅샷을 복사한다.
        bars = *snapshot_bars;
    }

    // 진행 중인 봉(bars[0])의 종가를 방금 들어온 체결가로 덮는다. 프리페치가 봉 주기당
    //  한 번만 받으므로 그 사이의 가격 변화는 이 한 줄이 반영한다. 봉이 이미 넘어갔는데
    //  프리페치가 아직 안 왔으면(bucket 불일치) 덮지 않는다 — 마감된 봉의 종가를 고칠 순 없다.
    if (!local_bars && !bars.empty() && bars_bucket == kst_bar_bucket(parameters_.interval_min))
    {
        bars[0].close = current_price;
    }

    return decision_bars;
}

void DeviationScaleStrategy::seed_aggregator(const std::vector<MarketData>& snapshot_bars, uint64_t bars_version,
                                             bool local_bars)
{
    const int before = aggregator_.closed_count(symbol_id_);
    const int added = aggregator_.seed(symbol_id_, snapshot_bars);
    const bool first = seeded_version_ == 0;
    seeded_version_ = bars_version;
    reseed_pending_ = false;
    const std::string line = "[" + id() + "] 봉 시드 src=" + (local_bars ? "ws" : "rest") + " REST " +
                             std::to_string(snapshot_bars.size()) + "봉, 새 " + std::to_string(added) +
                             ", 닫힌 " + std::to_string(aggregator_.closed_count(symbol_id_)) + "(전 " +
                             std::to_string(before) + ")" + ", 진행 " +
                             (aggregator_.current_slot(symbol_id_).valid() ? "있음" : "없음");

    // 첫 시드·전환 뒤 시드만 INFO — 워밍업 동안 봉마다 오는 시드는 DEBUG로 내린다.
    if (first || added > 0)
    {
        LOG_INFO(line);
    }
    else
    {
        LOG_DEBUG(line);
    }
}

void DeviationScaleStrategy::prefetch_once()
{
    // 장 밖에서는 받아봐야 같은 응답이다. 우리 분봉 조회(KisMarket.cpp get_minute_ohlcv)가 장 밖 기준시각을 15:30으로 바꾸므로
    //  (2026-09-27 대조로 고침 — KIS가 아니라 우리 코드가 바꾼다) 장 마감 후엔 종일 같은 봉을 다시 받고, 그 호출이 초당 한도를
    //  차지해 다른 조회를 500으로 밀어낸다. 발주는 어차피 장중에만 나가므로 건너뛴다.
    //  창은 08:50~15:35로 장 마감 청산(15:15)까지 덮는다.
    //  근거: 한도 초과가 500으로 오는 것은 실측 — DAILY_LOG.md 2026-09-04 항목(분봉 호출량이 실전 한도의 70%).
    const int hhmm = kst_hhmm(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
    const int wday = kst_tm().tm_wday;
    const bool weekday = wday >= 1 && wday <= 5;
    const bool in_session = weekday && hhmm >= kPrefetchWindowOpenHhmm && hhmm <= kPrefetchWindowCloseHhmm;

    // 창 밖이라도 스냅샷이 아직 없으면 한 번은 받는다. 애프터마켓(16:00~20:00)에 재기동하면 창 안에서 받을 기회가
    //  없어 일봉 미준비로 판단을 계속 건너뛰고, 다시 맡은 보유분에 익절·손절이 걸리지 않았다(10-01 실계좌 082270·016360).
    bool first_snapshot_missing;
    {
        std::lock_guard<std::mutex> lock(snap_mutex_);
        first_snapshot_missing = !snapshot_.daily || !snapshot_.bars;
    }

    if (kis_ && weekday && (in_session || first_snapshot_missing))
    {
        // 일봉·자본: 날짜 바뀌면 1회 갱신(장중엔 사실상 1일 1회).
        std::string today = kst_ymd();
        bool need_daily;
        {
            std::lock_guard<std::mutex> lock(snap_mutex_);
            need_daily = !snapshot_.daily || snapshot_.daily_date != today;
        }

        if (need_daily)
        {
            auto daily_ohlcv = kis_->get_daily_ohlcv(parameters_.ticker, parameters_.daily_lookback);

            // 일봉이 비면(500·휴장) 스냅샷을 안 채우므로 need_daily가 참으로 남아
            //  다음 주기에 또 온다. 그때 잔고까지 같이 부르면 한도 초과 상황에서
            //  호출을 오히려 늘린다 — 일봉이 온 경우에만 잔고를 부른다.
            if (!daily_ohlcv.empty())
            {
                double equity = fetch_equity();
                std::lock_guard<std::mutex> lock(snap_mutex_);
                snapshot_.daily = std::make_shared<const std::vector<MarketData>>(std::move(daily_ohlcv));
                snapshot_.daily_date = std::move(today);
                snapshot_.equity = equity;
            }
        }

        // 3분봉: 봉이 바뀔 때만 갱신한다. 이 조회는 페이지네이션이라 1회에 HTTP GET이
        //  세 번 나가는데(당일 63분치 1분봉을 다시 받아 집계), 그중 마감된 봉은 불변이고
        //  달라지는 건 진행 중인 봉 하나뿐이다. 그 하나는 아래 on_trade_batch가 들어오는
        //  체결 틱으로 덮으므로 SMA 값은 같게 유지되면서 조회는 봉 주기당 1회로 준다.
        //  bar_source=ws면 이 조회는 시드용이다 — 첫 스냅샷, 그리고 샤드 스레드가 원할 때(워밍업·
        //  REST 대체 틱·출처 전환 뒤)만 봉마다 한 번 받고, 틱이 살아 있고 봉이 찼으면 쉰다. 이때는
        //  1분봉 그대로 받는다(같은 63분치·같은 GET 수) — 접는 건 샤드 스레드의 resample이다. [why D-072]
        const int bucket = kst_bar_bucket(parameters_.interval_min);
        bool need_bars;
        {
            std::lock_guard<std::mutex> lock(snap_mutex_);
            need_bars = !snapshot_.bars || snapshot_.bars_bucket != bucket;

            if (need_bars && websocket_bars_ && snapshot_.bars && !seed_wanted_.load(std::memory_order_relaxed))
            {
                need_bars = false;
            }

            // 봉 경계 직후 종목별 지터만큼 미룬다(첫 스냅샷은 바로). 진행 중인 봉은
            //  on_trade_batch의 체결가 덮어쓰기가 채우므로 늦게 받아도 SMA는 같다.
            if (need_bars && snapshot_.bars && kst_sec_into_bucket(parameters_.interval_min) < prefetch_jitter_sec_)
            {
                need_bars = false;
            }
        }

        if (need_bars)
        {
            auto bars = websocket_bars_
                            ? kis_->get_minute_ohlcv(
                                  parameters_.ticker,
                                  (parameters_.simple_moving_average_period + 1) * parameters_.interval_min, 1)
                            : kis_->get_minute_ohlcv(parameters_.ticker, parameters_.simple_moving_average_period + 1,
                                                     parameters_.interval_min);

            if (!bars.empty())
            {
                std::lock_guard<std::mutex> lock(snap_mutex_);
                snapshot_.bars = std::make_shared<const std::vector<MarketData>>(std::move(bars));
                snapshot_.bars_bucket = bucket;
                ++snapshot_.bars_version;
            }
        }
    }
}

void DeviationScaleStrategy::stop_prefetch()
{
    if (prefetch_pool_ && prefetch_task_ != 0)
    {
        prefetch_pool_->remove(prefetch_task_);
    }

    prefetch_task_ = 0;
}

double DeviationScaleStrategy::fetch_equity()
{
    constexpr std::chrono::seconds kRetryCooldown{60}; // 조회 실패 뒤 다시 부르지 않는 시간

    std::string today = kst_ymd();

    {
        std::unique_lock<std::mutex> lock(g_equity_cache.mutex);
        EquityEntry&                 entry = g_equity_cache.by_account[parameters_.account];

        // 같은 계좌를 다른 스레드가 조회 중이면 끝나기를 기다린다. 기다리는 동안 락은 놓여 있다.
        g_equity_cache.fetched.wait(lock, [&entry]
        {
            return !entry.fetching;
        });

        if (entry.fetched_date == today && entry.equity > 0.0)
        {
            return entry.equity;
        }

        if (std::chrono::steady_clock::now() < entry.retry_after)
        {
            return 0.0; // 직전 조회가 실패했다 — 쿨다운 안에는 서버를 다시 때리지 않는다
        }

        entry.fetching = true;
    }

    // 잔고 조회(REST)는 락 밖에서 한다. 같은 계좌의 다른 스레드는 위 wait에서 기다리고, 다른 계좌는 막히지 않는다.
    double equity = 0.0;
    KisClient* kis_client = account_kis();

    if (kis_client && kis_client->has_account())
    {
        try
        {
            const KisResult<AccountBalance> balance = kis_client->get_balance();

            if (balance && balance->total_evaluation_amount)
            {
                equity = *balance->total_evaluation_amount;
            }
        }
        catch (...)
        {
            // 여기서 새면 fetching이 true로 남아 같은 계좌 스레드가 영영 기다린다. 실패는 아래 로그 한 줄로 남긴다.
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_equity_cache.mutex);
        EquityEntry&                entry = g_equity_cache.by_account[parameters_.account];
        entry.fetching                    = false;

        if (equity > 0.0)
        {
            entry.fetched_date = std::move(today);
            entry.equity = equity;
        }
        else
        {
            entry.retry_after = std::chrono::steady_clock::now() + kRetryCooldown;
        }
    }

    g_equity_cache.fetched.notify_all();

    if (equity <= 0.0)
    {
        LOG_WARN("[DEVSCALE] 기준자본(총평가금) 조회 실패 — " + std::to_string(kRetryCooldown.count()) +
                 "초 쿨다운, 그 안에는 폴백 자본으로 간다 - account(" + parameters_.account + ")");
    }

    return equity;
}

int DeviationScaleStrategy::kst_bar_bucket(int interval_min)
{
    if (interval_min < 1)
    {
        interval_min = 1;
    }

    struct tm local_time = kst_tm();
    return (local_time.tm_yday * kMinutesPerDay + local_time.tm_hour * 60 + local_time.tm_min) / interval_min;
}

int DeviationScaleStrategy::kst_sec_into_bucket(int interval_min)
{
    if (interval_min < 1)
    {
        interval_min = 1;
    }

    struct tm local_time = kst_tm();
    return ((local_time.tm_hour * 60 + local_time.tm_min) % interval_min) * 60 + local_time.tm_sec;
}

bars::BarAggregator::Config DeviationScaleStrategy::aggregator_config(const Params& parameters)
{
    bars::BarAggregator::Config config;
    config.interval_min = 1;
    config.keep =
        (std::max)(64, (parameters.simple_moving_average_period + 2) * (std::max)(1, parameters.interval_min));
    return config;
}
