#pragma once
#include "api/KisClient.h"
#include "api/KisWebSocket.h"
#include "core/DataPoller.h"
#include "core/LedgerReconciler.h"
#include "core/RingBuffer.h"
#include "core/WakeGate.h"
#include "core/OrderPacer.h"
#include "core/SignalDispatcher.h"
#include "core/RegimeController.h"
#include "core/RegimeFileBridge.h"
#include "core/Types.h"
#include "risk/OrderGate.h"
#include "strategy/StrategyBase.h"
#ifdef HAS_ZMQ
#include "ipc/ZmqBridge.h"
#endif
#include "ipc/OrderRouter.h"
#include "ipc/OpsServer.h"
#include "core/MpscQueue.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <condition_variable>
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
    // 스레드·뮤텍스를 소유한다 — 복사는 원본과 사본이 같은 자원을 두 번 닫는 길이라 막는다.
    Engine(const Engine&)            = delete;
    Engine& operator=(const Engine&) = delete;

    void add_strategy(std::unique_ptr<StrategyBase> strategy);
    // 기동 시(bootstrap) 실계좌 잔고를 내부 장부의 초기값으로 채운다(G5).
    // 프로그램을 재시작하면 OrderGate 원장이 0으로 비는데, 실계좌엔 이미 보유분이 남아있다.
    // get_balance(잔고조회)로 종목·수량·평단을 읽어 원장에 심어(seed) 실제와 장부를 맞춘다
    // (안 맞으면 매도수량·평단·손실한도 계산이 어긋난다). main이 config로 켠다.
    void set_bootstrap_ledger(bool b) { bootstrap_ledger_ = b; }
    // 실시간 체결가는 원래 WebSocket으로 받지만, WS 세션이 rt_cd=9(ALREADY IN USE, 중복접속)로
    // 폭주할 때의 우회책이다. true면 DataThread가 REST get_current_price(현재가 조회)를 주기적으로
    // 폴링해 그 값을 TradeData(체결 틱)처럼 td_queue_에 넣고, WS 연결은 생략한다. ITB 전략이
    // 이 틱으로 구동된다(ITB = IntradayBreakoutStrategy, 장중 돌파 전략).
    void set_rest_price_feed(bool b) { rest_price_feed_ = b; }
    // 매크로 레짐 보조 프로세스 브리지(2026-08-09 회의 Task 3). Python macro_regime_feed.py가
    // 원자적으로 쓰는 regime.json 경로를 지정하면, data_thread가 매 사이클 그 파일을 읽어
    // 시장이 위험하면 OrderGate 의 "신규매수 정지" 스위치(entry_halt)를 켜고, 풀리면 끈다
    // (매수만 막고 청산·매도는 그대로 통과). path 빈 문자열이면 기능 미가동(기본).
    // stale_sec(기본 kDefaultRegimeStaleSec)보다 오래된 파일은 보조 프로세스가 죽은 것으로 보고 무시한다.
    //  판정 규칙은 core/RegimeFileBridge.h가 소유한다. [why D-060]
    void set_regime_file(const std::string& path, int stale_sec = kDefaultRegimeStaleSec)
    {
        regime_file_ = path;
        regime_bridge_.set_stale_sec(stale_sec);
    }

    // 매크로 진입정지의 시간 상자. 개장 후 이 분수가 지나면 entry_halt를 스스로 풀고,
    // 그날 매크로 축은 다시 halt를 걸지 못한다. 그 뒤 통제는 장중을 실제로 보는 축
    // (UniverseScanner 코스피 게이트·종목 정배열)이 갖는다. force_liquidate는 대상이 아니다.
    // 0 이하면 만료를 끈다. [why D-033]
    void set_regime_halt_expire_min(int m) { regime_bridge_.set_halt_expire_min(m); }

    // 기동 스모크 테스트(smoke test: 전원 켜서 최소한 도는지 보는 점검) — 서버 실행 직후 지정
    //  종목을 시장가로 딱 1회 매수해 주문 경로 전체(OrderRouter→체결통보→원장)가 살아있는지
    //  확인한다. qty≤0 또는 ticker 빈 문자열이면 미가동.
    //  strategy_thread가 order_queue_의 단일 생산자이므로 그 스레드 진입 시 1회만 push한다.
    //  하루 한 번만 낸다(logs/startup_probe_YYYYMMDD 표식) — 재기동마다 내면 점검 주식이 쌓인다
    //  (2026-09-09 재기동 13회 → 005930 13주). 체결이 확인되면 같은 수량을 바로 되판다.
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

    // 주문 호출 간격 조절/재시도 (C-2/W-3) — 버스트 청산이 초당한도로 튕겨 유실되는 것 방지.
    //  min_interval_ms 간격으로만 발주(레이트리밋 하회), 거부된 청산 SELL은 order_thread
    //  로컬 큐로 dedup 창 밖에서 최대 max_retries회 재시도. 스레드 시작 전에만 호출.
    void set_order_pacing(int min_interval_ms, int max_retries)
    {
        order_min_interval_ms_ = min_interval_ms;
        order_max_retries_ = max_retries;
    }

    // OrderGate 위험 한도를 config로 주입(스레드 시작 전에만). 기본값은 OrderGate::Config.
    void set_risk_config(const OrderGate::Config& c) { order_gate_.set_config(c); }
    // 슬롯 수 조회 — 스캐너가 "베이스 총합이 목표 노출을 넘지 않도록" 배수를 정규화할 때 쓴다.
    //  위험 config는 전략 로딩보다 먼저 주입되므로(main.cpp) 이 시점에 이미 유효하다.
    int risk_max_positions() const { return order_gate_.config().max_concurrent_positions; }
    // 점수 랭크를 게이트에 주입 — 슬롯이 꽉 차갈수록 상위 점수만 통과시킨다.
    //  같은 표를 로그 폴더 entry_scores.json에도 남겨 대시보드가 보유 종목을 점수순으로 보인다. [why D-047]
    void set_entry_priority(std::unordered_map<std::string, int> rank,
                            std::unordered_map<std::string, double> z, int total);
    // 현재 보유(롱) 원장 스냅샷 — 유니버스 재스캔의 "보유분 제외"가 매회 최신 잔고를 보게 한다.
    //  기동 시 1회 조회한 잔고를 계속 쓰면 청산된 종목이 세션 내내 후보에서 빠진다.
    std::vector<OrderGate::HeldPos> held_positions() const { return order_gate_.snapshot_positions(); }
    size_t strategy_count() const { return strategies_.size(); }
    // 직전 추가된 전략에 활성 국면 설정 (main.cpp config 파싱용)
    void set_last_active_regimes(const std::vector<Regime>& r)
    {
        if (!strategies_.empty())
        {
            strategies_.back()->set_active_regimes(r);
        }
    }

    // 주기적 유니버스 재스캔(동적 등록). universe_fn: 시세 클라이언트로 유니버스 티커 목록 산출.
    // factory: 티커 → 전략 인스턴스 생성. interval_sec: 재스캔 주기(초, ≤0이면 비활성).
    // data_thread가 interval_sec마다 universe_fn을 호출해 신규 티커만 런타임 등록한다.
    // max_registered: 등록 전략 총수 상한(0이면 무제한). 등록 하나당 프리페치 스레드와 실시간
    //  구독이 붙으므로 상한이 없으면 하루가 갈수록 조회량이 늘어난다.
    // drop_after_sec: 스캔 결과에서 이만큼 연속으로 빠져 있는 종목의 전략을 뗀다(≤0이면 안 뗌).
    //  보유·미체결 선점이 있는 종목은 빠져 있어도 안 뗀다. 20초 재스캔에 한 번 빠졌다고 떼면
    //  등록(차트 조회)과 해제가 도는 회전이 나므로 유지 시간을 둔다.
    void set_universe_rescan(
        std::function<std::vector<std::string>(KisClient&)> universe_fn,
        std::function<std::unique_ptr<StrategyBase>(const std::string&)> factory,
        int interval_sec, size_t max_registered = 0, int drop_after_sec = 0)
    {
        // 슬리브마다 한 번씩 부른다 — 덮어쓰지 않고 쌓는다. 예전에는 단일 슬롯이라
        //  두 번째 호출이 첫 번째를 조용히 지웠다(먼저 건 재스캔이 사라짐).
        RescanJob j;
        j.universe_fn    = std::move(universe_fn);
        j.factory        = std::move(factory);
        j.interval_sec   = interval_sec;
        j.max_registered = max_registered;
        j.drop_after_sec = drop_after_sec;
        rescan_jobs_.push_back(std::move(j));
    }

    // ── G1: 국면→전략 자동선택 ──────────────────────────────────────────────
    // 국면(BULL/NEUTRAL/BEAR)별 활성 전략 id 목록(권위적 선택자). 스레드 시작 전에만.
    //  현재 국면 목록에 든 전략만 활성, 나머지 비활성.
    //  id 항목이 '*'로 끝나면 접두 매칭(스캐너 동적 id: "DevScale_*", "ITB_*").
    //  맵이 비면(미지정) 기존 per-strategy active_regimes 방식으로 폴백(하위호환).
    void set_regime_strategies(std::map<Regime, std::vector<std::string>> m)
    {
        regime_strategies_ = std::move(m);
        has_regime_map_ = !regime_strategies_.empty();
    }

    // 장중 국면 재평가 주기(초, ≤0이면 기본 유지). 국면 변화 시 전략셋 동적 재선택.
    void set_regime_reeval_interval(int sec)
    {
        if (sec > 0)
        {
            regime_reeval_interval_sec_ = sec;
        }
    }

    // 국면 판정기 파라미터(지수코드·이평기간·점수 임계값). 스레드 시작 전에만.
    //  미지정이면 RegimeController::Config 기본값 그대로라 기존 동작이 변하지 않는다.
    void set_regime_config(RegimeController::Config c) { regime_cfg_ = c; }
    // ZMQ 제어 채널. bind 주소 기본 127.0.0.1(모든 인터페이스 노출 금지), token이 비면 KILL은
    //  거부된다(config `zmq_control_token`). 스레드 시작 전에만. HAS_ZMQ가 꺼진 빌드에선 무시.
    void set_zmq_control(const std::string& bind_addr, const std::string& token)
    {
        if (!bind_addr.empty())
        {
            zmq_bind_addr_ = bind_addr;
        }

        zmq_control_token_ = token;
    }

    // 운영단말 TCP 채널(config `ops_bind_addr`·`ops_port`·`ops_token`). port 0이면 열지 않는다.
    //  스레드 시작 전에만. 루프백이 아닌 주소는 token이 있어야 서버가 뜬다(OpsServer::start).
    void set_ops_control(const std::string& bind_addr, int port, const std::string& token)
    {
        ops_bind_addr_ = bind_addr;
        ops_port_      = port;
        ops_token_     = token;
    }

    // 티커→종목명 매핑 등록/조회 (로그 가독성). 스캔·청산 관리 부착 스레드가 write,
    //  전략 스레드의 신호 로그가 read라 ticker_names_mu_로 보호.
    // 청산 관리가 붙은 티커. 이 종목은 그날 스캔 슬리브의 신규매수 대상에서 뺀다.
    //  청산 관리(청산 전용)과 스캔 전략(진입)이 같은 티커에 동시에 붙으면 한쪽이 턴 것을
    //  다른 쪽이 곧바로 되사서 수수료만 나간다(2026-09-08 금호건설: 13:39:59 전량매도 →
    //  13:40:12 재매수). 기동 시 단일스레드 구간에서만 채우고 전략 스레드는 읽기만 한다.
    void mark_guardian_ticker(const std::string& ticker) { guardian_tickers_.insert(ticker); }
    bool is_guardian_ticker(const std::string& t) const { return guardian_tickers_.count(t) > 0; }

    void register_ticker_name(const std::string& ticker, const std::string& name);
    // 이름이 있으면 "티커(종목명)", 없으면 티커 원문을 반환.
    std::string ticker_label(const std::string& ticker) const;
    // 등록된 종목명(hts_kor_isnm)만 반환, 없으면 빈 문자열. 전략에 이름을 주입해 로그에 노출할 때 사용.
    std::string ticker_name(const std::string& ticker) const;

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
    void fill_thread_fn();     // 체결통보 소비(fill_queue_ → OrderRouter::on_fill → ops 방송). WS 수신 스레드에서 뗀 것 [why D-056]
    void control_thread_fn(); // WebSocket 시세단절 감지·재연결(연속 실패 시 kill switch). ZMQ REP 처리는 ZmqBridge 내부 스레드 담당
    StrategyBase::SellableInfo ledger_sellable(const std::string& account, const std::string& ticker) const;
    // 매크로 레짐 파일 읽기 → RegimeFileBridge 판정 → OrderGate entry_halt·force_liquidate_ 적용 (data_thread 전용)
    void poll_regime_file();
    void maybe_rescan_universe();  // 주기적 유니버스 재스캔 → 신규 티커 런타임 등록·이탈 티커 해제 (data_thread 전용)
    // 전략 해제는 두 단계다. 뗄 때는 strategies_에서 빼고 retired_로 옮기며 버전을 올린다 —
    //  strategy_thread의 옛 스냅샷이 아직 그 포인터를 들고 있을 수 있어서 바로 지우지 않는다.
    //  strategy_thread가 새 스냅샷을 만든 뒤(strat_seen_version_ ≥ 뗀 시점 버전) on_stop·파기한다.
    //  force=true는 종료 경로(strategy_thread 합류 뒤)에서 전부 비운다.
    void reap_retired(bool force);
    // G1: 현재 국면 r에 맞춰 전략별 active 플래그 재선택. 선택 결정을 로그로 기록(국면 변화
    //  또는 force_log 시). data_thread 전용(strategies_ 반복은 이 스레드에서만 mutate).
    void apply_regime_selection(Regime r, bool force_log);
    // 런타임 전략 등록(set_kis·position_provider·on_start·set_active·watch_specs_ 추가 일괄).
    // strategies_ push_back은 락 하에, strat_version_ 증가로 strategy_thread 스냅샷 갱신 유도.
    void register_strategy_runtime(std::unique_ptr<StrategyBase> strategy);

    // 지금 활성인 전략 중 일봉(on_data)을 쓰는 전략이 하나라도 있는가. data_thread의 일봉 폴링
    //  가드 — 아무도 안 쓰면 종목 수만큼의 차트 TR이 매 사이클 버려진다. 국면 전환으로 전략 집합이
    //  바뀌므로 캐시하지 않고 매번 확인한다(strategies_는 strat_mutex_ 하에 읽는다).
    bool daily_bars_needed();

    bool is_kr_market_open() const;
    bool is_us_market_open() const;
    bool is_any_market_open() const;
    void print_stats() const;
    // WS 피드가 죽었을 때 REST 현재가 폴링으로 낮춘다. 폴링이 쓸 시세 소스(실전 도메인)가
    //  없으면 낮춰봐야 틱이 안 나오므로 false를 돌려주고, 호출부는 kill switch로 넘어간다.
    bool activate_rest_fallback(const std::string& reason);
    // WS가 돌아왔을 때 원래 피드로 복귀. 처음부터 폴링이었으면(rest_price_feed_=true) 아무것도 안 한다.
    void deactivate_rest_fallback();

    KisConfig kis_cfg_;
    int fetch_interval_sec_;
    bool bootstrap_ledger_ = false; // 기동 시 실계좌 보유분 원장 시드 여부(G5, opt-in)
    std::string startup_probe_ticker_;  // 기동 점검 종목(빈 문자열=미가동)
    int         startup_probe_qty_ = 0; // 기동 점검 수량(≤0=미가동)
    bool        startup_probe_fired_ = false; // 기동 점검 1회성 발사 가드
    // 되팔기 상태(strategy_thread 전용). base=발사 직전 보유수량, 그 위로 qty만큼 늘면 체결로 본다.
    bool        startup_probe_settled_ = true;  // 되팔기 끝났거나 할 일 없음
    int         startup_probe_base_qty_ = 0;
    std::chrono::steady_clock::time_point startup_probe_fired_at_{};
    bool rest_price_feed_ = false;  // REST 현재가 폴링을 체결 피드로 사용(WS 우회, opt-in)
    // 지금 실제로 어느 피드로 도는지(런타임 상태). 기동 시 rest_price_feed_로 초기화하고,
    //  WS 모드에서 연결이 죽으면 control_thread가 true로 올려 폴링으로 낮춘다(WS 복귀 시 되돌림).
    //  config 의도(rest_price_feed_)와 분리해 둔 이유는, 폴백으로 켜진 것인지 처음부터 폴링이었는지를
    //  구분해야 복귀 여부를 판단할 수 있어서다. control_thread write / data_thread read.
    std::atomic<bool> rest_feed_active_{false};
    bool rest_fallback_engaged_ = false; // 폴백으로 낮춘 상태인가(control_thread 전용, 전이 로그·복귀 판정)
    // 매크로 레짐 브리지(data_thread 전용) — regime.json → OrderGate entry_halt. 상태기계는 헤더에, 파일 I/O·로그는 여기.
    std::string                      regime_file_;   // 빈 문자열이면 기능 미가동
    regime_bridge::RegimeFileBridge  regime_bridge_; // stale·시간 상자·1회 로그 판정 [why D-060]
    // G3: 극단 위험회피(force_liquidate=TRUE) 시 보유 전량 강제청산 요청 플래그.
    //  data_thread(poll_regime_file)가 set → strategy_thread(order_queue_ 단일 생산자)가
    //  이 플래그를 보고 매 주기 시장가 전량 매도를 발주(주문큐의 단일생산자·단일소비자(SPSC)
    //  규약 위반 회피 — order_queue_에 넣는 스레드를 하나로 유지). 해제 시 중단.
    std::atomic<bool> force_liquidate_{false};
    KisConfig quote_kis_cfg_;        // 시세 전용(실전 도메인) 설정
    bool has_quote_kis_ = false;     // 시세 전용 클라이언트 사용 여부
    int order_min_interval_ms_ = 350; // 주문 간 최소 간격(ms) — order_thread의 OrderPacer가 쓴다 [why D-065]
    int order_max_retries_ = 3;       // 거부된 주문의 재시도 횟수(C-2)
    // 잔고 → 원장 대조기(기동 시드·주기 대조·손익 기준선·서킷브레이커). start()에서 kis_·order_router_ 뒤에
    //  만들고 data_thread만 부른다. [why D-061]
    std::unique_ptr<LedgerReconciler> ledger_;
    // REST 현재가 폴러(폴링 모드 유니버스·WS 넘침 대체·보유 보충). start()에서 kis_ 뒤에 만들고
    //  data_thread만 부른다 — 넘침 목록이 여기로 옮겨가며 watch_specs_mtx_ 보호에서 빠졌다. [why D-062]
    std::unique_ptr<DataPoller> poller_;

    std::unique_ptr<KisClient> kis_;
    // 시세 전용(실전 도메인). WS 모드에서도 폴백이 쓸 수 있어야 하므로 config에 블록이 있으면 항상 만든다.
    std::unique_ptr<KisClient> quote_kis_;
    std::unique_ptr<KisWebSocket> ws_;

    std::vector<std::unique_ptr<StrategyBase>> strategies_;
    // strategies_ 동시성 보호: strategy_thread는 strat_version_ 변경 시에만 StrategyBase*
    // 스냅샷을 재구성(무락 순회), data_thread는 재스캔 등록 시 락+version 증가.
    std::mutex strat_mutex_;
    std::atomic<uint64_t> strat_version_{0};
    std::atomic<uint64_t> strat_seen_version_{0}; // strategy_thread가 마지막으로 스냅샷에 반영한 버전
    // 뗀 전략 대기열(data_thread 전용). ver=뗀 직후의 strat_version_.
    struct Retired
    {
        std::unique_ptr<StrategyBase> strategy;
        uint64_t                      ver = 0;
    };
    std::vector<Retired> retired_;

    // 주기적 유니버스 재스캔 상태 (data_thread 전용)
    // 슬리브 하나당 한 건. 상한(max_registered)은 그 슬리브가 등록한 수로만 센다
    //  — 공유 카운트로 세면 한 슬리브가 다른 슬리브의 자리를 먹는다.
    struct RescanJob
    {
        std::function<std::vector<std::string>(KisClient&)> universe_fn;
        std::function<std::unique_ptr<StrategyBase>(const std::string&)> factory;
        int    interval_sec   = 0;
        size_t max_registered = 0;
        size_t registered     = 0;
        int    drop_after_sec = 0;
        std::chrono::steady_clock::time_point last_run{};
        std::unordered_map<std::string, StrategyBase*> owned; // 이 슬리브가 등록한 티커 → 전략(해제 대상 식별)
        std::unordered_map<std::string, std::chrono::steady_clock::time_point> absent_since; // 스캔에서 빠진 시각
    };
    std::vector<RescanJob> rescan_jobs_;
    std::unordered_set<std::string> registered_tickers_; // 등록된 KR 티커(중복 방지, 슬리브 공유)
    std::unordered_set<std::string> guardian_tickers_;  // 청산 관리 보유 티커(스캔 신규매수 제외)

    // G1: 국면→전략 자동선택 상태 (data_thread 전용)
    std::map<Regime, std::vector<std::string>> regime_strategies_; // 국면별 활성 전략 id(빈 항목=아무 전략도 활성 안 함)
    bool has_regime_map_ = false;                 // false면 per-strategy active_regimes 폴백
    Regime last_selected_regime_ = Regime::UNKNOWN; // 직전 선택 국면(변화 감지→재선택·로그)
    int regime_reeval_interval_sec_ = 300;        // 장중 국면 재평가 주기(초)
    std::chrono::steady_clock::time_point last_regime_eval_{};

    RingBuffer<MarketData> market_queue_{1024};
    RingBuffer<OrderSignal> order_queue_{256};
    RingBuffer<OrderBook> ob_queue_{4096}; // 호가 (국내)
    RingBuffer<TradeData> td_queue_{4096}; // 체결 (미국 + 국내)
    // WS 상한에 밀린 종목의 REST 대체 틱. td_queue_는 WS 콜백 스레드가 생산자라 데이터 스레드가
    //  같이 넣으면 SPSC가 깨진다(두 생산자가 같은 슬롯에 쓰고 head를 한 칸만 올린다). [why D-053]
    RingBuffer<TradeData> rest_td_queue_{1024};
    // 체결통보. WS 수신 스레드는 여기 push만 하고 원장 반영(OrderRouter::on_fill)은 fill_thread가 한다 —
    //  체결 하나 처리(hist_mtx_·CSV 쓰기) 동안 전 종목 틱 수신이 멈추지 않게. [why D-056]
    RingBuffer<FillNotification> fill_queue_{1024};
    std::atomic<uint64_t> fill_dropped_{0};   // fill_queue_ 가득 차 버린 체결통보 수. 0이 아니면 잔고 대조가 원장을 메운다
    // 소비자 깨우기 — 생산자가 push 뒤 notify, 소비자는 큐가 비면 잔다. 1ms 폴링은 Windows 타이머 격자 때문에
    //  실측 p50 15.6ms였다(bench_sleep_res). [why D-071]
    sync::WakeGate fill_wake_;  // fill_thread ← WS 수신 스레드
    sync::WakeGate strat_wake_; // strategy_thread ← WS 수신·데이터·운영단말 스레드
    sync::WakeGate order_wake_; // order_thread ← strategy_thread

    std::thread data_thread_;
    std::thread strategy_thread_;
    std::thread order_thread_;
    std::thread fill_thread_;
    std::thread control_thread_;

    std::atomic<bool> running_{false};

