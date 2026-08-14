#pragma once
#include "api/KisClient.h"
#include "api/KisWebSocket.h"
#include "core/RingBuffer.h"
#include "core/RegimeController.h"
#include "core/Types.h"
#include "risk/OrderGate.h"
#include "strategy/StrategyBase.h"
#ifdef HAS_ZMQ
#include "ipc/ZmqBridge.h"
#endif
#include "ipc/OrderRouter.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Engine  —  퀀트 트레이딩 엔진
//
//  [데이터 스레드]  KIS REST 일봉 폴링   → market_queue_
//  [전략 스레드]    ob_queue_ + td_queue_ + market_queue_ → order_queue_
//  [주문 스레드]    order_queue_ → KIS REST 주문 (KR/US 자동 분기)
//
//  WS 구독 목록은 on_start() 이후 전략의 get_watch_specs()로 동적 수집
// ─────────────────────────────────────────────────────────────────────────────
class Engine
{
public:
    Engine(KisConfig kis_cfg, int fetch_interval_sec = 60);
    ~Engine();

    void add_strategy(std::unique_ptr<StrategyBase> strategy);
    // 기동 시 get_balance로 실계좌 보유분을 OrderGate 원장에 시드(G5). main이 config로 설정.
    void set_bootstrap_ledger(bool b) { bootstrap_ledger_ = b; }
    // REST 현재가 폴링을 체결 피드로 사용(WS 실시간 세션 우회). true면 DataThread가
    // get_current_price를 폴링해 TradeData로 td_queue_에 넣고, WS 연결은 생략한다.
    void set_rest_price_feed(bool b) { rest_price_feed_ = b; }
    // 매크로 레짐 사이드카 브리지(2026-08-09 회의 Task 3). Python macro_regime_feed.py가
    // 원자적으로 쓰는 regime.json 경로를 지정하면, data_thread가 매 사이클 읽어
    // OrderGate::set_entry_halt(entry_halt)를 토글한다(신규매수만 차단, 청산은 통과).
    // path 빈 문자열이면 기능 미가동(기본). stale_sec 경과 파일은 신뢰하지 않음(페일세이프).
    void set_regime_file(const std::string& path, int stale_sec = 600)
    {
        regime_file_ = path;
        if (stale_sec > 0) regime_stale_sec_ = stale_sec;
    }
    // 기동 스모크 프로브 — 서버 실행 직후 지정 종목을 시장가로 1회 매수해 모의계좌 주문경로
    //  (OrderRouter→체결통보→원장)가 실제로 도는지 검증한다. qty≤0 또는 ticker 빈 문자열이면 미가동.
    //  strategy_thread가 order_queue_의 단일 생산자이므로 그 스레드 진입 시 1회만 push한다.
    void set_startup_probe(const std::string& ticker, int qty)
    {
        startup_probe_ticker_ = ticker;
        startup_probe_qty_    = qty;
    }
    // 시세 전용 클라이언트 설정(실전 도메인). KIS 모의(openapivts)는 시세 REST가 HTTP 500이라
    // 시세는 실전 키+실전 도메인으로 조회하고 주문만 모의로 낸다. rest_price_feed_ 폴링이 사용.
    void set_quote_kis_config(const KisConfig& c)
    {
        quote_kis_cfg_ = c;
        has_quote_kis_ = true;
    }
    // 주문 페이싱/재시도 (C-2/W-3) — 버스트 청산이 초당한도로 튕겨 유실되는 것 방지.
    //  min_interval_ms 간격으로만 발주(레이트리밋 하회), 거부된 청산 SELL은 order_thread
    //  로컬 큐로 dedup 창 밖에서 최대 max_retries회 재시도. 스레드 시작 전에만 호출.
    void set_order_pacing(int min_interval_ms, int max_retries)
    {
        order_min_interval_ms_ = min_interval_ms;
        order_max_retries_ = max_retries;
    }
    // OrderGate 위험 한도를 config로 주입(스레드 시작 전에만). 기본값은 OrderGate::Config.
    void set_risk_config(const OrderGate::Config& c) { order_gate_.set_config(c); }
    size_t strategy_count() const { return strategies_.size(); }
    // 직전 추가된 전략에 활성 국면 설정 (main.cpp config 파싱용)
    void set_last_active_regimes(const std::vector<Regime>& r)
    {
        if (!strategies_.empty()) strategies_.back()->set_active_regimes(r);
    }
    // 주기적 유니버스 재스캔(동적 등록). universe_fn: 시세 클라이언트로 유니버스 티커 목록 산출.
    // factory: 티커 → 전략 인스턴스 생성. interval_sec: 재스캔 주기(초, ≤0이면 비활성).
    // data_thread가 interval_sec마다 universe_fn을 호출해 신규 티커만 런타임 등록한다.
    void set_universe_rescan(
        std::function<std::vector<std::string>(KisClient&)> universe_fn,
        std::function<std::unique_ptr<StrategyBase>(const std::string&)> factory,
        int interval_sec)
    {
        universe_fn_ = std::move(universe_fn);
        strategy_factory_ = std::move(factory);
        rescan_interval_sec_ = interval_sec;
    }
    // 티커→종목명 매핑 등록/조회 (로그 가독성). 스캔·가디언 부착 스레드가 write,
    //  전략 스레드의 신호 로그가 read라 ticker_names_mu_로 보호.
    void register_ticker_name(const std::string& ticker, const std::string& name);
    // 이름이 있으면 "티커(종목명)", 없으면 티커 원문을 반환.
    std::string ticker_label(const std::string& ticker) const;

