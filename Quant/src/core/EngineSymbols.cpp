// 종목 쪽 — 티커↔정수 id 풀이·등록, 종목명 라벨, 최근 가격.
//  Engine 클래스는 그대로다. Engine.cpp 가 1,900줄을 넘겨 열기 어려워 이 갈래만 따로 낸 것이다
//  (헤더는 한 줄도 안 바뀐다 — 같은 Engine 의 멤버 함수 본체가 여기 있을 뿐이다).
//
//  ── 부르는 자리 ──────────────────────────────────────────────────────────
//  lookup_symbol() · register_symbol()      : 전략 적재·구독 목록 만들기·폴러 가격 반영
//  request_symbol_registration() 등          : 전략 쪽이 장중에 새 종목·전략 id 를 주문 쪽에 받아 올 때
//  last_price() · set_last_price()          : 전략(읽기)·시세 입력과 폴러(쓰기)
//  register_ticker_name() · ticker_label()  : 스캔·청산 관리 부착(쓰기), 로그 문구(읽기)

#include "core/Engine.h"
#include "core/LatencyTrace.h"
#include "utils/Logger.h"
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace
{

// 전략 쪽이 새 번호를 기다리는 시간. 주문 쪽이 요청을 집어 표에 넣고 그 값이 같은 공유 표에
//  뜨기까지 걸리는 시간이다 — 느린 경로(기동·재스캔·바스켓)에서만 기다린다.
//  스레드가 뜬 뒤에는 짧게 본다. 그 스레드가 기다리는 동안 틱도 신호도 멎기 때문이다.
//  다만 주문 쪽 잠 깨우기(order_wake)는 프로세스를 못 넘는다 — 제어 줄에 넣어도 건너편은
//  제 잠 만기(100ms)가 되어서야 집는다. 그 만기보다 넉넉히 길게 잡는다. [why D-114]
constexpr auto kRegisterWaitRunning = std::chrono::milliseconds(300);
constexpr auto kRegisterPollRunning = std::chrono::microseconds(200);

// 기동 중(스레드 전)에는 길게 본다. 건너편도 제 유니버스 스캔·잔고 대조를 하느라 제어 줄을 몇 분 뒤에
//  집을 수 있고, 여기서 접으면 그 종목·전략이 통째로 빠진 채 장을 연다 — 기다려도 잃는 것이 없는 구간이다.
//  [why D-114] 주문 쪽이 전략 적재를 건너뛰게 만들면(다음 단계) 이 기다림은 짧아진다.
constexpr auto kRegisterWaitStartup = std::chrono::minutes(5);
constexpr auto kRegisterPollStartup = std::chrono::milliseconds(5);
constexpr auto kRegisterNoticeEvery = std::chrono::seconds(10);

// 건너편 박동이 이만큼 끊겼으면 답을 줄 쪽이 없다고 보고 그 자리에서 접는다. 사망 문턱(1초)보다 훨씬
//  길게 잡는다 — 기동 중 주문 스레드는 잔고 대조로 한참 붙들려 있을 수 있어, 살아 있는 쪽을 죽었다고
//  읽으면 그 종목이 통째로 빠진 채 장을 연다. 5분을 기다리는 것보다 낫기만 하면 된다. [why D-114]
constexpr auto kRegisterPeerSilent = std::chrono::seconds(30);

static_assert(symbol::kNone == 0 && strategy_table::kNone == 0, "둘 다 0이어야 한 함수로 기다린다");

// 번호가 표에 뜰 때까지 본다. 뜨면 그 번호, 시간이 다하면 0.
//  abandoned 는 기동 중 한 번 빈손으로 접었는지를 들고 있는 래치, peer_beat_ns 는 건너편 박동 시각이다
//  (0이면 아직 한 번도 안 뛰었거나 볼 자리가 없다 — 그때는 살아 있는 것으로 본다).
[[nodiscard]] uint32_t wait_for_shared_id(const std::function<uint32_t()>& lookup, bool running, const std::string& what,
                                          std::atomic<bool>& abandoned, const std::function<int64_t()>& peer_beat_ns)
{
    using Duration = std::chrono::steady_clock::duration;

    // 이미 한 번 접었으면 한 번만 보고 만다. 기다려도 같은 답이 온다.
    if (!running && abandoned.load(std::memory_order_acquire))
    {
        return lookup();
    }

    const auto start    = std::chrono::steady_clock::now();
    const auto deadline = start + (running ? std::chrono::duration_cast<Duration>(kRegisterWaitRunning)
                                           : std::chrono::duration_cast<Duration>(kRegisterWaitStartup));
    const auto poll     = running ? std::chrono::duration_cast<Duration>(kRegisterPollRunning)
                                  : std::chrono::duration_cast<Duration>(kRegisterPollStartup);
    auto       notice   = start + kRegisterNoticeEvery;

    while (std::chrono::steady_clock::now() < deadline)
    {
        if (const uint32_t id = lookup(); id != 0)
        {
            return id;
        }

        if (!running && std::chrono::steady_clock::now() >= notice)
        {
            LOG_WARN("[Engine] " + what + " 번호를 주문 쪽에서 아직 못 받았다 — 계속 기다린다");
            notice = std::chrono::steady_clock::now() + kRegisterNoticeEvery;

            // 박동은 10초에 한 번만 본다 — 여기서 도는 값이라 자주 볼 까닭이 없다.
            if (const int64_t beat = peer_beat_ns ? peer_beat_ns() : 0; beat != 0)
            {
                const int64_t now_ns = trace::now_ns();

                if (now_ns - beat > std::chrono::duration_cast<std::chrono::nanoseconds>(kRegisterPeerSilent).count())
                {
                    LOG_ERROR("[Engine] " + what + " 번호를 기다리다 접는다 — 주문 쪽 박동이 " +
                              std::to_string((now_ns - beat) / 1000000) + "ms 끊겼다");
                    abandoned.store(true, std::memory_order_release);

                    return 0;
                }
            }
        }

        std::this_thread::sleep_for(poll);
    }

    // 기동 중에 시한을 다 썼다. 까닭은 다음 종목에서도 같으므로 래치를 세운다.
    if (!running)
    {
        abandoned.store(true, std::memory_order_release);
    }

    return 0;
}

} // namespace

