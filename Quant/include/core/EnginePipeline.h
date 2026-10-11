#pragma once
#include "core/MpscQueue.h"
#include "core/RingBuffer.h"
#include "core/ShardMatrix.h"
#include "core/ShardRoutes.h"
#include "core/StrategyShard.h"
#include "core/Types.h"
#include "core/WakeGate.h"
#include "ipc/FillChannel.h"
#include "ipc/Heartbeat.h"
#include "ipc/OrderChannel.h"
#include "ipc/SharedSpscRing.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <thread>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Engine의 N×M 샤드 파이프라인(수신 → 샤드 → 전략 → 주문 → 체결 큐와 박동 칸)과 큐 수위 묶음.
//  Engine.h에서 떼어 낸 것이다 — Engine 안에서는 같은 이름(Engine::ShardPipeline·Engine::QueueStatistics)으로 쓴다.
//  관련 결정: D-071·D-110·D-114.
// ─────────────────────────────────────────────────────────────────────────────

namespace engine
{

// 큐 수위·버린 건수 묶음. Engine::queue_statistics()가 채운다.
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
    // 체결통보. WS 수신 스레드는 여기 push만 하고 장부 반영(OrderRouter::on_fill)은 fill_thread가 한다 —
    //  체결 하나 처리(history_mutex_·CSV 쓰기) 동안 전 종목 틱 수신이 멈추지 않게. [why D-056]
    //  칸은 문자열 없는 레코드(ipc::FillNotice)다 — 갈라 띄운 날의 체결 통로와 같은 모양이라 두 길의 옮기는
    //  코드가 하나다. [why CODE_REVIEW W-7]
    RingBuffer<ipc::FillNotice> fill_queue{kFillQueueCapacity};
    std::atomic<uint64_t> fill_dropped{0};   // 버린 체결통보 수. 넘침 목록이 생긴 뒤로는 버리지 않아 늘 0이다
    // 큐가 가득 찼을 때 체결통보를 버리지 않고 잠시 두는 곳. 다음 체결통보가 올 때와 제어 스레드가
    //  5초마다 앞에서부터 큐로 다시 넣는다. 늘 뒤에 붙이고 앞에서 빼니 순서가 지켜진다.
    //  [inv] fill_producing 차례를 쥔 쪽만 만진다.
    //  체결은 잔고·예약 수량을 바꾼다. 버리면 잔고 대조가 메울 때까지 장부가 틀린다.
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

} // namespace engine