    void start();
    void stop();

    bool is_running() const
    {
        return running_.load();
    }

private:
    void data_thread_fn();
    void strategy_thread_fn();
    void order_thread_fn();
    void control_thread_fn(); // ZMQ REP 명령 처리 (HAS_ZMQ 시 활성)
    void bootstrap_ledger();  // G5: get_balance → OrderGate.seed_position (스레드 시작 전 1회)
    void reconcile_from_balance(); // C-1: rest 모드 주기적 잔고 재조회 → positions_/daily_pnl_ 재동기
    void poll_regime_file();       // 매크로 레짐 파일 폴링 → OrderGate entry_halt 토글 (data_thread 전용)
    void maybe_rescan_universe();  // 주기적 유니버스 재스캔 → 신규 티커 런타임 등록 (data_thread 전용)
    // 런타임 전략 등록(set_kis·position_provider·on_start·set_active·watch_specs_ 추가 일괄).
    // strategies_ push_back은 락 하에, strat_version_ 증가로 strategy_thread 스냅샷 갱신 유도.
    void register_strategy_runtime(std::unique_ptr<StrategyBase> strategy);

    bool is_kr_market_open() const;
    bool is_us_market_open() const;
    bool is_any_market_open() const;
    void print_stats() const;

    KisConfig kis_cfg_;
    int fetch_interval_sec_;
    bool bootstrap_ledger_ = false; // 기동 시 실계좌 보유분 원장 시드 여부(G5, opt-in)
    std::string startup_probe_ticker_;  // 기동 스모크 프로브 종목(빈 문자열=미가동)
    int         startup_probe_qty_ = 0; // 기동 스모크 프로브 수량(≤0=미가동)
    bool        startup_probe_fired_ = false; // 프로브 1회성 발사 가드
    bool rest_price_feed_ = false;  // REST 현재가 폴링을 체결 피드로 사용(WS 우회, opt-in)
    // 매크로 레짐 브리지 상태(data_thread 전용) — regime.json → OrderGate entry_halt.
    std::string regime_file_;             // 빈 문자열이면 기능 미가동
    int  regime_stale_sec_    = 600;      // 이 초 이상 오래된 파일은 신뢰 안 함(사이드카 사망 감지)
    bool regime_halt_on_      = false;    // 우리가 현재 건 halt 상태(전이 시에만 로그·set 호출)
    bool regime_stale_warned_ = false;    // stale 경고 1회화
    bool regime_liq_warned_   = false;    // force_liquidate 경고 1회화
    KisConfig quote_kis_cfg_;        // 시세 전용(실전 도메인) 설정
    bool has_quote_kis_ = false;     // 시세 전용 클라이언트 사용 여부
    int order_min_interval_ms_ = 350; // 주문 간 최소 간격(ms) — 초당한도 회피(C-2/W-3)
    int order_max_retries_ = 3;       // 거부된 청산 SELL 재시도 횟수(C-2)
    // C-1 리컨사일 상태(rest 모드 전용) — 당일 기준 총평가금 대비 델타로 daily_pnl_ 근사.
    bool have_pnl_baseline_ = false;
    double pnl_baseline_ = 0.0;       // 당일 첫 리컨사일 시 캡처한 총평가금(원)
    // 잔고조회 서킷브레이커 — 모의/실서버 inquire-balance가 연속 타임아웃(12002)하면 GET 3회
    //  재시도로 사이클당 ~60s를 태우고 데이터 스레드를 정체시킨다. 실패 누적 시 지수 백오프로
    //  조회 자체를 건너뛰어 핫루프를 보호하고, 성공 시 즉시 복귀한다.
    int reconcile_fail_streak_ = 0;   // 연속 실패 수(성공 시 0)
    int reconcile_skip_remaining_ = 0; // 남은 스킵 사이클 수(>0이면 조회 생략)

