// 피드 소스 여러 개를 한 IFeedSource로 묶는다 — Engine은 소켓이 몇 개든 하나만 본다. 소켓마다 수신 스레드가 따로
//  돌고(원칙 1), 종목은 한 소스에만 배정된다(원칙 2). 콜백 전달은 두 모드다 — 직접 호출 모드(set_lane_callbacks)는 소스 i의
//  수신 스레드가 번호 i를 달고 Engine 콜백을 직접 부른다(Engine의 N행 행렬이 소비자라 생산자를 모을 이유가 없다, 원칙 5).
//  multiplexer 모드(set_callbacks)는 소스당 SPSC 링 하나를 multiplexer 스레드 하나가 돌아가며 비워 콜백을 부른다 — 소비자가 하나여야
//  하는 쪽(시험·단일 큐)만 쓴다. 소켓이 하나면 끼우지 않는다.
// 스레드: 직접 호출 모드는 소스의 수신 스레드가 콜백까지. multiplexer 모드는 수신 스레드가 push, multiplexer 스레드가 pop·콜백.
//  connect/subscribe는 Engine의 제어·데이터 스레드. 체결통보는 **맡은 소스 하나**만 넘긴다(owns_fill_notice가
//  참인 첫 소스) — KIS는 세션마다 같은 통보를 보내므로 둘 이상 받으면 원장이 두 번 센다. 맡은 자리가 0번으로
//  고정돼 있지 않아 나중에 그 세션만 주문 쪽으로 뗄 수 있다(D-114 단계 3). 직접 호출 모드에서도 그 소스 스레드
//  하나만 부르므로 통보 큐는 SPSC로 남는다. [why D-071]
#pragma once
#include "core/IFeedSource.h"
#include "core/RingBuffer.h"
#include "core/Types.h"
#include "core/WakeGate.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace feed
{

class FeedMux final : public IFeedSource
{
public:
    using Event = std::variant<OrderBook, TradeData, FillNotification>;

    // ring_capacity는 소스당 링 슬롯 수(2의 거듭제곱으로 올림). 가득 차면 수신 스레드는 버리고 dropped()로 센다 —
    //  막히면 소켓 뒤로 밀려 그 소스의 모든 종목이 늦어진다(원칙 3).
    explicit FeedMux(std::vector<std::unique_ptr<IFeedSource>> sources, size_t ring_capacity = 1u << 16);

    ~FeedMux() override
    {
        multiplexer_thread_ = {}; // request_stop + join. 소스 소멸(수신 스레드 정지)보다 먼저 — 링을 비우는 쪽이 먼저 선다.
    }

    FeedMux(const FeedMux&)            = delete;
    FeedMux& operator=(const FeedMux&) = delete;

    uint32_t lanes() const override
    {
        return static_cast<uint32_t>(sources_.size());
    }

    // 직접 호출 모드 — 소스 i의 수신 스레드가 번호 i를 달고 Engine 콜백을 직접 부른다. 링·multiplexer 스레드를 거치지 않는다.
    //  set_callbacks와 같이 쓰지 않는다(나중에 부른 쪽이 소스 콜백을 덮는다).
    void set_lane_callbacks(LaneOrderBookCb on_order_book, LaneTradeCb on_trade) override;

    // multiplexer 모드 — 소스마다 "자기 링에 push" 콜백을 등록하고 multiplexer 스레드를 띄운다. Engine 콜백은 multiplexer 스레드에서만 불린다.
    void set_callbacks(OrderBookCb on_order_book, TradeCb on_trade) override;

    void set_fill_callback(FillCb callback) override;

    // 묶인 소스 중 하나라도 체결통보를 맡으면 참.
    bool owns_fill_notice() const override;

    // 체결통보를 맡은 소스들의 번호. 비면 아무도 안 맡은 것이고, 둘 이상이면 설정이 잘못된 것이다
    //  (원장이 체결을 두 번 센다). Engine이 기동 로그·판정에 쓰고 테스트가 읽는다. [why D-114]
    [[nodiscard]] std::vector<size_t> fill_notice_sources() const;

    // specs를 소스에 고르게 나눠(i % N) 각각 connect한다. 이미 배정된 종목은 그 소스를 지킨다(재연결).
    //  하나라도 실패하면 전부 끊고 false — Engine의 재연결 판단이 소스 단위가 아니라 하나이기 때문이다.
    bool connect(const std::vector<WatchSpec>& specifications) override;

    void disconnect() override;

    // 새 종목은 배정 수가 가장 적은 소스로. 상한에 걸려 그 소스 목록에서도 빠지면 배정을 지워 has_spec이 false가 된다.
    bool subscribe_incremental(const WatchSpec& specification) override;

    bool has_specification(const WatchSpec& specification) const override;

    std::vector<WatchSpec> take_overflow_specifications() override;

    bool is_connected() const override;

    // 소스 하나라도 끊겼거나 멈췄으면 stale — 어느 것인지는 reconnect_stale이 다시 가려 그것만 잇는다.
    bool is_stale(int threshold_sec) const override;

    // 멈췄거나 끊긴 소스만 자기 배정 종목으로 다시 잇는다. 배정이 없는 종목(넘침 회수분 등)은 다시 잇는 소스에 돌아가며
    //  붙인다. 전부 멈췄으면(또는 멈춘 것이 없는데 불렸으면) 기본 동작 — 전부 끊고 전부 다시 — 과 같다.
    //  실패한 소스는 끊긴 채 남아 다음 판정에서 다시 멈춘 것으로 잡힌다.
    bool reconnect_stale(const std::vector<WatchSpec>& specifications, int threshold_sec) override;

    [[nodiscard]] size_t source_count() const noexcept
    {
        return sources_.size();
    }

    // 측정용(원칙 7). 링이 찼을 때 버린 이벤트 수와 소스별 고수위.
    [[nodiscard]] uint64_t dropped() const noexcept
    {
        return dropped_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] size_t high_water(size_t source_index) const noexcept
    {
        return source_index < lanes_.size() ? lanes_[source_index]->ring.high_water() : 0;
    }

    // 종목이 어느 소스에 배정됐는지. 없으면 nullopt. 테스트·진단용.
    [[nodiscard]] std::optional<size_t> source_of(const WatchSpec& specification) const;

private:
    struct Lane
    {
        explicit Lane(size_t capture) : ring(capture) {}

        RingBuffer<Event> ring;
    };

    static std::string key(const WatchSpec& specification)
    {
        return specification.is_future ? specification.ticker + "/F" : specification.ticker;
    }

    // 수신 스레드에서. 링이 차면 버린다 — 여기서 기다리면 그 소켓의 전 종목이 밀린다.
    void enqueue(Lane& lane, Event&& event);

    size_t least_loaded_locked() const;

    bool all_empty() const;

    // 링을 돌아가며 비운다. 한 링당 한 바퀴에 kBurst개까지만 — 한 소켓이 바쁘다고 다른 소켓 종목이 밀리지 않게.
    void multiplexer_loop(std::stop_token stop_token);

    void dispatch(Event& event);

    std::vector<std::unique_ptr<IFeedSource>> sources_;
    std::vector<std::unique_ptr<Lane>>        lanes_; // sources_와 같은 index

    OrderBookCb     on_order_book_;    // multiplexer 모드
    TradeCb         on_trade_; // multiplexer 모드
    LaneOrderBookCb lane_order_book_;    // 직접 호출 모드
    LaneTradeCb     lane_trade_; // 직접 호출 모드
    FillCb          on_fill_;
    bool            lane_mode_ = false; // 수신 스레드가 돌기 전(connect 전)에 정해진다

    mutable std::mutex                      assign_mutex_;
    // 문자열 키인 이유: 소스 계층은 종목 테이블 앞이라 id가 아직 없고, 키에 선물 접미사("/F")가 붙는 구독 스펙이다.
    //  연결·증분 구독 때만 만진다(틱 경로 아님).
    std::unordered_map<std::string, size_t> assign_; // key(specification) → 소스 index. 한 종목은 한 소스에만

    wake::WakeGate        wake_;
    std::atomic<uint64_t> dropped_{0};
    std::jthread          multiplexer_thread_; // 멤버 선언 순서상 마지막 — 소멸자가 먼저 세운다
};

} // namespace feed
