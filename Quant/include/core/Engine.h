#pragma once
#include "api/KisClient.h"
#include "api/KisWebSocket.h"
#include "core/CommandLine.h"
#include "core/ControlPlane.h"
#include "core/DataPoller.h"
#include "core/LedgerReconciler.h"
#include "core/RingBuffer.h"
#include "core/WakeGate.h"
#include "core/OrderRateLimiter.h"
#include "core/SignalDispatcher.h"
#include "core/SymbolTable.h"
#include "core/TickCapture.h"
#include "core/UniverseRescan.h"
#include "core/ReplaySource.h"
#include "core/PaperExecutor.h"
#include "core/PrefetchPool.h"
#include "core/FeedMux.h"
#include "core/FeedSupervisor.h"
#include "core/SessionEndJudge.h"
#include "core/StrategyRouter.h"
#include "core/ShardRoutes.h"
#include "core/StrategyShard.h"
#include "core/RegimeFileJudge.h"
#include "core/LatencyTrace.h"
#include "core/Types.h"
#include "core/WebSocketSlotPlan.h"
#include "risk/OrderGate.h"
#include "risk/ProtectiveOrders.h"
#include "strategy/StrategyBase.h"
#ifdef HAS_ZMQ
#include "ipc/ZmqBridge.h"
#endif
#include "ipc/DbManager.h"
#include "ipc/Heartbeat.h"
#include "ipc/OrderChannel.h"
#include "ipc/ControlChannel.h"
#include "ipc/LedgerSnapshot.h"
#include "ipc/SharedLayout.h"
#include "ipc/SharedRegion.h"
#include "ipc/OrderRouter.h"
#include "ipc/OpsServer.h"
#include "core/MpscQueue.h"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <map>
#include <memory>
#include <condition_variable>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct AppConfig;

// ─────────────────────────────────────────────────────────────────────────────
// Engine  —  퀀트 트레이딩 엔진
//
//  [데이터 스레드]  KIS REST 일봉·대체 틱   → bars_matrix·trade_matrix (행렬의 데이터 스레드 행)
//  [샤드 스레드 m]  order_book_matrix + trade_matrix + bars_matrix 열 m → 전략 → shard_out
//  [전략 스레드]    shard_out + 수동주문 → 디스패처(순번·슬롯·교체) → 요청 면
//  [주문 스레드]    요청 면 → KIS REST 주문 (KR/US 자동 분기)
//
//  WS 구독 목록은 on_start() 이후 전략의 get_watch_specifications()로 동적 수집
// ─────────────────────────────────────────────────────────────────────────────

// 주문 큐에서 이 시간을 넘게 기다린 신규 매수는 꺼낼 때 버린다. 큐가 찬 동안 증권사에 낼 수 있는 건수는
//  초당한도로 고정이라 늘릴 수 없고, 남은 예산을 2초 전 판단에 쓰면 그만큼 지금 판단이 못 나간다.
//  취소·정정과 매도(손절·청산)는 나이를 안 본다 — 늦어도 보내야 하는 주문이다. [why D-127]
inline constexpr int64_t kOrderSignalMaxAgeNs   = 1'000'000'000; // 1초
inline constexpr int64_t kNanosecondsPerMillisecond = 1'000'000; // 로그에 ms로 적을 때 쓰는 나눗수

// 지금 꺼내 보내기엔 너무 오래된 신호인가. 시각을 안 찍은 신호(signal_at_ns=0)는 나이를 모르니 보낸다.
constexpr bool is_stale_entry(const OrderSignal& signal, int64_t popped_at_ns)
{
    return signal.action == OrderAction::NEW && signal.side == OrderSide::BUY && signal.signal_at_ns != 0
           && popped_at_ns - signal.signal_at_ns > kOrderSignalMaxAgeNs;
}

class Engine
{
public:
    Engine(KisConfig kis_config, int fetch_interval_sec = 60);
    ~Engine();

    // AppConfig(typed 값)를 아래 세터들에 한 번에 옮긴다 — start() 전에만. 몸통은 src/core/EngineConfigure.cpp.
    void configure(const AppConfig& app);

    // 스레드·뮤텍스를 소유한다 — 복사는 원본과 사본이 같은 자원을 두 번 닫는 길이라 막는다.
    Engine(const Engine&)            = delete;
    Engine& operator=(const Engine&) = delete;

public:
    // ── 이 프로세스가 맡는 자리 ─────────────────────────────────────────────
    // start() 전에만 부른다. Both 는 지금까지의 한 프로세스다. [why D-114]
    void set_role(ProcessRole role);

    [[nodiscard]] ProcessRole role() const noexcept
    {
        return role_;
    }

    // 이 프로세스가 맡는 일감. Both 면 셋 다 참이다 — 지금까지의 한 프로세스와 같다.
    //  주문 쪽은 주문·체결·원장·게이트, 전략 쪽은 전략·신호, 시세 쪽은 WebSocket 소켓과 디코드다
    //  (가르는 선은 docs/DECISIONS.md D-114).
    // **긍정형이다** — 열거한 역할만 참이다. 부정형으로 두면 역할이 늘 때 새 역할이 조용히 참이 되어
    //  시세만 맡을 프로세스가 주문 스레드·원장까지 띄운다. 판정 본문은 ProcessRole 쪽에 있다. [why D-114]
    [[nodiscard]] bool runs_order_side() const noexcept
    {
        return role_.runs_order_side();
    }

    [[nodiscard]] bool runs_strategy_side() const noexcept
    {
        return role_.runs_strategy_side();
    }

    [[nodiscard]] bool runs_feed_side() const noexcept
    {
        return role_.runs_feed_side();
    }

    // ── 티커 문자열이 종목 번호가 되는 자리 ─────────────────────────────────
    // 종목 표에 **넣는 쪽은 주문 프로세스 하나**다(D-114 단계 4). 전략 프로세스는 읽기만 한다 —
    //  양쪽이 각자 번호를 찍으면 전략 쪽 3번과 주문 쪽 3번이 다른 종목이 되고, 그건 엉뚱한 종목에
    //  주문이 나가는 것이다. 그래서 티커를 번호로 바꾸는 자리를 이 둘로만 모았다.

    // 없으면 넣어서라도 번호를 받아 온다. **느린 경로 전용**이다 — 기동·재스캔·바스켓처럼 분당 몇 건인
    //  자리에서만 부른다. 전략 역할이면 넣기를 주문 쪽에 맡기고 표에 뜰 때까지 잠깐 기다린다.
    //  못 받으면 symbol::kNone 이다(부른 쪽이 그 줄을 접는다).
    [[nodiscard]] symbol::SymbolId register_symbol(std::string_view ticker);

    // 표에 있으면 번호, 없으면 symbol::kNone. **넣지 않는다** — 틱·신호처럼 잦은 자리에서 부른다.
    //  없는 티커가 오는 것은 등록 경로가 빠진 것이라 세어서 드러낸다.
    [[nodiscard]] symbol::SymbolId lookup_symbol(std::string_view ticker) noexcept;