    std::unique_ptr<KisClient> kis_;
    std::unique_ptr<KisClient> quote_kis_; // 시세 전용(실전 도메인). rest_price_feed_ 시에만 생성
    std::unique_ptr<KisWebSocket> ws_;

    std::vector<std::unique_ptr<StrategyBase>> strategies_;
    // strategies_ 동시성 보호: strategy_thread는 strat_version_ 변경 시에만 StrategyBase*
    // 스냅샷을 재구성(무락 순회), data_thread는 재스캔 등록 시 락+version 증가.
    std::mutex strat_mutex_;
    std::atomic<uint64_t> strat_version_{0};

    // 주기적 유니버스 재스캔 상태 (data_thread 전용)
    std::function<std::vector<std::string>(KisClient&)> universe_fn_;
    std::function<std::unique_ptr<StrategyBase>(const std::string&)> strategy_factory_;
    int rescan_interval_sec_ = 0;                 // ≤0이면 재스캔 비활성
    std::unordered_set<std::string> registered_tickers_; // 이미 등록된 KR 티커(중복 등록 방지)
    std::chrono::steady_clock::time_point last_rescan_{};

    RingBuffer<MarketData> market_queue_{1024};
    RingBuffer<OrderSignal> order_queue_{256};
    RingBuffer<OrderBook> ob_queue_{4096}; // 호가 (국내)
    RingBuffer<TradeData> td_queue_{4096}; // 체결 (미국 + 국내)

    std::thread data_thread_;
    std::thread strategy_thread_;
    std::thread order_thread_;
    std::thread control_thread_;

    std::atomic<bool> running_{false};

#ifdef HAS_ZMQ
    std::unique_ptr<ZmqBridge> zmq_bridge_;
#endif

    std::atomic<uint64_t> data_count_{0};
    std::atomic<uint64_t> signal_count_{0};
    std::atomic<uint64_t> order_count_{0};

    OrderGate order_gate_;
    std::unique_ptr<OrderRouter> order_router_; // FEP 레이어 (start() 이후 유효)
    std::unique_ptr<RegimeController> regime_;  // 국면 메타레이어 (start() 이후 유효)

    // 전략에서 수집한 구독 스펙 (on_start 이후 확정)
    std::vector<WatchSpec> watch_specs_;

    // 티커→종목명 라벨(로그 표시용). 여러 스레드가 접근해 ticker_names_mu_로 보호.
    std::unordered_map<std::string, std::string> ticker_names_;
    mutable std::mutex ticker_names_mu_;
};