#ifdef HAS_ZMQ
    std::unique_ptr<ZmqBridge> zmq_bridge_;
#endif

    // 운영단말 서버와 수동주문 인테이크. 서버 스레드가 push, strategy_thread(order_queue_ 단일
    //  생산자)가 pop해 OrderSignal(strategy_id="MANUAL")로 바꿔 게이트·원장을 그대로 지난다.
    //  FORCE_LIQ와 같은 이유로 소켓 스레드가 order_queue_에 직접 넣지 않는다. [why D-043]
    std::unique_ptr<OpsServer>      ops_server_;
    MpscQueue<OpsOrderReq>          manual_inbox_{256};
    std::string                     ops_bind_addr_;
    int                             ops_port_ = 0;
    std::string                     ops_token_;
    std::mutex                      manual_cid_mtx_;
    std::unordered_set<std::string> manual_cids_; // 재전송 중복 차단(세션 내)
    void        start_ops_server();
    std::string ops_status_json() const;
    std::string ops_positions_json() const;
    // strategy_thread 전용. emit은 그 스레드의 push_signal(단일 생산자 경로).
    void        drain_manual_inbox(const std::function<void(const OrderSignal&)>& emit);

    std::atomic<uint64_t> data_count_{0};
    std::atomic<uint64_t> td_drop_count_{0}; // WS 체결 큐 가득으로 버린 틱 수 [why D-055]
    std::atomic<uint64_t> ob_drop_count_{0}; // WS 호가 큐 가득으로 버린 호가 수 [why D-067]
    std::atomic<uint64_t> signal_count_{0}; // 신호 순번 자체는 strategy_thread의 SignalDispatcher가 찍는다 [why D-063]
    std::atomic<uint64_t> order_count_{0};

    OrderGate order_gate_;
    std::unique_ptr<OrderRouter> order_router_; // 주문 전처리·중계 레이어(증권업계 용어로 FEP, Front-End Processor). start() 이후 유효
    RegimeController::Config regime_cfg_{};     // 국면 판정 파라미터(config 주입, 미지정=기본값)
    std::unique_ptr<RegimeController> regime_;  // 국면 메타레이어 (start() 이후 유효)

    // 전략에서 수집한 구독 스펙 (on_start 이후 확정)
    //  data_thread(재스캔 등록)가 쓰고 control_thread(WS 재연결)가 읽는다 — watch_specs_mtx_로 보호.
    std::vector<WatchSpec> watch_specs_;
    mutable std::mutex     watch_specs_mtx_;

    std::string zmq_bind_addr_ = "127.0.0.1";
    std::string zmq_control_token_;

    // 티커→종목명 라벨(로그 표시용). 여러 스레드가 접근해 ticker_names_mu_로 보호.
    std::unordered_map<std::string, std::string> ticker_names_;
    mutable std::mutex ticker_names_mu_;

    // 종목별 최근 체결가(원 단위)와 받은 시각. 전략 스레드가 td_queue_를 비우며 쓰고, 데이터 스레드가
    //  틱이 끊긴 보유 종목을 REST로 보충하고, OpsServer 스레드가 POSITIONS 현재가·수동주문 ref_price로
    //  읽는다 — last_px_mu_로 보호. 틱이 없던 종목은 0.
    struct LastPx
    {
        double                                px = 0.0;
        std::chrono::steady_clock::time_point at{};
    };

    std::unordered_map<std::string, LastPx> last_px_;
    mutable std::mutex                      last_px_mu_;
    double                                  last_px(const std::string& ticker) const;
    void                                    set_last_px(const std::string& ticker, double px);
};