// ─── 티커→종목명 라벨 (로그 가독성) ─────────────────────────────────────────
//  스캔·청산 관리 부착 스레드가 write, 전략 스레드 신호 로그가 read라 뮤텍스로 보호.
void Engine::register_ticker_name(symbol::SymbolId symbol, const std::string& name)
{
    if (name.empty())
    {
        return;
    }

    if (symbol == symbol::kNone || symbol >= ticker_names_.size())
    {
        return;
    }

    std::lock_guard<std::mutex> lock(ticker_names_mutex_);
    ticker_names_[symbol] = name;
}

double Engine::last_price(symbol::SymbolId id) const noexcept
{
    return id < symbols_.table.capacity() ? symbols_.last_price_array[id].load(std::memory_order_relaxed) : 0.0;
}

double Engine::last_price(const std::string& ticker) const
{
    return last_price(symbols_.table.lookup(ticker));
}

int64_t Engine::last_price_at_ns(const std::string& ticker) const
{
    const auto id = symbols_.table.lookup(ticker);
    return id < symbols_.table.capacity() ? symbols_.last_price_at_ns[id].load(std::memory_order_relaxed) : 0;
}

void Engine::set_last_price(symbol::SymbolId id, double price) noexcept
{
    // id 0(미배선)과 상한 밖은 버린다 — 캐시가 틀리는 것보다 비는 쪽이 낫다.
    if (price <= 0.0 || id == symbol::kNone || id >= symbols_.table.capacity())
    {
        return;
    }

    symbols_.last_price_array[id].store(price, std::memory_order_relaxed);
    symbols_.last_price_at_ns[id].store(trace::now_ns(), std::memory_order_relaxed);
}

void Engine::set_last_price(const std::string& ticker, double price)
{
    // 폴러가 주는 티커는 이미 구독 목록에 있는 것뿐이다 — 여기서 새로 넣지 않는다.
    set_last_price(lookup_symbol(ticker), price);
}

symbol::SymbolId Engine::lookup_symbol(std::string_view ticker) noexcept
{
    const symbol::SymbolId id = symbols_.table.lookup(ticker);

    if (id == symbol::kNone)
    {
        symbol_lookup_misses_.fetch_add(1, std::memory_order_relaxed);
    }

    return id;
}

