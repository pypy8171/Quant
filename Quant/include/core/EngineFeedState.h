#pragma once
#include "api/KisClient.h"
#include "core/FeedSupervisor.h"
#include "core/IFeedSource.h"
#include "core/PaperExecutor.h"
#include "core/SymbolTable.h"
#include "core/TickCapture.h"
#include "core/WebSocketSlotPlan.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Engine의 피드 상태(소켓·REST 폴백·리플레이·캡처), 시세 쪽 WS 칸 상태, 종목 표·현재가 캐시.
//  Engine.h에서 떼어 낸 것이다 — Engine 안에서는 같은 이름(Engine::FeedState 등)으로 쓴다.
//  관련 결정: D-071·D-114·D-132.
// ─────────────────────────────────────────────────────────────────────────────

namespace engine
{

// 피드 상태. 쓰는 스레드는 칸마다 주석에 적었다.
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

// 시세 쪽 WS 칸 상태 한 줄 — Engine::websocket_slots_의 원소다.
struct WebSocketSlotState
{
    int32_t                               priority = websocket_slot::kUnranked;
    std::chrono::steady_clock::time_point on_since{}; // 칸을 쥔 것을 처음 본 때. 칸 밖이면 0
    bool                                  warned_off_socket = false; // 보유·선점인데 칸 밖이라고 남겼나
};

// 종목 표와 id 인덱스 현재가 캐시.
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
    std::vector<uint8_t> exit_managed{std::vector<uint8_t>(table.capacity(), 0)}; // 값 0/1. vector<bool> 은 비트 단위라 참조를 못 돌려준다
};

} // namespace engine