    // 등록을 주문 쪽에 맡겼다가 못 받은 횟수 / 표에 없는 티커로 잦은 자리가 불린 횟수.
    [[nodiscard]] uint64_t symbol_register_timeouts() const noexcept
    {
        return symbol_register_timeouts_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t symbol_lookup_misses() const noexcept
    {
        return symbol_lookup_misses_.load(std::memory_order_relaxed);
    }

    // 시세 역할이 구독 목록에 없는 종목을 받아 버린 횟수.
    [[nodiscard]] uint64_t unknown_ticker_dropped() const noexcept
    {
        return unknown_ticker_dropped_.load(std::memory_order_relaxed);
    }

    // 전략 이름 등록을 주문 쪽에 맡겼다가 못 받은 횟수. 0이 아니면 그 전략의 손익 귀속이 비어 있다.
    [[nodiscard]] uint64_t strategy_register_timeouts() const noexcept
    {
        return strategy_register_timeouts_.load(std::memory_order_relaxed);
    }

    // 구독 상한에 밀려 소켓에 못 건 종목 수. 0이 아니면 그 종목은 WS 틱을 못 받는다.
    [[nodiscard]] uint64_t watch_overflows() const noexcept
    {
        return watch_overflow_.load(std::memory_order_relaxed);
    }

    // 시세 통로가 차서 버린 건수(보내는 쪽) / 꺼낸 값이 말이 안 돼 버린 건수(받는 쪽).
    //  앞엣것이 늘면 전략 프로세스가 못 따라오는 것이고, 뒤엣것이 늘면 건너편을 의심한다. [why D-114]
    [[nodiscard]] uint64_t feed_channel_overflows() const noexcept
    {
        return feed_channel_overflow_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t feed_channel_discarded() const noexcept
    {
        return feed_channel_discarded_.load(std::memory_order_relaxed);
    }

    // 시세가 실제로 경계를 넘었는지 — 밀어 넣은 건수와 꺼낸 건수다. 순번은 프로세스마다 제 쪽
    //  것이라, 갈라 띄운 날에 주문 쪽에서는 밀어 넣은 수만, 전략 쪽에서는 꺼낸 수만 늘어난다. 한 프로세스로
    //  돌면 둘 다 0 이다 — 시세가 통로를 지나지 않는다. [why D-114]
    [[nodiscard]] uint64_t feed_channel_sent();
    [[nodiscard]] uint64_t feed_channel_received();

    // 통로에 쌓여 아직 안 건너간 체결 수(어림값). 한 프로세스로 돌면 늘 0이다 — 시세가 통로를 지나지 않는다. [why D-114]
    [[nodiscard]] size_t feed_channel_pending_trades(uint32_t lane);

    // 통로의 줄 수 = 소켓 수 + REST 대체 줄 하나. 마지막 줄에 넣는 쪽은 데이터 스레드다. [why D-114]
    [[nodiscard]] uint32_t feed_channel_lanes();

    // ── 체결 통로(시세 → 주문) ────────────────────────────────────────────
    // 앱키가 하나라 체결통보가 시세 소켓에 실린다 — 갈라 띄우면 체결이 이 통로로 경계를 넘는다.
    //  큐가 차서 못 넘긴 수·값이 말이 안 돼 버린 수는 둘 다 0이어야 한다. 0이 아니면 주문 쪽 예약 수량이
    //  안 풀려 총노출을 이중계상한다(단계 5의 A급 자리). 밀어 넣은 수·꺼낸 수는 시세 통로와 같은 모양으로
    //  프로세스마다 제 쪽 것만 는다. 한 프로세스로 돌면 넷 다 0이다. [why D-114 단계 5]
    [[nodiscard]] uint64_t fill_channel_overflows();
    [[nodiscard]] uint64_t fill_channel_discarded();
    [[nodiscard]] uint64_t fill_channel_sent();
    [[nodiscard]] uint64_t fill_channel_received();

    // ── 주문 쪽 스위치를 고치는 자리 ────────────────────────────────────────
    // OrderGate·원장은 주문 프로세스 것이다. 전략 역할이면 여기서 직접 고치지 않고 제어 요청 한 줄을
    //  보낸다 — 표를 고치는 일은 단일 시퀀서인 주문 스레드가 한다(D-071 원칙 4). [why D-114]
    //  Both 역할이면 지금까지처럼 그 자리에서 고친다.
    void request_reset_daily();                       // 장이 열렸다 — 하루치를 새로 연다
    void request_entry_halt(bool on);                 // 신규 진입 정지(청산·취소는 통과)
    void request_entry_scale(double entry_scale);     // 신규 진입 매수 비율 0~1
    void request_kill_switch(bool on);                // 전방향 주문 차단
    void request_manual_halt(OrderSide side, bool on); // 운영단말이 손으로 거는 한 방향 정지

    // 지금 값. 고치는 것은 위의 요청으로만 하고 읽기는 어느 쪽에서나 한다(게이트 안은 원자값이다).
    [[nodiscard]] bool   is_entry_halted() const
    {
        return order_gate_.is_entry_halted();
    }

    [[nodiscard]] double entry_scale() const
    {
        return order_gate_.entry_scale();
    }

    [[nodiscard]] bool   is_killed() const
    {
        return order_gate_.is_killed();
    }

    [[nodiscard]] bool   is_manual_sell_halted() const
    {
        return order_gate_.is_manual_sell_halted();
    }

    // ── 전략 등록 ────────────────────────────────────────────────────────────
    void add_strategy(std::unique_ptr<StrategyBase> strategy);

    size_t strategy_count() const
    {
        return strategy_.list.size();
    }

    // ── 피드 설정(스레드 시작 전에만) ───────────────────────────────────────
    // 실시간 체결가는 원래 WebSocket으로 받지만, WS 세션이 rt_cd=9(ALREADY IN USE, 중복접속)로
    // 폭주할 때의 우회책이다. true면 DataThread가 REST get_current_price(현재가 조회)를 주기적으로
    // 폴링해 그 값을 TradeData(체결 틱)처럼 trade_matrix의 데이터 스레드 행에 넣고, WS 연결은 생략한다. ITB 전략이
    // 이 틱으로 구동된다(ITB = IntradayBreakoutStrategy, 장중 돌파 전략).
    void set_rest_price_feed(bool rest_price_feed)
    {
        feed_.rest_price_feed = rest_price_feed;
    }

    // WS 틱·호가 캡처 폴더(빈 문자열이면 끔). 기동마다 ticks_<UTC시각>.bin 하나. REST 대체 틱은 raw 피드가
    //  아니라 캡처하지 않는다. [why D-071]
    void set_capture_directory(const std::string& directory)
    {
        feed_.capture_directory = directory;
    }

    // 캡처에 담을 종목만 고른다(비면 전부). [why D-138]
    void set_capture_tickers(const std::vector<std::string>& tickers)
    {
        feed_.capture_tickers = tickers;
    }

    // 전략이 안 봐도 WS 칸을 가장 앞 순위로 쥐는 종목(체결만). [why D-138]
    void set_websocket_pin_tickers(const std::vector<std::string>& tickers)
    {
        websocket_pin_tickers_ = tickers;
    }

    // 체결을 TimescaleDB에 바로 넣는 적재기 설정. enabled면 시세를 받는 프로세스(Both·Feed)만 start()에서 만든다. [why D-148]
    void set_database(const db::DbConfig& config)
    {
        database_config_ = config;
    }

    // 원장 저널 폴더 — 거래일마다 ledger_YYYYMMDD.bin 하나(틱 캡처와 달리 재기동이 같은 파일에 이어 쓴다, 다음 기동이
    //  리플레이해야 하므로). start()가 열고 리플레이하며, 못 열면 기동을 거부한다. 빈 문자열이면 저널 없이 동작
    //  (테스트·벤치). fsync는 append마다 디스크 동기화(전원 장애 방어). [why D-113]
    void set_ledger_journal(const std::string& directory, bool fsync)
    {
        ledger_journal_directory_ = directory;
        ledger_journal_fsync_     = fsync;
    }

    // 전략 샤드 수(config `strategy_shards`, 기본 1). 스레드 시작 전에만. 상한은 샤드 마스크 폭이고, 전략은 등록 순으로 샤드 하나씩 갖는다(걸침 검사 없음). [why D-110]
    void set_strategy_shards(uint32_t strategy_shards)
    {
        pipeline_.strategy_shards = strategy_shards == 0 ? 1u : strategy_shards;
    }

    // WS 소켓을 하나 더 연다(KIS는 app_key당 실시간 1세션이라 키가 하나 더 있어야 한다). 하나라도 있으면 기본 키와
    //  함께 FeedMux로 묶여 종목이 소켓들에 나뉜다 — 구독 상한(kMaxWsSubs)이 소켓 수만큼 는다. 리플레이 중엔 무시. [why D-071]
    void add_feed_config(const KisConfig& kis_config)
    {
        feed_.extra_feed_cfgs.push_back(kis_config);
    }

    // 캡처 파일 리플레이(빈 문자열이면 WS). WS 자리에 ReplaySource가 들어가 같은 콜백으로 틱·호가를 되돌린다.
    //  speed 0은 최대 속도, 1은 캡처 간격. set_feed_source와 같이 KisClient를 만들지 않는다 — 인증·계좌 없이 파일과
    //  config tickers만으로 뜨고, 주문은 PaperExecutor(현금 cash)가 다음 틱에 체결한다. [why D-071]
    void set_replay(const std::string& file, double speed, double cash)
    {
        feed_.replay_file  = file;
        feed_.replay_speed = speed;
        feed_.replay_cash  = cash;
    }

    // 피드 소스를 직접 준다 — 그러면 start()가 KisClient를 만들지 않는다(인증·토큰 갱신·계좌번호·잔고 조회 없음).
    //  주문·잔고는 리플레이와 같은 PaperExecutor(현금 cash)가 받고, 시세·차트 REST가 필요한 경로는 소스 없음으로 건너뛴다.
    //  시험용 시세로 Engine 한 바퀴를 KIS·소켓 없이 시험하는 자리. 스레드 시작 전에만. [why D-071]
    void set_feed_source(std::unique_ptr<feed::IFeedSource> source, double cash)
    {
        feed_.feed_override = std::move(source);
        feed_.replay_cash   = cash;
    }


    // 설정의 종목 목록을 그대로 구독 목록으로 깐다. 갈라 띄운 시세 프로세스에는 전략이 없어 구독 목록이
    //  비는데, 부하시험은 그 목록으로 종목 순번표를 적는다 — 비면 순번표가 0개가 되어 부하 투입기가
    //  보낼 종목을 못 찾는다. 한 프로세스판은 전략에서 같은 목록을 얻으므로 이 자리는 시세 역할 전용이다.
    //  스레드 시작 전에만. [why D-114 단계 5]
    void seed_watch_specifications(const std::vector<std::string>& tickers);

    // 브로커 없이 돈다고 못 박는다 — 피드 소스를 쥐지 않은 역할도 KisClient를 만들지 않고 모의 체결기를 쓴다.
    //  부하시험이 그 자리다: 수신단은 시세 프로세스에 있어서 주문 프로세스에는 피드 소스가 없는데,
    //  그대로 두면 주문 프로세스만 진짜 KIS에 붙어 시험 주문을 거래소로 내보낸다. 모의 체결기 현금도
    //  여기서 준다 — set_feed_source 는 시세 프로세스에서만 불려 주문 쪽 현금이 0으로 남고, 그러면
    //  들어온 주문이 전량 E_PAPER_CASH 로 거부된다(09-25 실측: 12,799건 전부). [why D-114 단계 5]
    void set_broker_offline(bool offline, double cash)
    {
        feed_.broker_offline = offline;
        feed_.replay_cash    = cash;
    }

    // ── 기동 옵션 ────────────────────────────────────────────────────────────
    // 기동 시(bootstrap) 실계좌 잔고를 내부 장부의 초기값으로 채운다(G5).
    // 프로그램을 재시작하면 OrderGate 원장이 0으로 비는데, 실계좌엔 이미 보유분이 남아있다.
    // get_balance(잔고조회)로 종목·수량·평단을 읽어 원장에 심어(seed) 실제와 장부를 맞춘다
    // (안 맞으면 매도수량·평단·손실한도 계산이 어긋난다). main이 config로 켠다.
    void set_bootstrap_ledger(bool bootstrap_ledger)
    {
        bootstrap_ledger_ = bootstrap_ledger;
    }

    // 시세 전용 클라이언트 설정(실전 도메인). KIS 모의(openapivts)는 시세 REST가 HTTP 500이라
    // 시세는 실전 키+실전 도메인으로 조회하고 주문만 모의로 낸다. rest_price_feed 폴링이 사용.
    void set_quote_kis_config(const KisConfig& quote_kis_config)
    {
        feed_.quote_kis_config = quote_kis_config;
        feed_.has_quote_kis = true;
    }

    // ── 운영 카운터·상태 조회 ───────────────────────────────────────────────
    // 운영 카운터 — data는 받은 시세 건수(WS 체결과 REST 폴링을 다 센다. 갈라 띄우면 전략 쪽은 통로에서 꺼낸 체결을 센다), signal은 주문 큐에 넣은 신호, order는 접수된 주문.
    uint64_t data_count() const
    {
        return data_count_.load(std::memory_order_relaxed);
    }

    uint64_t signal_count() const
    {
        return signal_count_.load(std::memory_order_relaxed);
    }

    uint64_t order_count() const
    {
        return order_count_.load(std::memory_order_relaxed);
    }

    // start()가 실제로 잡은 수신 스레드(행)·전략 샤드(열) 수 — config와 다를 수 있다(샤드 수가 마스크 폭을 넘으면 거기서 자른다). 기동 뒤에만 뜻이 있다.
    uint32_t websocket_lanes() const
    {
        return pipeline_.websocket_lanes;
    }

    uint32_t shard_count() const
    {
        return static_cast<uint32_t>(pipeline_.shards.size());
    }

    // 종목 id의 틱을 받는 샤드 마스크(시험·진단용). 0이면 아무 전략도 안 보는 종목이다.
    shard::ShardMask route_mask(symbol::SymbolId symbol_id) const
    {
        return pipeline_.routes.mask(symbol_id);
    }

    // 큐 수위·버린 건수 — control_thread가 1분마다 찍는 [큐 고수위] 로그와 같은 값을 그 주기를 기다리지 않고 준다.
    //  부하 하네스(bench_engine_load)가 구간마다 읽는다. 읽기 전용이라 어느 스레드에서 불러도 된다.
    struct QueueStatistics
    {
        size_t   shard_high_water = 0; // 샤드 셀 가운데 가장 높았던 수위
        size_t   shard_out_size   = 0; // 샤드 → 전략 스레드 큐의 현재 깊이
        uint64_t trade_dropped    = 0; // 수신 스레드 → 샤드 셀에서 버린 체결 수(trade_drop_count_)
        size_t   order_high_water = 0;
        size_t   fill_high_water  = 0;
        uint64_t shard_dropped    = 0;
        uint64_t order_dropped    = 0;
        uint64_t order_stale      = 0; // 큐에서 너무 오래 기다려 버린 신규 매수 수 [why D-127]
        uint64_t order_sell_held  = 0; // 큐가 가득 차 버리지 않고 들고 있던 매도 수(자리가 나면 먼저 넣는다)
        uint64_t fill_dropped     = 0;
        uint64_t order_duplicate  = 0; // 주문 쪽이 같은 순번을 두 번 받아 거른 수. 0이 아니면 통로가 샜다
        uint64_t order_response_dropped = 0; // 전략이 답을 안 가져가 버린 수
        int64_t  strategy_beat_gap_max_ns = 0; // 전략 박동의 가장 긴 공백. 사망 문턱의 근거
        int64_t  order_beat_gap_max_ns    = 0; // 주문 박동의 가장 긴 공백. 증권사 왕복이 그대로 들어온다
        int64_t  feed_beat_gap_max_ns     = 0; // 시세 박동의 가장 긴 공백. 제어 바퀴 5초가 그대로 들어온다
        uint64_t order_answer_overdue     = 0; // 시한을 넘겨도 답이 안 온 요청을 본 횟수. 0이 아니면 답이 샌다
    };

    QueueStatistics queue_statistics() const;

    // ── 매크로 레짐 브리지 ──────────────────────────────────────────────────
    // 매크로 레짐 브리지(2026-08-09 회의 Task 3). 국면 판정 피드(regime/RegimeFeed.h)가
    // 원자적으로 쓰는 regime.json 경로를 지정하면, data_thread가 매 사이클 그 파일을 읽어
    // 시장이 위험하면 OrderGate 의 "신규매수 정지" 스위치(entry_halt)를 켜고, 풀리면 끈다
    // (매수만 막고 청산·매도는 그대로 통과). path 빈 문자열이면 기능 미가동(기본).
    // stale_sec(기본 kDefaultRegimeStaleSec)보다 오래된 파일은 보조 프로세스가 죽은 것으로 보고 무시한다.
    //  판정 규칙은 core/RegimeFileJudge.h가 소유한다. [why D-060]
    void set_regime_file(const std::string& path, int stale_sec = kDefaultRegimeStaleSec);

    // 매크로 진입정지의 시간 상자. 개장 후 이 분수가 지나면 entry_halt를 스스로 풀고,
    // 그날 매크로 축은 다시 halt를 걸지 못한다. 그 뒤 통제는 장중을 실제로 보는 축
    // (UniverseScanner 코스피 게이트·종목 정배열)이 갖는다. force_liquidate는 대상이 아니다.
    // 0 이하면 만료를 끈다. [why D-033]
    void set_regime_halt_expire_min(int regime_halt_expire_min)
    {
        regime_file_judge_.set_halt_expire_min(regime_halt_expire_min);
    }

    // ── 리스크·주문 ──────────────────────────────────────────────────────────
    // 주문 호출 간격 조절/재시도 (C-2/W-3) — 버스트 청산이 초당한도로 튕겨 유실되는 것 방지.
    //  min_interval_ms 간격으로만 발주(레이트리밋 하회), 거부된 청산 SELL은 order_thread
    //  로컬 큐로 deduplicate 창 밖에서 최대 max_retries회 재시도. 스레드 시작 전에만 호출.
    void set_order_interval(int min_interval_ms, int max_retries)
    {
        order_min_interval_ms_ = min_interval_ms;
        order_max_retries_ = max_retries;
    }

    // OrderGate 위험 한도를 config로 주입(스레드 시작 전에만). 기본값은 OrderGate::Config.
    void set_risk_config(const OrderGate::Config& risk_config)
    {
        order_gate_.set_config(risk_config);
    }

    // 마감 자기 종료 — 마지막 매매 창이 닫히는 분과 유예 초(스레드 시작 전에만). close_min 0이면 판정 없음. [why D-098]
    void set_session_end(int close_min, int grace_sec);

    // 슬롯 수 조회 — 스캐너가 "베이스 총합이 목표 노출을 넘지 않도록" 배수를 정규화할 때 쓴다.
    //  위험 config는 전략 로딩보다 먼저 주입되므로(main.cpp) 이 시점에 이미 유효하다.
    int risk_max_positions() const
    {
        return order_gate_.config().max_concurrent_positions;
    }

    // 점수 랭크를 게이트에 주입 — 슬롯이 꽉 차갈수록 상위 점수만 통과시킨다.
    //  같은 표를 로그 폴더 entry_scores.json에도 남겨 대시보드가 보유 종목을 점수순으로 보인다. [why D-047]
    //  항목은 종목 id(symbols().intern)·랭크·z — 문자열 표는 없다. [why D-112]
    void set_entry_priority(const std::vector<OrderGate::PriorityEntry>& entries, int total);
    // 현재 보유(롱) 원장 스냅샷 — 유니버스 재스캔의 "보유분 제외"가 매회 최신 잔고를 보게 한다.
    //  기동 시 1회 조회한 잔고를 계속 쓰면 청산된 종목이 세션 내내 후보에서 빠진다.
    std::vector<OrderGate::HeldPos> held_positions() const
    {
        return order_gate_.ledger().snapshot_positions();
    }

    // 종목 테이블 — 문자열 티커는 경계(설정·잔고·스캔 응답)에서 여기로 한 번 번호가 되고 그 뒤로는 id로 다닌다. [why D-112]
    symbol::SymbolTable&       symbols() noexcept
    {
        return symbols_.table;
    }

    const symbol::SymbolTable& symbols() const noexcept
    {
        return symbols_.table;
    }

    // 바스켓 슬리브 소유 종목 — 슬롯·교체·강제청산 밖(OrderGate::set_slot_exempt). 바스켓 로더가 기동 때, 전략이 파일을
    //  다시 읽을 때 넣고, DEVSCALE 재스캔이 이 목록을 유니버스에서 뺀다(같은 종목을 두 슬리브가 들지 않게). [why D-109]
    //  전략 쪽에서 부르는 자리라 게이트를 바로 고치지 않고 제어 요청으로 보낸다 — 표를 고치는 것은
    //  주문 스레드 하나다(원칙 4). 건 결과는 장부 사본의 slot_exempt 비트로 돌아온다. [why D-114]
    void set_slot_exempt_tickers(const std::vector<std::string>& tickers);
    std::vector<symbol::SymbolId> slot_exempt_symbols() const
    {
        return order_gate_.ledger().slot_exempt_symbols();
    }

    // ── 유니버스 재스캔 ─────────────────────────────────────────────────────
    // 주기적 유니버스 재스캔(동적 등록). universe_fn: 시세 클라이언트로 유니버스 티커 목록 산출.
    // factory: 티커 → 전략 인스턴스 생성. interval_sec: 재스캔 주기(초, ≤0이면 비활성).
    // data_thread가 interval_sec마다 universe_fn을 호출해 신규 티커만 런타임 등록한다.
    // max_registered: 등록 전략 총수 상한(0이면 무제한). 등록 하나당 프리페치 스레드와 실시간
    //  구독이 붙으므로 상한이 없으면 하루가 갈수록 조회량이 늘어난다.
    // drop_after_sec: 스캔 결과에서 이만큼 연속으로 빠져 있는 종목의 전략을 뗀다(≤0이면 안 뗌).
    //  보유·미체결 선점이 있는 종목은 빠져 있어도 안 뗀다. 20초 재스캔에 한 번 빠졌다고 떼면
    //  등록(차트 조회)과 해제가 도는 회전이 나므로 유지 시간을 둔다.
    // block_after_sec: 같은 시계로 이만큼 빠져 있으면 떼기 전에 신규매수부터 막는다(≤0이면 안 막음).
    //  return_confirm: 막힌 종목이 다시 보여도 이 횟수 연속이어야 푼다. 판정은 core/UniverseExit.h [why D-077].
    //  universe_fn·factory는 종목 id로 말한다 — 스캐너가 응답의 문자열 티커를 symbols()에 한 번 넣고 id를 준다.
    void set_universe_rescan(
        std::function<std::vector<symbol::SymbolId>(KisClient&)> universe_fn,
        std::function<std::unique_ptr<StrategyBase>(symbol::SymbolId)> factory,
        int interval_sec, size_t max_registered = 0, int drop_after_sec = 0,
        int block_after_sec = 0, int return_confirm = 2);

    // 기동 때 add_strategy로 넣은 유니버스 종목을 마지막 set_universe_rescan 슬리브의 소유로 잡는다.
    //  재스캔이 등록한 종목만 소유로 두면 기동 종목은 하루 종일 차단·해제 밖이라 순위에서 밀려도 남는다.
    //  스레드 시작 전에만, set_universe_rescan 바로 뒤에 부른다 [why D-077].
    void seed_universe_rescan(const std::vector<symbol::SymbolId>& symbols);

    // ── G1: 국면→전략 자동선택 ──────────────────────────────────────────────
    // 국면(BULL/NEUTRAL/BEAR)별 활성 전략 id 목록(권위적 선택자). 스레드 시작 전에만.
    //  현재 국면 목록에 든 전략만 활성, 나머지 비활성.
    //  id 항목이 '*'로 끝나면 접두 매칭(스캐너 동적 id: "DevScale_*", "ITB_*").
    //  맵이 비면(미지정) 기존 per-strategy active_regimes 방식으로 폴백(하위호환).
    void set_regime_strategies(std::map<Regime, std::vector<std::string>> regime_strategies)
    {
        strategy_.regime_map = std::move(regime_strategies);
        strategy_.has_regime_map = !strategy_.regime_map.empty();
    }

    // ZMQ 제어 채널. bind 주소 기본 127.0.0.1(모든 인터페이스 노출 금지), token이 비면 KILL은
    //  거부된다(config `zmq_control_token`). 스레드 시작 전에만. HAS_ZMQ가 꺼진 빌드에선 무시.
    void set_zmq_control(const std::string& bind_address, const std::string& token);

    // ZMQ 포트 넷. 한 기계에 엔진이 둘이면 뒤에 뜬 쪽이 bind에 실패하므로 설정으로 뺐다. 역할마다 발행
    //  포트가 하나씩이고(주문·시세·전략) 제어(REP)는 주문 쪽에만 있다 — 한 포트로 모으면 그 자리가
    //  죽을 때 셋이 같이 멎어 프로세스를 가른 뜻이 없어진다. [why D-114]
    struct ZmqPorts
    {
        int order_pub    = 5555; // config `zmq_pub_port`
        int order_rep    = 5556; // config `zmq_rep_port`
        int feed_pub     = 5557; // config `zmq_feed_pub_port`
        int strategy_pub = 5558; // config `zmq_strategy_pub_port`
    };

    void set_zmq_ports(ZmqPorts ports)
    {
        zmq_ports_ = ports;
    }

    // 보호 주문 표(config `protective_orders`) — off/shadow/owner. 전략이 멈춰도 주문 쪽이 표만 보고
    //  손절·트레일 청산을 낸다. 스레드 시작 전에만. [why D-114]
    static constexpr int kProtectiveIntervalMsDefault = 200;   // 표를 보는 간격
    static constexpr int kProtectiveRetryMsDefault    = 30000; // 청산이 안 먹힐 때 다시 내는 간격

    void set_protective_orders(const std::string& mode, int interval_ms, int retry_ms);

    // ZMQ 발행·제어 채널을 아예 열지 않는다(스레드 시작 전에만). 구독자 없이 도는 부하 하네스에서
    //  발행 큐가 차며 나는 drop 로그가 측정을 가리기 때문이다. 라이브는 기본값(켜짐) 그대로 쓴다.
    void set_zmq_enabled(bool enabled)
    {
        zmq_enabled_ = enabled;
    }

    // 한 기계에서 계좌를 둘 이상 돌릴 때 이 엔진을 가르는 이름(config `instance`, 예 "live").
    //  마감 표지 파일 이름에 붙는다 — 모의 엔진이 15:30에 남긴 마감 표지를 실계좌 감시견이 읽고
    //  20:00까지 도는 실계좌 트레이더를 되살리지 않던 것을 막는다. [why D-122]
    void set_instance(std::string name)
    {
        instance_ = std::move(name);
    }

    // 운영단말 TCP 채널(config `ops_bind_addr`·`ops_port`·`ops_token`). port 0이면 열지 않는다.
    //  스레드 시작 전에만. 루프백이 아닌 주소는 token이 있어야 서버가 뜬다(OpsServer::start).
    void set_ops_control(const std::string& bind_address, int port, const std::string& token)
    {
        ops_.bind_address = bind_address;
        ops_.port      = port;
        ops_.token     = token;
    }

    // 운영단말이 낸 수동주문을 받는다 — 값만 보고 인테이크에 넣는다. 통과면 빈 문자열, 아니면 단말에 보일
    //  거부 사유. 내는 것은 주문 스레드가 take_manual_order로 꺼내서 한다. [why D-043][why D-114]
    std::string accept_manual_order(const OpsOrderReq& ops_order_request);

    // ── 종목명·청산 관리 티커 ───────────────────────────────────────────────
    // 티커→종목명 매핑 등록/조회 (로그 가독성). 스캔·청산 관리 부착 스레드가 write,
    //  전략 스레드의 신호 로그가 read라 ticker_names_mu_로 보호.
    // 청산 관리가 붙은 티커. 이 종목은 그날 스캔 슬리브의 신규매수 대상에서 뺀다.
    //  청산 관리(청산 전용)과 스캔 전략(진입)이 같은 티커에 동시에 붙으면 한쪽이 턴 것을
    //  다른 쪽이 곧바로 되사서 수수료만 나간다(2026-09-08 금호건설: 13:39:59 전량매도 →
    //  13:40:12 재매수). 기동 시 단일스레드 구간에서만 채우고 전략 스레드는 읽기만 한다.
    //  종목 id 인덱스 비트 — 잔고 응답의 문자열 티커는 여기서 한 번 번호가 된다.
    void mark_exit_managed_ticker(const std::string& ticker)
    {
        mark_exit_managed(symbols_.table.intern(ticker));
    }

    void mark_exit_managed(symbol::SymbolId symbol);

    bool is_exit_managed(symbol::SymbolId symbol) const
    {
        return symbol < symbols_.exit_managed.size() && symbols_.exit_managed[symbol];
    }

    //  이름은 종목 id 인덱스 배열에 둔다 — 문자열 티커를 받는 겹정의는 경계(로그 라벨·잔고 응답)용이고 안에서 id로 바꾼다.
    void register_ticker_name(symbol::SymbolId symbol, const std::string& name);
    void register_ticker_name(const std::string& ticker, const std::string& name);

    // 이름이 있으면 "티커(종목명)", 없으면 티커 원문을 반환.
    std::string ticker_label(symbol::SymbolId symbol) const;
    std::string ticker_label(const std::string& ticker) const;
    // 등록된 종목명(hts_kor_isnm)만 반환, 없으면 빈 문자열. 전략에 이름을 주입해 로그에 노출할 때 사용.
    std::string ticker_name(symbol::SymbolId symbol) const;
    std::string ticker_name(const std::string& ticker) const
    {
        return ticker_name(symbols_.table.lookup(ticker));
    }

    // ── 수명주기 ─────────────────────────────────────────────────────────────
    void start();
    void stop();

    bool is_running() const
    {
        return running_.load();
    }

    // running_ 내리고 다섯 스레드에 정지 요청. KILL 핸들러·시그널 핸들러·마감 판정·stop()이 부른다. join은 stop()만.
    //  reason은 로그 한 줄로 남는다 — 종료가 요청된 것인지 죽은 것인지 로그만으로 가르기 위해서다(D-20). [why D-098]
    //  recorded_reason 은 공유 쪽지에 남길 사유다. 먼저 부른 쪽 것이 남고, 적는 것은 stop() 끝이다 —
    //  여기서 적으면 그 뒤에 죽어도 정상 종료로 보인다. [why D-114]
    void request_shutdown(std::string_view         reason,
                          ipc::SharedShutdownReason recorded_reason = ipc::SharedShutdownReason::kOperator);

    // 보호 주문 한 주기를 부른 스레드가 맡는다. 평소 주인은 전략 스레드고 전략이 죽으면 주문 스레드가 이어받는데,
    //  멈췄던 전략이 깨어나면 둘 다 살아 있을 수 있다 — 시각을 원자로 밀어 잡은 쪽만 참을 받는다.
    //  같은 주기를 둘 다 잡으면 같은 청산이 두 번 나간다(A등급). 공개는 test_engine이 경합을 직접 보기 위해서다. [why D-114]
    bool claim_protective_cycle(std::chrono::steady_clock::time_point now);

private:
    // ── start() 단계 분리 (가독성용, 로직은 그대로) ────────────────────────────
    void setup_shards();

    // 소켓 수 = WS 수신 줄 수. 소켓을 만들기 전에 필요해 설정으로 센다(리플레이·소켓 하나면 1,
    //  feed_keys 가 있으면 1+N). 자리표를 깔 때와 샤드 행렬을 잡을 때가 같은 수를 써야 해서 한 벌로 둔다.
    [[nodiscard]] uint32_t websocket_lane_count() const;

    // 종목 표·전략 이름표를 공유 쪽지 위 한 벌로 바꾼다(갈라 띄울 때만). [why D-114]
    void adopt_shared_dictionaries();

    // 전략 쪽 넣기 — 주문 쪽에 넣어 달라 부탁하고 번호가 같은 표에 뜨는 것을 본다.
    //  [inv] 전략 프로세스에서만 부른다(adopt_shared_dictionaries 가 꽂는 hook).
    symbol::SymbolId           request_symbol_registration(std::string_view ticker);
    strategy_table::StrategyId request_strategy_registration(std::string_view name);

#ifdef HAS_ZMQ
    void setup_zmq_bridge();
#endif

    bool authenticate_feed(bool offline);
    void setup_paper_executor(bool offline);
    void initialize_order_router();
    void initialize_ledger_reconciler();
    void initialize_data_poller();
    bool try_bootstrap_ledger();
    bool try_open_ledger_journal(); // 저널 열기·리플레이. 거짓이면 기동 거부 [why D-113]
    void resolve_open_intents();    // 리플레이가 남긴 미결 주문을 KIS 미체결과 맞춘다 [why D-113]
    std::string ledger_journal_directory_;
    bool        ledger_journal_fsync_ = false;
    void start_strategies();
    void collect_watch_specifications();

    // 구독 스펙 하나를 내 목록에 넣는다. 이미 있으면 거짓 — 같은 종목을 두 번 구독하지 않는다.
    bool add_watch_specification(const WatchSpec& specification);

    // 구독 스펙 하나를 소켓 쥔 쪽에 보낸다. 소켓이 어느 프로세스에 있든 거는 자리는 하나다. [why D-114]
    void send_watch_request(const WatchSpec& specification, ipc::ControlKind kind = ipc::ControlKind::kWatchSubscribe,
                            int32_t priority = 0);

    // 쌓인 구독 스펙을 소켓에 건다. [inv] 감시 스레드만 부른다(주문 쪽). [why D-114]
    void drain_pending_subscriptions();

    // 구독 칸 우선순위(보유 → 선점 → 점수 순위)를 매겨 바뀐 것만 시세 쪽에 보내고, 어느 전략도 안 보고 보유·선점도
    //  없는 종목은 구독 해지를 보낸다. [inv] 재스캔을 도는 data_thread만 부른다. [why D-132]
    void publish_watch_priorities();

    // 받은 우선순위로 칸을 다시 나눈다 — 내줄 종목은 해제하고 REST 넘침으로, 받을 종목은 구독하고 넘침에서 뺀다.
    //  [inv] 감시 스레드만 부른다(apply_feed_control_requests·drain_pending_subscriptions와 같은 스레드). [why D-132]
    void rebalance_websocket_slots();
    // 시세 쪽이 아는 종목의 칸 우선순위. 모르면 kUnranked. 재연결 때 우선순위 순으로 다시 걸려고 쓴다.
    int32_t websocket_slot_priority(const WatchSpec& specification) const;

    void connect_feed();
    void spawn_threads();
    void start_rest_poll_loop(); // 피드 쪽이면 폴러 조회 스레드를 1초 바퀴로 띄운다 [why D-138]

    // 시세 한 건을 이 종목을 보는 샤드 전부에 나눠 넣는다(경로표 `routes`). 한 프로세스로 돌면 소켓
    //  수신 스레드가, 갈라 띄우면 전략 쪽 줄 스레드가 부른다 — 어느 쪽이든 행렬 그 행의 생산자는 하나다.
    //  [inv] 같은 줄(lane)을 두 스레드가 부르지 않는다. [why D-114]
    void fan_out_trade(uint32_t lane, const TradeData& trade);
    void fan_out_order_book(uint32_t lane, const OrderBook& order_book);

    // 갈라 띄운 주문 쪽이 시세 한 건을 통로에 넣는다(전략 프로세스가 꺼낸다). 큐가 차면 버리고 센다 —
    //  여기서 기다리면 그 소켓의 전 종목 시세가 같이 선다(원칙 3). [why D-114]
    void push_feed_trade(uint32_t lane, const TradeData& trade);
    void push_feed_order_book(uint32_t lane, const OrderBook& order_book);

    // ── 스레드 진입점 ────────────────────────────────────────────────────────
    // 다섯 스레드는 stop_token으로 정지를 본다. running_은 엔진 밖(main 루프·KILL 핸들러·폴러)이 읽는 깃발 [why D-070]
    void data_thread_fn(std::stop_token stop_token);
    void strategy_thread_fn(std::stop_token stop_token);
    void shard_thread_fn(std::stop_token stop_token, uint32_t column); // 행렬 열 m을 비워 전략을 돌리고 신호를 shard_out에 넣는다 [why D-071]

    // 줄 하나(소켓 하나)의 시세를 통로에서 꺼내 행렬에 나눠 넣는다. 전략 역할에만 뜬다 — 그 프로세스에는
    //  소켓이 없어 이 스레드가 행렬 그 행의 생산자다. [why D-114]
    void feed_lane_thread_fn(std::stop_token stop_token, uint32_t lane);
    void order_thread_fn(std::stop_token stop_token);
    // 갈라 띄운 주문 프로세스에서 접수된 주문 한 건을 그 값의 틱으로 모의 체결기에 먹인다 — 이 프로세스에는
    //  시세가 오지 않아 대기 주문이 영영 안 찬다. 주문 스레드가 접수를 마친 뒤에 부른다. [why D-114 단계 5]
    void feed_paper_fill_tick(const OrderSignal& signal);

    // 체결통보 한 건을 갈 길로 넣는다 — 시세 역할이면 공유 통로로, 아니면 프로세스 안 큐로.
    //  WS 콜백과 모의 체결기 콜백이 같이 쓴다(주문 역할에는 WS가 없다). [why D-114 단계 5]
    void push_fill_notification(const FillNotification& fill_notification);
    bool push_fill_notice(const ipc::FillNotice& notice); // 갈 길 하나에 한 건 넣기. 가득 차면 false
    // 넘침 목록을 앞에서부터 갈 길로 옮긴다. 다 옮겼으면 true.
    //  [inv] 부르는 쪽이 pipeline_.fill_producing 차례를 쥐고 있다.
    bool drain_fill_overflow();
    // 제어 스레드가 5초마다 부른다 — 체결통보가 더 오지 않아도 넘침 목록이 비게 한다.
    //  차례를 다른 쪽이 쥐고 있으면 그쪽이 비우므로 이번엔 건너뛴다.
    void flush_fill_overflow();

    void ledger_thread_fn(std::stop_token stop_token);   // 장부 사본 발행(읽는 쪽에 주는 복사본). 주문 스레드에서 뗀 것 [why D-114]
    void fill_thread_fn(std::stop_token stop_token);     // 체결통보 소비(fill_queue → OrderRouter::on_fill → ops 방송). WS 수신 스레드에서 뗀 것 [why D-056]
    void control_thread_fn(std::stop_token stop_token); // WebSocket 시세단절 감지·재연결(연속 실패 시 kill switch). ZMQ REP 처리는 ZmqBridge 내부 스레드 담당

    // 보호 주문 표 한 주기 — 원장 보유 스냅샷·현재가로 청산을 만들어 디스패처로 보낸다. strategy_thread 전용. [why D-114]
    void run_protective_orders(SignalDispatcher& dispatcher, std::chrono::steady_clock::time_point now);
    std::vector<OrderSignal> build_protective_orders(std::chrono::steady_clock::time_point now);

    // ── 제어 요청 (전략 쪽 → 주문 스레드) ───────────────────────────────────
    //  싣기·옮기기·적용은 control_plane_(Quant/include/core/ControlPlane.h)이 한다. 여기는 Engine 몫만 남는다.
    // 하루치를 새로 여는 실제 손질. [inv] 주문 쪽에서만 부른다(Both 의 data_thread 또는 order_thread).
    void apply_reset_daily();

    // 전략 → 시세 제어 줄을 비운다. 구독·해지 낱말만 이 줄로 오므로 받는 자리도 여기 하나다.
    //  소켓을 쥔 쪽의 감시 스레드가 부른다 — 목록에만 올리고 소켓에 거는 것은 이어지는
    //  drain_pending_subscriptions()가 한다. [why D-114 단계 5]
    void apply_feed_control_requests();

    // 전략 생사에 따라 주문 쪽 마무리를 켜고 끈다. 주문 스레드는 안 내려간다 — 보유분을 지키는 것이 남은 일이다.
    //  [inv] order_thread에서만 부른다(OrderRouter::submit의 단일 스레드 규약). [why D-114]
    void track_strategy_liveness(ipc::HeartbeatMonitor::Step step, bool just_died,
                                 std::chrono::steady_clock::time_point now);

    // 시세 생사에 따라 신규 진입을 끊고 푼다. 전략 쪽과 달리 보호 주문은 걸지 않는다 — 현재가가 멎어
    //  청산선 판단의 근거가 없기 때문이다. 하는 일은 진입을 끊고 남기는 것 둘뿐이다.
    //  [inv] order_thread에서만 부른다. [why D-137]
    void track_feed_liveness(ipc::HeartbeatMonitor::Step step, bool just_died);

    // ── 전략 레지스트리·국면·유니버스 보조 ─────────────────────────────────
    //  계좌 인자는 장부 사본을 읽게 된 뒤로 쓰지 않는다 — 한 판은 한 계좌만 담는다. 전략에 주는 함수 모양이라
    //  자리는 남겨 둔다(전략마다 고치지 않게). [why D-114]
    StrategyBase::SellableInfo ledger_sellable(const std::string& account, const std::string& ticker) const;

    // 전략이 보는 보유 수량 — 문자열 티커로 묻는 자리. 사본 한 줄을 읽는다.
    int ledger_position(const std::string& ticker) const;
    // 매크로 레짐 파일 읽기 → RegimeFileJudge 판정 → OrderGate entry_halt·force_liquidate_ 적용 (data_thread 전용)
    void poll_regime_file();
    // 전략 번호·청산 관리 여부를 등록 때 한 번 정한다 — 신호 봉투가 이 값을 싣는다. [why D-112]
    // 참이면 번호를 받았다. 거짓이면 이름표가 찼거나 건너편이 답을 안 준 것이라 등록하지 않는다 —
    //  번호 0으로 등록하면 그 전략의 주문이 전량 버려진다(2026-09-25 부하시험 52,916건). [why D-114]
    [[nodiscard]] bool assign_strategy_identity(StrategyBase& strategy);
    void maybe_rescan_universe();  // 주기적 유니버스 재스캔 → 신규 티커 런타임 등록·이탈 티커 해제 (data_thread 전용)
    // 전략 해제는 두 단계다. 뗄 때는 strategy_.list에서 빼고 strategy_.retired로 옮기며 버전을 올린다 —
    //  strategy_thread의 옛 스냅샷이 아직 그 포인터를 들고 있을 수 있어서 바로 지우지 않는다.
    //  strategy_thread가 새 스냅샷을 만든 뒤(strategy_.seen_version ≥ 뗀 시점 버전) on_stop·파기한다.
    //  force=true는 종료 경로(strategy_thread 합류 뒤)에서 전부 비운다.
    void reap_retired(bool force);
    // 전략 → 샤드 배정을 마치고 종목 → 샤드 마스크 표를 다시 만든다. strategy_.mutex 하에서(스레드 시작 전은 락 없이). [why D-110]
    void rebuild_routes_locked();
    // 새 전략에 샤드를 하나 준다 — 등록 순 라운드로빈. 스레드 시작 전·strategy_.mutex 하에서만.
    void assign_shard(StrategyBase& strategy);
    // G1: 현재 국면 r에 맞춰 전략별 active 플래그 재선택. 선택 결정을 로그로 기록(국면 변화
    //  또는 force_log 시). data_thread 전용(strategy_.list 반복은 이 스레드에서만 mutate).
    void apply_regime_selection(Regime regime, bool force_log);
    // 런타임 전략 등록(set_kis·position_provider·on_start·set_active·watch_specifications_ 추가 일괄).
    // strategy_.list push_back은 락 하에, strategy_.version 증가로 strategy_thread 스냅샷 갱신 유도.
    void register_strategy_runtime(std::unique_ptr<StrategyBase> strategy);

    // 지금 활성인 전략 중 일봉(on_data)을 쓰는 전략이 하나라도 있는가. data_thread의 일봉 폴링
    //  가드 — 아무도 안 쓰면 종목 수만큼의 차트 TR이 매 사이클 버려진다. 국면 전환으로 전략 집합이
    //  바뀌므로 캐시하지 않고 매번 확인한다(strategies_는 strategy_mutex_ 하에 읽는다).
    bool daily_bars_needed();

    // ── 통계·REST 폴백 ──────────────────────────────────────────────────────
    void print_statistics() const;
    // WS 피드가 죽었을 때 REST 현재가 폴링으로 낮춘다. 폴링이 쓸 시세 소스(실전 도메인)가
    //  없으면 낮춰봐야 틱이 안 나오므로 false를 돌려주고, 호출부는 kill switch로 넘어간다.
    bool activate_rest_fallback(const std::string& reason);
    // WS가 돌아왔을 때 원래 피드로 복귀. 처음부터 폴링이었으면(rest_price_feed=true) 아무것도 안 한다.
    void deactivate_rest_fallback();
    // 마감 자기 종료 한 주기 — 판정(session_end_)에 따라 로그·_private/state 표지 파일·request_shutdown. control_thread 전용.
    void step_session_end();
    // 감시견이 읽는 표지 파일 — _private/state/<name>_<KST 날짜>. 있으면 감시견이 그날 재기동하지 않는다. [why D-098]
    void write_state_marker(std::string_view name, std::string_view body) const;

private:
    // ── 기본 설정 ───────────────────────────────────────────────────────────
    KisConfig kis_config_;
    int fetch_interval_sec_;
    bool bootstrap_ledger_ = false; // 기동 시 실계좌 보유분 원장 시드 여부(G5, option-in)
    // ── 피드 상태 ────────────────────────────────────────────────────────────
    struct FeedState
    {
        // REST 폴백 상태
        bool rest_price_feed = false;  // REST 현재가 폴링을 체결 피드로 사용(WS 우회, option-in)
        // 지금 실제로 어느 피드로 도는지(런타임 상태). 기동 시 rest_price_feed로 초기화하고,
        //  WS 모드에서 연결이 죽으면 control_thread가 true로 올려 폴링으로 낮춘다(WS 복귀 시 되돌림).
        //  config 의도(rest_price_feed)와 분리해 둔 이유는, 폴백으로 켜진 것인지 처음부터 폴링이었는지를
        //  구분해야 복귀 여부를 판단할 수 있어서다. control_thread write / data_thread read.
        std::atomic<bool> rest_feed_active{false};
        bool rest_fallback_engaged = false; // 폴백으로 낮춘 상태인가(control_thread 전용, 전이 로그·복귀 판정)
        feed::Supervisor feed_sup; // WS stale→재연결 백오프→폴백 요구 판정(control_thread 전용) [why D-071]

        // 시세 설정
        KisConfig quote_kis_config;        // 시세 전용(실전 도메인) 설정
        bool has_quote_kis = false;     // 시세 전용 클라이언트 사용 여부

        // 피드 소스
        std::unique_ptr<KisClient> kis;
        // 시세 전용(실전 도메인). WS 모드에서도 폴백이 쓸 수 있어야 하므로 config에 블록이 있으면 항상 만든다.
        std::unique_ptr<KisClient> quote_kis;
        std::unique_ptr<feed::IFeedSource> websocket; // KisWebSocket·FeedMux(소켓 여럿) 또는 ReplaySource. 이름은 호출부 호환용.
        std::vector<KisConfig>             extra_feed_cfgs; // 추가 WS 세션 키. 비면 소켓 하나(FeedMux 없음)
        std::string                        replay_file;
        double                             replay_speed = 0.0;
        double                             replay_cash  = 0.0;
        std::unique_ptr<feed::PaperExecutor> paper; // 리플레이·피드 주입일 때만. OrderRouter·대조기가 kis 대신 본다
        std::unique_ptr<feed::IFeedSource>   feed_override; // set_feed_source가 준 소스. start()가 ws로 옮기고 kis는 비운다 [why D-071]
        bool                                 broker_offline = false; // 피드 소스가 없는 역할도 KIS를 부르지 않는다 [why D-114 단계 5]
        std::string                        capture_directory;
        std::unique_ptr<feed::TickCapture> capture; // WS 수신 스레드(소켓마다 하나)가 on_*를 부른다 — 큐는 MPSC
        std::vector<std::string>           capture_tickers;
        // 종목 id → 캡처에 담나. 비면 전부 담는다. [inv] 캡처를 열 때 한 번 채우고 그 뒤로는 읽기만 한다.
        std::vector<uint8_t>               capture_wanted;
    };
    FeedState feed_;
    // ── 매크로 레짐 ──────────────────────────────────────────────────────────
    // 매크로 레짐 브리지(data_thread 전용) — regime.json → OrderGate entry_halt. 상태기계는 헤더에, 파일 I/O·로그는 여기.
    std::string                      regime_file_;   // 빈 문자열이면 기능 미가동
    regime_file::RegimeFileJudge  regime_file_judge_; // stale·시간 상자·1회 로그 판정 [why D-060]
    // G3: 극단 위험회피(force_liquidate=TRUE) 시 보유 전량 강제청산 요청 플래그.
    //  data_thread(poll_regime_file)가 set → strategy_thread(요청 면 단일 생산자)가
    //  이 플래그를 보고 매 주기 시장가 전량 매도를 발주(요청 면의 단일생산자·단일소비자(SPSC)
    //  규약 위반 회피 — 요청 면에 넣는 스레드를 하나로 유지). 해제 시 중단.
    std::atomic<bool> force_liquidate_{false};
    // ── 주문 설정 ───────────────────────────────────────────────────────────
    int order_min_interval_ms_ = 350; // 주문 간 최소 간격(milliseconds) — order_thread의 OrderRateLimiter가 쓴다 [why D-065]
    int order_max_retries_ = 3;       // 거부된 주문의 재시도 횟수(C-2)

    // ── 잔고 대조·REST 폴러 ─────────────────────────────────────────────────
    // 잔고 → 원장 대조기(기동 시드·주기 대조·손익 기준선·서킷브레이커). start()에서 feed_.kis·order_router_ 뒤에
    //  만들고 data_thread만 부른다. [why D-061]
    std::unique_ptr<LedgerReconciler> ledger_;
    // REST 현재가 폴러(폴링 모드 유니버스·WS 넘침 대체·보유 보충). start()에서 feed_.kis 뒤에 만든다.
    //  poll_*은 폴러 자기 스레드(start_rest_poll_loop), top_up은 data_thread가 부른다. [why D-062] [why D-138]
    std::unique_ptr<DataPoller> poller_;
    static constexpr int kRestPollRoundMs = 1000; // 넘침 종목 한 바퀴 목표. 종목이 한도(초당 10)를 넘으면 그만큼 늘어난다

    // 무거운 REST를 미리 당기는 공용 프리페치 풀. 전략보다 먼저 선언해 나중에 사라지게 둔다
    //  — 전략 소멸자가 자기 작업을 떼는 동안 풀이 살아 있어야 한다. [why D-071]
    static constexpr int kPrefetchPeriodMs = 3000; // 일봉은 1일 1회·분봉은 봉 경계마다라 초 단위로 충분(전략 하트비트 기본값과 같다)
    prefetch::Pool prefetch_pool_{prefetch::Pool::recommended_thread_count(),
                                  std::chrono::milliseconds(kPrefetchPeriodMs)};

    // ── 전략 레지스트리 ──────────────────────────────────────────────────────
    // 뗀 전략 대기열 항목(data_thread 전용). version=뗀 직후의 strategy_.version.
    struct Retired
    {
        std::unique_ptr<StrategyBase> strategy;
        uint64_t                      version = 0;
    };
    struct StrategyRegistry
    {
        std::vector<std::unique_ptr<StrategyBase>> list;
        // list 동시성 보호: strategy_thread는 version 변경 시에만 StrategyBase*
        // 스냅샷을 재구성(무락 순회), data_thread는 재스캔 등록 시 락+version 증가.
        std::mutex             mutex;
        std::atomic<uint64_t>  version{0};
        std::atomic<uint64_t>  seen_version{0}; // strategy_thread가 마지막으로 스냅샷에 반영한 버전
        std::vector<Retired>   retired;
        // G1: 국면→전략 자동선택 상태 (data_thread 전용)
        std::map<Regime, std::vector<std::string>> regime_map; // 국면별 활성 전략 id(빈 항목=아무 전략도 활성 안 함)
        bool   has_regime_map = false;                 // false면 per-strategy active_regimes 폴백
        Regime last_selected_regime = Regime::UNKNOWN; // 직전 선택 국면(변화 감지→재선택·로그)
    };
    StrategyRegistry strategy_;

    // ── 유니버스 재스캔 ──────────────────────────────────────────────────────
    // 장부는 UniverseRescan(멤버 universe_rescan_, 목록 끝에 선언)이 쥔다. 전략 목록에서 떼는 일은 여기서 한다 —
    //  잠금·버전·라우팅 재구성이 전략 레지스트리 몫이라서다. make_line은 해제 로그 문장.
    void retire_strategy(StrategyBase* pointer, const std::function<std::string(const StrategyBase&)>& make_line);

    // ── 공유 쪽지 자리표 ────────────────────────────────────────────────────
    // 경계를 넘을 면 열을 자리표 한 장 위에 모은다(ipc::SharedLayout). 갈라 띄우면 이 바이트가 공유
    //  쪽지가 되고, Both 로 돌면 아래 힙 한 덩이가 그 자리를 대신한다 — 자리 셈도 코드 경로도 하나다.
    //  여기서 둘로 갈라 두면 갈라 띄운 날에만 도는 코드가 생기고, 그런 코드는 그날 처음 돈다. [why D-114]
    //  [inv] 아래 파이프라인·장부 사본이 이 자리표를 가리킨다 — 먼저 선언해 나중에 죽는다.
    //  [inv] 스레드가 뜨기 전에만 다시 깐다(생성자와 start()의 준비 단계). 뜬 뒤에 깔면 돌던 큐가 지워진다.
    std::vector<std::byte>  layout_storage_;
    // 갈라 띄울 때만 열리는 공유 쪽지. 주문 쪽이 만들고 전략 쪽이 붙는다 — Both 로 돌면 닫힌 채다.
    //  [inv] layout_ 보다 먼저 선언한다. 지도가 접히면 자리표가 가리키던 바이트가 사라진다.
    ipc::SharedRegion       layout_region_;
    ipc::SharedLayoutConfig layout_config_;
    ipc::SharedLayout       layout_;
    [[nodiscard]] bool      bind_layout(uint32_t feed_lanes);
    // 자리표를 힙 한 덩이 위에 깐다(Both). 바이트를 잡고 캐시라인 경계에 맞춰 얹는다.
    [[nodiscard]] bool      bind_layout_on_heap(size_t needed);
    // 자리표를 공유 쪽지 위에 얹는다(Order는 만들고 Strategy는 붙는다). 만드는 쪽이 늦게 떠도
    //  붙는 쪽이 잠깐 기다린다 — 감시견이 띄우는 순서를 정해 주지 않는다. [why D-114]
    [[nodiscard]] bool      bind_layout_on_region(size_t needed);
    // 두 프로세스가 같은 이름을 보게 계좌로 짓는다 — 모의·실계좌를 같이 띄워도 쪽지가 겹치지 않는다.
    [[nodiscard]] std::string shared_region_name() const;

    // ── N×M 샤드 파이프라인·큐 ──────────────────────────────────────────────
    // 수신 N × 전략 샤드 M 링 행렬. 셀 하나의 생산자는 스레드 하나다 — WS 수신 스레드 i(소켓 i의 수신 스레드)는 행 i, 체결은
    //  데이터 스레드 행(REST 대체 틱, 행 data_row)을 더 둔다(D-053이 두 큐로 풀던 것을 행으로 푼다). 열은 전략 단위다 —
    //  전략 객체는 샤드 하나가 갖고(등록 순 라운드로빈, StrategyBase::shard_index), 종목 틱은 routes가 준 마스크의 샤드 전부에
    //  넣는다. 한 열 안에서 종목 순서는 지켜진다(원칙 2). 전략 객체를 두 샤드 스레드가 만지는 일은 없다. [why D-071] [why D-110]
    struct ShardPipeline
    {
        // 큐 용량. 셀 하나(행렬 원소)·shard_out은 틱 폭주를 받는 크기, 주문·체결은 KIS 왕복 몇 초를 받는 크기다.
        //  고수위 로그가 같은 상수로 분모를 찍는다 — 여기만 바꾸면 로그도 따라온다.
        static constexpr size_t   kTickCellCapacity   = 4096;
        static constexpr size_t   kBarCellCapacity    = 1024;
        static constexpr size_t   kOrderQueueCapacity = 1024;
        static constexpr size_t   kFillQueueCapacity  = 1024;
        static constexpr size_t   kOrderResponseCapacity = 1024; // 요청 하나에 답 하나 — 요청 큐와 같은 크기 [why D-114]
        static constexpr uint64_t kDropLogEvery       = 100; // 큐 가득으로 버린 신호는 첫 건과 이 배수마다만 WARN
        static constexpr uint32_t kDefaultWebsocketLanes = 1; // 앱키 하나 = 소켓 하나. 추가 앱키·주입 소스가 있을 때만 websocket_lane_count()가 늘린다
        uint32_t                  websocket_lanes        = kDefaultWebsocketLanes; // WS 수신 스레드 수 = 소켓 수. start()가 행 수로 쓴다
        uint32_t                  data_row() const  // trade_matrix의 데이터 스레드(REST 대체) 행 = 소켓 행 바로 뒤
        {
            return websocket_lanes;
        }

        uint32_t                  strategy_shards = 1;    // config. start()가 열 수로 쓴다(상한 shard::kMaxShards)
        uint32_t                  next_shard      = 0;    // 다음 전략에 줄 샤드(라운드로빈 커서). strategy_.mutex 하에서
        shard::RouteTable         routes;                 // 종목 id → 그 종목을 보는 샤드 마스크. 수신 스레드가 틱마다 읽는다
        shard::Matrix<OrderBook>  order_book_matrix{1, 1, kTickCellCapacity};   // 호가 (국내) — WS 수신 스레드 행 N. 행·열 수는 start()의 reshape
        shard::Matrix<TradeData>  trade_matrix{2, 1, kTickCellCapacity};   // 체결 (미국 + 국내) — WS 수신 스레드 행 N + 데이터 스레드 행
        shard::Matrix<MarketData> bars_matrix{1, 1, kBarCellCapacity}; // 일봉 — 데이터 스레드 행(index 0)만
        std::vector<std::unique_ptr<strategy::Shard>> shards;           // 열 m을 비우는 샤드. start()가 만든다
        std::vector<std::jthread>                  shard_threads;
        // 샤드 → 전략(디스패치) 스레드. 생산자가 M이라 MPSC(원칙 5). 가득 차면 버리고 센다 — order_dropped와 같은 규칙.
        MpscQueue<strategy::Emitted> shard_out{kTickCellCapacity};
        std::atomic<uint64_t>     shard_dropped{0};
        // 전략 → 주문 요청 면. 큐는 자리표 위에 있고 여기 있는 것은 그 자리를 가리키는 포인터뿐이라,
        //  프로세스를 갈라도 이 줄은 그대로다. 크기는 주문 스레드가 KIS 왕복에 묶이는 몇 초를 받는 만큼이다.
        //  [inv] bind_layout()이 꽂는다. [inv] 넣는 쪽은 전략 스레드 하나, 꺼내는 쪽은 주문 스레드 하나다.
        //  [why D-114] [why D-073]
        ipc::SharedSpscRing<ipc::OrderRequest>* requests = nullptr;
        // 요청 면에 한 번에 쌓였던 최대 줄 수. 공유 쪽지 링은 이것을 스스로 재지 않는다 — 재려면 칸을 하나 더
        //  두어야 하고 그 칸은 건너편이 덮을 수 있다. 그래서 넣는 쪽이 넣은 뒤 한 번 보고 최고치만 남긴다.
        //  [inv] 넣는 쪽이 하나라 읽고 쓰는 사이에 끼어들 쪽이 없다. [why D-114]
        std::atomic<uint64_t> order_high_water{0};
        // 체결통보. WS 수신 스레드는 여기 push만 하고 원장 반영(OrderRouter::on_fill)은 fill_thread가 한다 —
        //  체결 하나 처리(history_mutex_·CSV 쓰기) 동안 전 종목 틱 수신이 멈추지 않게. [why D-056]
        //  칸은 문자열 없는 레코드(ipc::FillNotice)다 — 갈라 띄운 날의 체결 통로와 같은 모양이라 두 길의 옮기는
        //  코드가 하나다. [why CODE_REVIEW W-7]
        RingBuffer<ipc::FillNotice> fill_queue{kFillQueueCapacity};
        std::atomic<uint64_t> fill_dropped{0};   // 버린 체결통보 수. 넘침 목록이 생긴 뒤로는 버리지 않아 늘 0이다
        // 큐가 가득 찼을 때 체결통보를 버리지 않고 잠시 두는 곳. 다음 체결통보가 올 때와 제어 스레드가
        //  5초마다 앞에서부터 큐로 다시 넣는다. 늘 뒤에 붙이고 앞에서 빼니 순서가 지켜진다.
        //  [inv] fill_producing 차례를 쥔 쪽만 만진다.
        //  [why] 체결은 잔고·예약 수량을 바꾼다. 버리면 잔고 대조가 메울 때까지 원장이 틀린다.
        std::deque<ipc::FillNotice> fill_overflow;
        std::atomic<uint64_t> fill_overflow_waiting{0}; // 넘침 목록 길이. 제어 스레드가 차례 없이 보려고 따로 둔다
        std::atomic<uint64_t> fill_overflowed{0};       // 넘침 목록에 넣은 누적 건수. 0이 아니면 큐가 찬 적이 있다
        // 체결통보를 지금 넣고 있는 쪽이 있는지. fill_queue·체결 통로 둘 다 SPSC라 넣는 쪽은 한 번에 하나여야 한다 —
        //  실매매는 체결통보를 맡은 소켓의 수신 스레드 하나, 오프라인은 모의 체결기 하나(전달을 스스로 한 줄로 세운다).
        //  둘이 겹치면 뒤에 온 쪽이 기다려 한 줄로 서고 fill_producer_overlap에 센다.
        //  [inv] fill_producer_overlap은 0이다. 0이 아니면 생산자가 둘 생긴 것이다. [why CODE_REVIEW W-6]
        std::atomic<bool>     fill_producing{false};
        std::atomic<uint64_t> fill_producer_overlap{0};
        std::atomic<uint64_t> order_dropped{0};  // 요청 면이 가득 차 버린 신호 수. [큐 고수위] 줄에 같이 찍힌다
        std::atomic<uint64_t> order_sell_held{0}; // 요청 면이 가득 차 버리지 않고 들고 있던 매도 수. 같은 줄에 찍힌다
        std::atomic<uint64_t> order_stale{0};    // 큐에서 너무 오래 기다려 꺼낼 때 버린 신규 매수 수 [why D-127]
        std::atomic<uint64_t> order_implausible{0};      // 값이 말이 안 돼 버린 요청 수. 0이 아니면 통로가 덮였다
        std::atomic<uint64_t> order_reason_truncated{0}; // 판단 근거·주문 이름이 칸을 넘어 잘린 신호 수
        // 주문 → 전략 응답. 보내는 쪽이 주문 스레드 하나, 받는 쪽이 전략 스레드 하나라 SPSC다. 큐는
        //  자리표 위에 있고 여기 있는 것은 그 자리를 가리키는 포인터뿐이라, 프로세스를 갈라도 이 줄은
        //  그대로다. [inv] bind_layout()이 꽂는다. [why D-114]
        //  [inv] Both 로 돌 때는 한 인스턴스가 양쪽 끝을 맡는다 — 보내는 쪽 자리와 받는 쪽 자리를 각각
        //   한 스레드만 만지므로 같은 인스턴스라도 값이 섞이지 않는다(칸 하나에 값 하나씩 따로 있다).
        ipc::SharedSpscRing<ipc::OrderResponse>* order_responses = nullptr;
        std::atomic<uint64_t> order_response_dropped{0}; // 전략이 답을 안 가져가 버린 응답 수
        std::atomic<uint64_t> order_duplicate{0};        // 주문 쪽이 같은 순번을 두 번 받아 거른 수. 0이 아니면 통로가 샜다
        // 전략 스레드가 한 바퀴마다 찍고 주문 스레드가 공백만 보고 생사를 판정한다. 자리표의 박동 면
        //  가운데 전략 쪽 칸을 가리킨다 — 프로세스가 갈려도 찍는 자리도 보는 자리도 그대로다. [why D-114]
        //  [inv] bind_layout()이 꽂는다.
        ipc::Heartbeat* strategy_heartbeat = nullptr;
    // 주문 스레드가 본 가장 긴 박동 공백(나노초). 문턱을 감으로 정하지 않으려고 밖으로 낸다 — 부하 하네스의
    //  beat_gap_max_ms 열과 [큐 고수위] 줄, check_runtime_health의 판정 행이 이 값 하나를 본다. [why D-114]
    std::atomic<int64_t> strategy_beat_gap_max_ns{0};
        // 주문 스레드가 한 바퀴마다 찍고 전략 스레드가 공백만 본다. 자리표의 박동 면 가운데 주문 쪽 칸을
        //  가리킨다 — 칸은 처음부터 있었지만 찍는 쪽도 보는 쪽도 없어 비어 있었다. [why D-114]
        //  [inv] bind_layout()이 꽂는다.
        ipc::Heartbeat* order_heartbeat = nullptr;
        // 시세 스레드가 한 바퀴마다 찍는다. 시세가 죽으면 체결통보가 주문 쪽에 안 들어와 예약 수량이 안
        //  풀리므로(총노출 이중계상), 이 칸의 공백이 그 사고를 가장 먼저 알리는 자리다. [why D-114 단계 5]
        //  [inv] bind_layout()이 꽂는다.
        ipc::Heartbeat* feed_heartbeat = nullptr;
        // 체결통보 레코드 순번 발급기(프로세스 안 큐·시세 → 주문 통로 공용). 0은 안 쓴다 — 받는 쪽이 0을 "안 채워진 칸"으로 본다.
        std::atomic<uint64_t> fill_sequence{0};
        // 전략 스레드가 본 가장 긴 주문 박동 공백(나노초). 전략 쪽 공백과 달리 여기에는 증권사 왕복이
        //  그대로 들어온다 — 문턱을 실측으로 좁히려고 밖으로 낸다. [why D-114]
        std::atomic<int64_t> order_beat_gap_max_ns{0};
        // 시한을 넘겨도 답이 안 온 요청을 본 횟수. 세고 찍기만 한다 — 다시 보내지 않는다. [why D-114]
        std::atomic<uint64_t> order_answer_overdue{0};
        // 주문 스레드가 본 가장 긴 시세 박동 공백(나노초). 시세 쪽은 hot loop 가 아니라 제어 바퀴
        //  5초마다 찍으므로 공백에 그 간격이 그대로 들어온다 — 문턱을 실측으로 좁히려고 밖으로 낸다.
        //  [why D-137]
        std::atomic<int64_t> feed_beat_gap_max_ns{0};
        // 소비자 깨우기 — 생산자가 push 뒤 notify, 소비자는 큐가 비면 잔다. 1ms 폴링은 Windows 타이머 격자 때문에
        //  실측 p50 15.6ms였다(bench_sleep_res). [why D-071]
        wake::WakeGate fill_wake;  // fill_thread ← WS 수신 스레드
        wake::WakeGate strategy_wake; // strategy_thread ← 샤드·운영단말 스레드. 샤드 자신의 게이트는 Shard::wake()
        wake::WakeGate order_wake; // order_thread ← strategy_thread
    };
    ShardPipeline pipeline_;
    // 구간 지연 분포 — 주문 스레드가 넣고 데이터 스레드가 HEALTH에 분위수로 싣는다(원자 버킷이라 락 없음). [why D-071]
    trace::PipelineLatency pipeline_latency_;
    // [inv] 직전 HEALTH 때 뜬 버킷 사본. 데이터 스레드만 읽고 쓴다 — 다른 스레드가 건드리면 구간 분위수가 어긋난다.
    trace::PipelineSnapshot previous_latency_snapshot_;

    // 장부 사본을 내는 간격. 읽는 쪽에 주는 약속이 이 값이다 — "고친 것이 이 시간 안에 사본에 닿는다".
    //  전에는 주문 스레드가 바쁠 땐 주문마다, 쉴 땐 이 간격으로 내 약속이 둘이었다. [why D-114]
    static constexpr std::chrono::milliseconds kLedgerPublishInterval{100};

    // ── 스레드 핸들 ──────────────────────────────────────────────────────────
    std::jthread data_thread_;
    std::jthread strategy_thread_;
    std::jthread order_thread_;
    std::jthread fill_thread_;
    std::jthread ledger_thread_;
    std::jthread control_thread_;

    // 줄마다 하나. 전략 역할에만 뜬다(주문 쪽은 소켓 수신 스레드가 그 일을 한다). [why D-114]
    std::vector<std::jthread> feed_lane_threads_;

    std::atomic<bool> running_{false};
    // start() 를 한 번이라도 불렀는가. 번호를 기다리는 시간을 여기서 가른다 — 기동 중에는 건너편이
    //  제 일감을 하느라 늦게 집어도 기다리고, 한 번 뜬 뒤에는 짧게 본다(멈춘 채로 5분을 기다리지 않는다).
    std::atomic<bool> start_was_called_{false};
    // 공유 쪽지에 적을 종료 사유. 스레드를 다 회수한 뒤 stop() 이 적는다 — 안 적힌 0 이 크래시다. [why D-114]
    std::atomic<uint32_t> shutdown_reason_{static_cast<uint32_t>(ipc::SharedShutdownReason::kOperator)};
    // 붙을 때 본 건너편 기동 번호. 0 이면 안 붙은 것이다(주문 쪽·Both). 달라지면 건너편이 죽고 다시 떴다. [why D-114]
    uint64_t peer_boot_generation_ = 0;
    session_end::Judge session_end_; // 마감 자기 종료 판정(control_thread 전용). 기본은 창 0 = 판정 없음 [why D-098]

#ifdef HAS_ZMQ
    std::unique_ptr<ZmqBridge> zmq_bridge_;
#endif
    db::DbConfig database_config_;
#ifdef HAS_PQ
    // WS 수신 스레드(소켓마다 하나)가 on_trade를 부른다. start()가 만들고 stop()이 수신 스레드를 거둔 뒤 멈춘다. [why D-148]
    std::unique_ptr<db::DbManager> database_;
#endif

    // ── 운영 채널(Ops·수동주문) ──────────────────────────────────────────────
    // 운영단말 서버와 수동주문 인테이크. 서버 스레드가 push, order_thread가 pop해
    //  OrderSignal(strategy_id="MANUAL")로 바꿔 게이트·원장을 그대로 지난다. 소켓 스레드가 발주 사슬에
    //  직접 들어가지 않는 것은 FORCE_LIQ와 같은 이유고, 꺼내는 쪽을 주문 쪽에 둔 것은 전략 프로세스가
    //  멎어도 사람이 손으로 낼 수 있어야 해서다. [why D-043][why D-114]
    struct OpsChannel
    {
        std::unique_ptr<OpsServer>      server;
        static constexpr size_t         kManualInboxCapacity = 256; // 단말 수동주문은 사람이 치는 속도라 이만큼이면 남는다
        MpscQueue<OpsOrderReq>          manual_inbox{kManualInboxCapacity};
        std::string                     bind_address;
        int                             port = 0;
        std::string                     token;
        std::mutex                      manual_client_id_mutex;
        // 재전송 중복 차단(세션 내). cid는 운영단말이 준 문자열 그 자체라 문자열 집합 — 해시로 줄이면 충돌이
        //  정상 주문을 "중복"으로 거부할 수 있고, 세션당 수십 건이라 얻는 것도 없다.
        std::unordered_set<std::string> manual_cids;
    };
    OpsChannel ops_;

    // 표지 파일 이름 접미. 비어 있으면 접미 없이 예전 이름을 쓴다. [why D-122]
    std::string instance_;
    void        start_ops_server();
    std::string ops_status_json() const;
    std::string ops_positions_json() const;
    // order_thread 전용 — 낼 것이 있으면 signal에 담고 true. 거부는 그 자리에서 단말에 알리고 다음 건을 본다. [why D-114]
    [[nodiscard]] bool take_manual_order(OrderSignal& signal);

    // ── 카운터 ───────────────────────────────────────────────────────────────
    std::atomic<uint64_t> data_count_{0};
    std::atomic<uint64_t> trade_drop_count_{0}; // WS 체결 큐 가득으로 버린 틱 수 [why D-055]
    std::atomic<uint64_t> order_book_drop_count_{0}; // WS 호가 큐 가득으로 버린 호가 수 [why D-067]
    std::atomic<uint64_t> signal_count_{0}; // 신호 순번 자체는 strategy_thread의 SignalDispatcher가 찍는다 [why D-063]
    std::atomic<uint64_t> order_count_{0};

    // ── 리스크 게이트·주문 라우터 ────────────────────────────────────────────
    OrderGate order_gate_;
    // 고정 이름 신호의 전략 번호 — 게이트 테이블에서 한 번 받는다. order_gate_ 뒤에 선언해야 한다.
    //  스레드가 뜨기 전(생성자)이라 표를 고치는 쪽은 여전히 하나다. 전략 스레드에서 받으면 그것이
    //  전략 쪽의 표 쓰기가 된다 — 디스패처가 자기 생성자에서 받던 것을 여기로 올렸다. [why D-114]
    //  갈라 띄우면 표가 공유 쪽지 위 것으로 바뀌므로(adopt_shared_dictionaries) 그 자리에서 다시 받는다 —
    //  그래서 const 가 아니다. 바꾸는 자리는 거기 하나이고 스레드 전이다. [why D-114]
    strategy_table::StrategyId manual_strategy_index_   = order_gate_.ledger().strategy_index_of("MANUAL");
    strategy_table::StrategyId force_liquidation_index_ = order_gate_.ledger().strategy_index_of("FORCE_LIQ");
    strategy_table::StrategyId limit_trim_index_        = order_gate_.ledger().strategy_index_of("LIMIT_TRIM");
    std::unique_ptr<OrderRouter> order_router_; // 주문 전처리·중계 레이어(증권업계 용어로 FEP, Front-End Processor). start() 이후 유효
    // 전략 쪽이 읽을 장부 사본. 장부가 바뀔 때마다 order_gate_가 여기에 한 판을 낸다. 자리표의 마지막
    //  면이라 Engine 안에 실체가 없다 — 여기 있는 것은 그 자리를 가리키는 포인터다. [why D-114]
    //  [inv] bind_layout()이 꽂는다. 못 깔면 start()가 뜨지 않으니 여기서 빈 포인터를 보지 않는다.
    ipc::LedgerSnapshot* ledger_snapshot_ = nullptr;

    // ── 구독 스펙 ─────────────────────────────────────────────────────────────
    // 전략에서 수집한 구독 스펙 (on_start 이후 확정)
    //  data_thread(재스캔 등록)가 쓰고 control_thread(WS 재연결)가 읽는다 — watch_specs_mtx_로 보호.
    std::vector<WatchSpec> watch_specifications_;
    mutable std::mutex     watch_specifications_mutex_;

    // 아직 소켓에 걸지 않은 구독 스펙. 주문 스레드가 제어 요청에서 꺼내 여기 쌓고, 감시 스레드가
    //  비우며 소켓에 건다 — 주문 스레드는 단일 시퀀서라 소켓 쓰기로 막으면 그동안 주문이 안 나간다. [why D-114]
    //  [inv] watch_specifications_mutex_ 로 보호한다(넣는 쪽 주문 스레드, 비우는 쪽 감시 스레드).
    std::vector<WatchSpec> pending_subscriptions_;
    // 아직 소켓에서 풀지 않은 구독 해지. pending_subscriptions_와 같은 자물쇠·같은 두 스레드. [why D-132]
    std::vector<WatchSpec> pending_unsubscriptions_;
    // 시세 쪽 칸 상태 — 종목 id 인덱스. [inv] 감시 스레드만 만진다. [why D-132]
    struct WebSocketSlotState
    {
        int32_t                               priority = websocket_slot::kUnranked;
        std::chrono::steady_clock::time_point on_since{}; // 칸을 쥔 것을 처음 본 때. 칸 밖이면 0
        bool                                  warned_off_socket = false; // 보유·선점인데 칸 밖이라고 남겼나
    };
    std::vector<WebSocketSlotState> websocket_slots_;
    // 전략 쪽이 마지막으로 보낸 칸 우선순위 — 종목 id 인덱스, 아직 안 보냈으면 kUnsent. [inv] data_thread만. [why D-132]
    static constexpr int32_t kUnsent = -1;
    std::vector<int32_t>     watch_priority_sent_;
    // 칸을 늘 쥐는 종목(설정 websocket_pin_tickers). 기동 때 한 번 받고 읽기만 한다 — 몇 개뿐이라 선형 탐색. [why D-138]
    std::vector<std::string> websocket_pin_tickers_;
    bool is_websocket_pinned(std::string_view ticker) const;
    // 이 종목 체결·호가를 캡처에 담나. 수신 스레드가 부른다 — 문자열 없이 id 인덱스로 본다(원칙 6).
    bool capture_wanted(symbol::SymbolId symbol) const
    {
        return feed_.capture_wanted.empty() || (symbol < feed_.capture_wanted.size() && feed_.capture_wanted[symbol] != 0);
    }

    // 구독 상한에 밀린 종목 수. 0이 아니면 그 종목은 WS 틱을 못 받는다 — 판정 행 "구독 상한"이 본다. [why D-114]
    std::atomic<uint64_t> watch_overflow_{0};
    // 시세 통로 버린 건수 둘. 보내는 쪽은 소켓 수신 스레드, 받는 쪽은 줄 스레드가 올린다. [why D-114]
    std::atomic<uint64_t> feed_channel_overflow_{0};
    std::atomic<uint64_t> feed_channel_discarded_{0};

    // ── ZMQ ──────────────────────────────────────────────────────────────────
    bool        zmq_enabled_      = true; // set_zmq_enabled. 부하 하네스만 끈다
    std::string zmq_bind_address_ = "127.0.0.1";
    std::string zmq_control_token_;
    // 보호 주문 표 — 전략(샤드 스레드)가 등록하고 strategy_thread(주문 시퀀서)가 본다. 표 자체가 잠금을 가진다. [why D-114]
    risk::ProtectiveOrderBook             protective_book_;
    std::chrono::milliseconds             protective_interval_{kProtectiveIntervalMsDefault};
    // 다음에 표를 볼 시각(steady_clock 틱). 전략·주문 두 스레드가 잡으러 오므로 원자다 — claim_protective_cycle만 민다.
    std::atomic<std::chrono::steady_clock::rep> protective_next_ticks_{0};
    // 전략이 죽어 주문 쪽이 마무리에 들어간 국면. 주문 스레드만 쓰고 HEALTH·판정 행이 읽는다. [why D-114]
    std::atomic<bool> strategy_wound_down_{false};
    // 시세가 죽어 신규 진입을 끊은 국면. 주문 스레드만 쓰고 HEALTH·판정 행이 읽는다. [why D-137]
    std::atomic<bool> feed_wound_down_{false};
    ZmqPorts    zmq_ports_;

    // ── 심볼·현재가 캐시 ──────────────────────────────────────────────────────
    struct SymbolCache
    {
        // 종목 문자열 ↔ 정수 id. 수신·폴러 스레드가 틱·호가에 id를 찍고 아래 현재가 캐시가 그 id로 인덱스한다.
        //  처음 보는 종목은 그 자리에서 등록되므로 기동 시 채울 필요가 없다. [why D-071]
        //  [inv] table이 아래 두 배열보다 먼저 선언돼야 한다 — 배열 크기가 table.capacity()로 초기화된다.
        symbol::SymbolTable table;

        // 종목별 최근 체결가(원 단위)와 받은 시각(steady_clock nanoseconds), id 인덱스 배열. 전략 스레드가 td_queue_를 비우며
        //  쓰고, 데이터 스레드가 틱이 끊긴 보유 종목을 REST로 보충하고, OpsServer 스레드가 POSITIONS 현재가·수동주문
        //  ref_price로 읽는다. 원소가 atomic이라 락이 없고, price와 at은 따로 읽혀 순간 어긋날 수 있다(둘 다 감시용).
        //  틱이 없던 종목은 0.
        std::unique_ptr<std::atomic<double>[]>  last_price_array{std::make_unique<std::atomic<double>[]>(table.capacity())};
        std::unique_ptr<std::atomic<int64_t>[]> last_price_at_ns{std::make_unique<std::atomic<int64_t>[]>(table.capacity())};

        // 청산 관리가 붙은 종목(id 인덱스). 기동 시 단일스레드 구간에서만 켜고 전략 스레드는 읽기만 한다.
        std::vector<bool> exit_managed{std::vector<bool>(table.capacity(), false)};
    };
    SymbolCache symbols_;

    // 이 프로세스가 맡는 자리. 기동 때 한 번 정해지고 그 뒤로 안 바뀐다.
    ProcessRole           role_ = ProcessRole::Both;
    std::atomic<uint64_t> symbol_register_timeouts_{0};
    // 기동 중 등록 기다림을 한 번이라도 빈손으로 접었으면 선다. 그 뒤로는 기다리지 않는다 —
    //  건너편이 답을 못 주는 까닭(이름표가 찼다·프로세스가 죽었다)은 다음 종목에서도 그대로라,
    //  종목마다 5분씩 다시 기다리면 2,700종목에 225시간이 된다. [why D-114]
    std::atomic<bool>     register_wait_abandoned_{false};

    // 건너편(주문 쪽) 박동 시각. 볼 자리가 없으면 0을 준다 — 한 프로세스로 돌 때가 그렇고,
    //  그때는 기다림도 이 프로세스 안에서 끝나 볼 까닭이 없다.
    [[nodiscard]] int64_t peer_order_beat_ns() const
    {
        return pipeline_.order_heartbeat ? pipeline_.order_heartbeat->last_ns() : int64_t{0};
    }

    std::atomic<uint64_t> symbol_lookup_misses_{0};
    std::atomic<uint64_t> strategy_register_timeouts_{0};

    // 시세 역할이 구독하지 않은 종목을 받아 버린 횟수. 0이 아니면 구독 목록과 세션이 어긋난 것이다.
    std::atomic<uint64_t> unknown_ticker_dropped_{0};

    // ── 종목명 캐시 ──────────────────────────────────────────────────────────
    // 종목 id→종목명 라벨(로그 표시용, 빈 문자열=없음). 여러 스레드가 접근해 ticker_names_mutex_로 보호.
    //  [inv] symbols_ 뒤에 선언한다 — 크기가 table.capacity()로 초기화된다.
    std::vector<std::string> ticker_names_{std::vector<std::string>(symbols_.table.capacity())};
    mutable std::mutex       ticker_names_mutex_;

    // 유니버스 재스캔 장부 (data_thread 전용). [inv] symbols_ 뒤에 선언한다 — 표 참조를 생성자에서 받는다.
    UniverseRescan universe_rescan_{
        symbols_.table,
        [this](std::unique_ptr<StrategyBase> strategy)
        {
            register_strategy_runtime(std::move(strategy));
        },
        [this](StrategyBase* pointer, const std::function<std::string(const StrategyBase&)>& make_line)
        {
            retire_strategy(pointer, make_line);
        }};

    // 제어 요청 통로 — 전략 쪽이 주문 쪽 표를 고쳐 달라고 보내는 요청을 싣고·옮기고·적용한다. 보호 주문 창구도
    //  여기서 전략에 꽂아 준다. [inv] order_gate_·protective_book_·symbols_·pipeline_ 뒤에 선언한다 — 참조를
    //  생성자에서 받는다. 뒤 토막은 bind_layout()이 꽂는다. [why D-114]
    ControlPlane control_plane_{order_gate_, protective_book_, symbols_.table, pipeline_.strategy_wake,
                                pipeline_.order_wake, [this]
                                {
                                    apply_reset_daily();
                                }};

    double                                  last_price(symbol::SymbolId id) const noexcept;
    double                                  last_price(const std::string& ticker) const;
    int64_t                                 last_price_at_ns(const std::string& ticker) const;
    void                                    set_last_price(symbol::SymbolId id, double price) noexcept;
    void                                    set_last_price(const std::string& ticker, double price);
};