symbol::SymbolId Engine::register_symbol(std::string_view ticker)
{
    // 표에 넣는 쪽은 주문 프로세스 하나다 — 양쪽이 각자 번호를 찍으면 같은 번호가 다른 종목을 가리킨다.
    //  갈라 띄우면 표 자체가 넣기를 주문 쪽으로 돌리므로(adopt_shared_dictionaries) 부르는 자리는 역할을
    //  몰라도 된다. 아래 한 갈래는 표를 아직 안 바꾼 채 번호를 안 다는 역할로 도는 길을 막는 것이다 —
    //  자리표를 못 깔았거나 단위 시험이 역할만 바꿔 도는 때다. 시세 역할도 번호를 안 딴다. [why D-114]
    if (!runs_order_side())
    {
        const symbol::SymbolId known = symbols_.table.lookup(ticker);

        if (known != symbol::kNone)
        {
            return known;
        }

        // 시세 역할은 청하지도 않는다 — 제어 줄은 보내는 쪽 하나(전략)로 SPSC가 서 있어, 시세가 같은 줄에
        //  끼면 깨진다. 구독 목록은 전략이 번호를 붙여 넘기므로 여기 모르는 티커가 오는 것은 구독하지 않은
        //  종목이 세션에 실려 온 때다 — 버리고 센다. [why D-114 단계 5]
        if (role_ == ProcessRole::Feed)
        {
            unknown_ticker_dropped_.fetch_add(1, std::memory_order_relaxed);

            return symbol::kNone;
        }

        return request_symbol_registration(ticker);
    }

    return symbols_.table.intern(ticker);
}

symbol::SymbolId Engine::request_symbol_registration(std::string_view ticker)
{
    ipc::ControlRequest request;
    request.kind   = ipc::ControlKind::kRegisterSymbol;
    request.ticker = ticker;

    if (control_plane_.send(request))
    {
        // 앞 토막에서 경계 너머로 직접 옮긴다 — 평소 옮겨 주는 전략 스레드가 바로 이 자리에서 번호를
        //  기다리고 있을 수 있다. 그때는 아무도 안 옮겨 기다림이 헛돈다. [why D-114]
        control_plane_.relay();

        // 답을 따로 받지 않는다 — 주문 쪽이 넣으면 같은 공유 표에 뜬다. 그것을 본다.
        const symbol::SymbolId id = wait_for_shared_id([this, ticker]
        {
            return symbols_.table.lookup(ticker);
        },
                                                       start_was_called_.load(std::memory_order_acquire),
                                                       "종목 " + std::string(ticker), register_wait_abandoned_,
                                                       [this]
                                                       {
                                                           return peer_order_beat_ns();
                                                       });

        if (id != symbol::kNone)
        {
            return id;
        }
    }

    symbol_register_timeouts_.fetch_add(1, std::memory_order_relaxed);
    LOG_WARN("[Engine] 종목 " + std::string(ticker) + " 등록을 주문 쪽에서 못 받았다 — 이번 줄을 접는다");
    return symbol::kNone;
}

strategy_table::StrategyId Engine::request_strategy_registration(std::string_view name)
{
    ipc::ControlRequest request;
    request.kind = ipc::ControlKind::kRegisterStrategy;
    request.strategy_name.assign(name);

    if (control_plane_.send(request))
    {
        control_plane_.relay(); // 종목 등록과 같은 이유로 이 자리에서 직접 옮긴다 [why D-114]

        const strategy_table::StrategyId id =
            wait_for_shared_id([this, name]
            {
                return order_gate_.ledger().strategy_table().lookup(name);
            },
                               start_was_called_.load(std::memory_order_acquire), "전략 " + std::string(name),
                               register_wait_abandoned_, [this]
                               {
                                   return peer_order_beat_ns();
                               });

        if (id != strategy_table::kNone)
        {
            return id;
        }
    }

    strategy_register_timeouts_.fetch_add(1, std::memory_order_relaxed);
    LOG_WARN("[Engine] 전략 " + std::string(name) + " 등록을 주문 쪽에서 못 받았다 — 손익 귀속이 빈 채로 간다");
    return strategy_table::kNone;
}

// 값으로 돌려준다 — ticker_names_는 뮤텍스 아래 갱신되므로 락을 벗어난 참조는 쓸 수 없다.
std::string Engine::ticker_label(symbol::SymbolId symbol) const
{
    std::string label = symbols_.table.name(symbol).string();
    const std::string name = ticker_name(symbol);

    if (!name.empty())
    {
        label += "(" + name + ")";
    }

    return label;
}

std::string Engine::ticker_label(const std::string& ticker) const
{
    const symbol::SymbolId symbol = symbols_.table.lookup(ticker);

    if (symbol == symbol::kNone)
    {
        return ticker; // 테이블에 없는 티커(외국 종목·오타)는 원문 그대로
    }

    return ticker_label(symbol);
}

// 값으로 돌려준다 — 위와 같은 이유(락 밖 참조 금지).
std::string Engine::ticker_name(symbol::SymbolId symbol) const
{
    if (symbol == symbol::kNone || symbol >= ticker_names_.size())
    {
        return std::string();
    }

    std::lock_guard<std::mutex> lock(ticker_names_mutex_);
    return ticker_names_[symbol];
}

void Engine::register_ticker_name(const std::string& ticker, const std::string& name)
{
    register_ticker_name(register_symbol(ticker), name);
}
