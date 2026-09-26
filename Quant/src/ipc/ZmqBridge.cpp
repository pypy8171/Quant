#ifdef HAS_ZMQ
#include "ipc/ZmqBridge.h"
#include "core/RegimeFileJudge.h"
#include "utils/Logger.h"
#include "utils/ThreadName.h"

#include <charconv>
#include <chrono>
#include <cstring>
#include <nlohmann/json.hpp>
#include <string_view>
#include <type_traits>

using json = nlohmann::json;
using namespace std::chrono;
using namespace std::chrono_literals;

namespace
{
// 송신 큐 상한(밀림 처리). 원장 정합에 직결되는 토픽(FILL/ORDER/SIGNAL)은 훨씬 크게 잡아
// 구독자 지연에도 최대한 보존하고, 고빈도 TRADE/HEALTH는 작게 잡아 메모리 폭주를 막는다.
constexpr size_t kCriticalQueueCap = 100000; // FILL/ORDER/SIGNAL 하드캡
constexpr size_t kNormalQueueCap   = 1000;   // HEALTH 하드캡
// TRADE 링 용량. 한 바퀴에 링을 통째로 비우므로 상한은 (용량 × 한 바퀴 수)건이다. 주문 다리는 REP를
//  10 ms 폴링하니 초당 100바퀴, 발행 전용 다리는 생산자가 깨우니 그보다 훨씬 자주 돈다.
//  8,192칸에서 초당 67.9만 건을 걸었더니 들어온 5,450만 건의 43%(2,327만 건)를 여기서 버렸다
//  (2026-09-25 회차 M, docs/reports/stresstest/OVERVIEW.md 5.5). 버려진 것의 원인은 100% 이 한 군데였다 —
//  소켓이 안 받은 것도, 보내다 난 예외도 0이다. 한 바퀴가 (폴링 10 ms + 비우는 시간)이라 그 사이 들어온
//  1만 건 남짓이 8,192칸을 넘겼다. 8배로 올린다. 봉투 하나 ~100 B라 65,536칸도 7 MB가 안 된다.
//  실제 장 최대 유량(초당 73,315건)에서는 한 바퀴에 733건이라 어느 쪽이든 안 차지만, 전 시장 2,500종목이
//  목표라 먼저 닿는 천장이 여기다. 근본 해결은 폴링 10 ms 를 깨우기로 바꾸는 것이고 그건 따로 한다.
//  [why D-137]
constexpr size_t kTradeQueueCap    = 65536;
// 한 프레임에 싣는 체결 건수. 건마다 프레임을 보내면 봉투 두 개 할당 + memcpy + zmq_msg_send 왕복이 건당
//  붙어 발행 스레드 하나가 초당 51만 건에서 멎는다. 2026-09-25 재측정에서 들어온 것은 초당 75만 건이라
//  링을 여덟 배로 넓혀도(65,536칸) 차는 시점만 1.6초 밀렸을 뿐 1,762만 건을 버렸다
//  (docs/reports/stresstest/OVERVIEW.md 10절 (ㄱ)). 그래서 칸이 아니라 비우는 속도를 고친다 —
//  체결 500건을 JSON 배열 한 프레임에 실어 보내면 건당 남는 것은 문자열 만들기 + append 뿐이다.
//  500건이면 봉투 하나가 약 55 KB다. 더 키우면 한 프레임이 커져 받는 쪽 한 번의 파싱이 길어지고,
//  프레임이 버려질 때 한꺼번에 사라지는 건수도 같이 커진다. 다른 토픽은 건수가 적어 그대로 한 건 한 프레임이다.
//  [why D-139]
constexpr size_t kTradeBatchMax    = 500;
// 체결 한 건이 JSON 글자로 약 110 B라 넉넉하게 잡은 값이다. 묶음 버퍼를 기동 때 한 번만 늘리는 데 쓴다.
constexpr size_t kTradeJsonBytesEach = 128;
constexpr auto   kReplyPollTimeout = 10ms;   // REP 명령 수신 폴링 1회 대기 시간

// 정수·실수를 JSON 숫자 표기로 붙인다. 실수는 nlohmann과 같은 최단 왕복 표기 + 정수처럼 보이면 ".0"을 붙여
//  구독자(PYQuant/ipc/subscriber.py)가 받는 문자열이 예전 dump()와 글자 단위로 같다.
template <typename Number>
void append_number(std::string& out, Number value)
{
    char       buffer[32];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    const std::string_view text(buffer, static_cast<size_t>(result.ptr - buffer));
    out.append(text);

    if constexpr (std::is_floating_point_v<Number>)
    {
        if (text.find_first_of(".eEn") == std::string_view::npos) // n: nan/inf는 dump()도 null인데 시세엔 없다
        {
            out.append(".0");
        }
    }
}
} // namespace

// ─── 생성자/소멸자 ──────────────────────────────────────────────────────────
ZmqBridge::ZmqBridge(int pub_port, int rep_port)
    : pub_port_(pub_port), rep_port_(rep_port), trade_queue_(kTradeQueueCap)
{
    // 묶음 버퍼는 기동 때 한 번만 늘린다 — 한 봉투가 500건 × 약 110 B라 발행 중에 다시 늘 일이 없다.
    trade_batch_.reserve(kTradeBatchMax * kTradeJsonBytesEach);
}

ZmqBridge::~ZmqBridge()
{
    stop();
}

// ─── 시작/종료 ──────────────────────────────────────────────────────────────
bool ZmqBridge::start()
{
    if (running_.load())
    {
        return true;
    }

    running_.store(true);
    zmq_thread_ = std::thread(&ZmqBridge::thread_fn, this);
    LOG_INFO("[ZMQ] 브리지 시작 — PUB:" + std::to_string(pub_port_) +
             (rep_port_ > 0 ? " REP:" + std::to_string(rep_port_) : std::string(" REP:없음(발행 전용)")));
    return true;
}

void ZmqBridge::set_bind_address(std::string address)
{
    if (!address.empty())
    {
        bind_address_ = std::move(address);
    }
}

void ZmqBridge::stop()
{
    // running_ 과 관계없이 joinable 이면 거둔다 — bind 실패 경로는 스레드가 running_ 을 스스로 내려서,
    //  예전 `if (!running_) return` 은 joinable 인 std::thread 를 그대로 부쉈다(std::terminate).
    const bool was_running = running_.exchange(false);

    if (zmq_thread_.joinable())
    {
        zmq_thread_.join();
    }

    if (was_running)
    {
        LOG_INFO("[ZMQ] 브리지 종료");
    }
}

// ─── 스레드 본체 (ZMQ 소켓은 이 스레드에서만 사용) ─────────────────────────
void ZmqBridge::thread_fn()
{
    thread_name::set_current("ZmqBridge");
    zmq::context_t context{1};
    zmq::socket_t publish_socket{context, zmq::socket_type::pub};
    // 명령 소켓은 주문 쪽 프로세스에만 둔다 — rep_port_ 가 0 이하면 빈 소켓으로 두고 bind 도 폴링도 건너뛴다.
    //  시세·전략 프로세스는 발행만 하고, KILL·STATUS 는 주문 쪽 하나만 받는다. [why D-129]
    const bool    serves_commands = rep_port_ > 0;
    zmq::socket_t rep             = serves_commands ? zmq::socket_t{context, zmq::socket_type::rep} : zmq::socket_t{};

    // 받는 쪽(적재기·대시보드)이 밀릴 때 소켓이 들고 있을 건수. ZMQ 기본은 1,000건이고, PUB 소켓은
    //  이 칸이 차면 조용히 버린다 — 보낸 쪽에 실패를 알리지도, 버린 수를 세어 주지도 않는다.
    //  2026-09-25 부하시험에서 엔진이 초당 2만 행을 내보내는 동안 파이썬 적재기는 초당 300행을 넣고
    //  있었으니 1,000칸은 0.05초치였다(docs/reports/stresstest/OVERVIEW.md 5.6). 받는 쪽도 같은 크기로
    //  넓혔고(PYQuant/ipc/subscriber.py), 넣는 속도 자체는 묶음 적재로 따로 올렸다.
    //  한 건 ~200 B라 20만 칸은 40 MB 안쪽이고, 실제로 그만큼 쌓이는 것은 받는 쪽이 밀릴 때뿐이다.
    //  [inv] HWM 은 bind 보다 먼저 걸어야 그 연결에 적용된다. [why D-137]
    constexpr int kPublishHighWaterMark = 200'000;
    publish_socket.set(zmq::sockopt::sndhwm, kPublishHighWaterMark);

    try
    {
        publish_socket.bind("tcp://" + bind_address_ + ":" + std::to_string(pub_port_));

        if (serves_commands)
        {
            rep.bind("tcp://" + bind_address_ + ":" + std::to_string(rep_port_));
        }
    }
    catch (const zmq::error_t& zmq_error)
    {
        LOG_ERROR(std::string("[ZMQ] 소켓 bind 실패: ") + zmq_error.what());
        running_.store(false);
        return;
    }

    // REP 소켓 폴링 대상 (아래 루프에서 타임아웃 폴링으로 확인). 명령을 안 받는 프로세스에서는 안 쓴다.
    zmq::pollitem_t items[] = {{rep, 0, ZMQ_POLLIN, 0}};

    while (running_.load())
    {
        // 0. "보낼 것 있음" 깃발을 비우기 전에 내린다. 비운 뒤에 내리면 그 사이에 들어온 건이 깃발째
        //    지워져 다음 깨우기까지 잠든다. 먼저 내리면 헛도는 바퀴가 한 번 더 도는 것으로 끝난다.
        work_pending_.store(false, std::memory_order_relaxed);

        // 1. 송신 큐 소진 — 락 안에서는 스왑만 하고 전송은 락 밖에서(Logger writer와 같은 패턴).
        //    락을 쥔 채 큐 상한(10만 건)까지 밀어내면 그동안 전략·주문·WS 콜백의 enqueue가 전부 선다.
        std::queue<Message> local;
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            std::swap(local, send_queue_);
        }

        // 멀티파트: frame1=topic, frame2=payload. 두 프레임 다 dontwait — 이 스레드가 REP 폴링도 맡아
        //  전송에서 멈추면 명령 채널까지 같이 선다. PUB는 HWM에서 드롭이 정상 동작이다.
        // 돌려주는 값은 "소켓이 받았다"다 — 체결 묶음은 한 프레임에 여러 건이 실려, 버려졌으면 몇 건이
        //  사라졌는지 부르는 쪽만 안다.
        const auto send_frames = [&](Topic topic, std::string_view payload) -> bool
        {
            const std::string_view topic_text = topic_name(topic);
            zmq::message_t         topic_frame(topic_text.size());
            zmq::message_t         payload_frame(payload.size());
            std::memcpy(topic_frame.data(), topic_text.data(), topic_text.size());
            std::memcpy(payload_frame.data(), payload.data(), payload.size());

            try
            {
                if (publish_socket.send(topic_frame, zmq::send_flags::sndmore | zmq::send_flags::dontwait))
                {
                    (void)publish_socket.send(payload_frame, zmq::send_flags::dontwait);
                    return true;
                }

                ++socket_full_drop_count_;
            }
            catch (const zmq::error_t& zmq_error)
            {
                ++socket_error_drop_count_;
                LOG_WARN(std::string("[ZMQ] publish 실패 topic=") + topic_name(topic) + " : " + zmq_error.what());
            }

            return false;
        };

        while (!local.empty())
        {
            const auto& front = local.front();
            (void)send_frames(front.topic, front.payload);
            local.pop();
        }

        // 1b. TRADE 링 소진 — 문자열은 여기서 만든다(버퍼 하나를 돌려 쓴다). 링 용량이 한 바퀴 상한이다.
        //     한 건마다 보내지 않고 kTradeBatchMax 건씩 JSON 배열 한 프레임에 실어 보낸다. 배열이라
        //     받는 쪽(PYQuant/ipc/subscriber.py)은 첫 글자로 갈라볼 필요 없이 푼 결과가 list 인지만 본다.
        //     [why D-139]
        size_t batch_count = 0;
        trade_batch_.clear();

        const auto flush_trade_batch = [&]()
        {
            if (batch_count == 0)
            {
                return;
            }

            trade_batch_.push_back(']');

            if (!send_frames(Topic::Trade, trade_batch_))
            {
                // 프레임 하나가 버려지면 그 안의 체결이 통째로 사라진다 — 프레임 수가 아니라 건수로 센다.
                trade_socket_drop_count_ += batch_count;
            }

            trade_batch_.clear();
            batch_count = 0;
        };

        while (const auto envelope = trade_queue_.pop())
        {
            format_trade(*envelope, account_no_, trade_payload_);
            trade_batch_.push_back(batch_count == 0 ? '[' : ',');
            trade_batch_.append(trade_payload_);

            if (++batch_count >= kTradeBatchMax)
            {
                flush_trade_batch();
            }
        }

        flush_trade_batch();

        // 2. 명령 수신 (REP, kReplyPollTimeout 타임아웃). 명령을 안 받는 프로세스는 폴링할 소켓이 없으니
        //    생산자가 깨울 때까지 잔다. sleep_for 로 쉬면 안 된다 — 윈도우 기본 타이머 격자에서 10 ms 를
        //    재면 실측 p50 15.6 ms 라(Quant/include/core/WakeGate.h 머리말) 한 바퀴가 1.5배로 늘고
        //    TRADE 링의 처리량 상한(용량 × 바퀴수)이 그만큼 내려간다. 깨우면 격자와 무관하게 돈다.
        //    kReplyPollTimeout 은 종료 확인 상한이지 깨우는 수단이 아니다. [why D-129]
        if (!serves_commands)
        {
            send_gate_.wait_for(kReplyPollTimeout,
                                [this]
                                {
                                    return !work_pending_.load(std::memory_order_acquire);
                                });
            continue;
        }

        try
        {
            zmq::poll(items, 1, kReplyPollTimeout);

            if (items[0].revents & ZMQ_POLLIN)
            {
                zmq::message_t request;
                (void)rep.recv(request, zmq::recv_flags::none); // POLLIN 뒤라 실패는 예외로만 온다
                std::string command(static_cast<char*>(request.data()), request.size());

                // KILL만 토큰을 요구한다: "KILL <token>". 토큰 미설정·불일치면 핸들러에 닿지 않는다.
                //  REP는 요청마다 응답을 보내야 하므로 거부도 reply로 끝낸다.
                std::string reply_string = "OK";
                bool        allowed   = true;
                const auto  space_position        = command.find(' ');
                // 동사·토큰은 command 안을 가리키는 뷰다 — 비교에만 쓰고, 아래 resize 전까지만 유효하다.
                const std::string_view verb = std::string_view(command).substr(0, space_position);

                if (verb == "KILL")
                {
                    const std::string_view given = (space_position == std::string::npos) ? std::string_view() : std::string_view(command).substr(space_position + 1);

                    if (control_token_.empty() || given != control_token_)
                    {
                        allowed   = false;
                        reply_string = "DENIED";
                        LOG_WARN(std::string("[ZMQ] KILL 거부 — ") +
                                 (control_token_.empty() ? "zmq_control_token 미설정" : "토큰 불일치"));
                    }
                    else
                    {
                        command.resize(verb.size()); // "KILL <token>" → "KILL"
                    }
                }

                if (allowed && command_handler_)
                {
                    try
                    {
                        reply_string = command_handler_(command);
                    }
                    catch (const std::exception& exception)
                    {
                        // 처리기가 실패했는데 "OK"를 돌려주면 부른 쪽은 성공으로 안다. [why 전수조사 B2b-11]
                        reply_string = "ERROR";
                        LOG_ERROR("[ZMQ] 명령 처리 실패 '" + command + "': " + exception.what());
                    }
                    catch (...)
                    {
                        reply_string = "ERROR";
                        LOG_ERROR("[ZMQ] 명령 처리 실패 '" + command + "': 알 수 없는 예외");
                    }
                }

                zmq::message_t reply_message(reply_string.size());
                std::memcpy(reply_message.data(), reply_string.data(), reply_string.size());
                rep.send(reply_message, zmq::send_flags::none);
            }
        }
        catch (const zmq::error_t& zmq_error)
        {
            if (zmq_error.num() != ETERM)
            {
                LOG_WARN(std::string("[ZMQ] poll 오류: ") + zmq_error.what());
            }
        }
    }

    publish_socket.close();
    rep.close();
    context.close();
}

// ─── 메시지 enqueue (스레드-안전) ───────────────────────────────────────────
const char* ZmqBridge::topic_name(Topic topic)
{
    switch (topic)
    {
    case Topic::Trade:  return "TRADE";
    case Topic::Signal: return "SIGNAL";
    case Topic::Order:  return "ORDER";
    case Topic::Health: return "HEALTH";
    case Topic::Fill:   return "FILL";
    }

    return "UNKNOWN";
}

// 국면을 적는다. 공유 칸이 꽂혀 있으면 거기에도 같이 적는다 — 그 칸이 건너편 프로세스가 읽는 자리다.
//  프로세스 안 정수도 계속 적는다: 한 프로세스로 돌 때(both) 읽는 자리가 그쪽이다. [why D-129]
void ZmqBridge::set_regime(Regime regime)
{
    const int32_t code = static_cast<int32_t>(regime);
    regime_code_.store(code, std::memory_order_relaxed);

    if (regime_cell_ != nullptr)
    {
        regime_cell_->code.store(code, std::memory_order_relaxed);
    }
}

// 국면 정수를 라벨로 편다. 아직 판정이 없으면(-1) 빈 문자열 — 판정 전 행을 "UNKNOWN"으로 적으면
//  "판정이 UNKNOWN"과 구분이 안 된다. 리터럴이라 수명은 정적이다. [inv]
std::string_view ZmqBridge::current_regime_label() const
{
    // 꽂혀 있으면 공유 칸이 정본이다 — 갈라 띄운 날 주문 쪽은 제 정수를 한 번도 적지 않는다.
    const int32_t code = regime_cell_ != nullptr ? regime_cell_->code.load(std::memory_order_relaxed)
                                                 : regime_code_.load(std::memory_order_relaxed);

    if (code < 0)
    {
        return {};
    }

    return regime_file::label_of(Regime(static_cast<Regime::Value>(code)));
}

uint64_t ZmqBridge::drop_count() const
{
    return socket_full_drop_count_.load() + socket_error_drop_count_.load()
         + send_queue_full_drop_count_.load() + trade_ring_full_drop_count_.load();
}

void ZmqBridge::enqueue(Topic topic, std::string payload)
{
    uint64_t critical_dropped = 0; // 버린 치명 메시지의 누적 번호 — 로그는 락 밖에서 남긴다

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        // (C8) 토픽별 drop 차등 — 원장 정합성에 직결되는 FILL/ORDER/SIGNAL은
        // HEALTH보다 훨씬 큰 하드캡까지 보존한다. TRADE는 이 큐를 거치지 않는다(trade_queue_).
        const bool   critical = (topic == Topic::Fill || topic == Topic::Order || topic == Topic::Signal);
        const size_t capacity = critical ? kCriticalQueueCap : kNormalQueueCap;

        if (send_queue_.size() >= capacity)
        {
            const uint64_t dropped = ++send_queue_full_drop_count_;
            critical_dropped       = critical ? dropped : 0;
        }
        else
        {
            send_queue_.push({topic, std::move(payload)});
        }
    }

    // 이 큐가 차는 것은 발행 스레드가 밀릴 때다 — 구독자가 죽으면 소켓 한도에서 버려 socket_full 로 센다.
    //  폭주 때 건마다 락을 쥔 채 적으면 로그가 쏟아지고 락도 길어진다: 첫 건과 1,000건마다 한 줄. [why 전수조사 B2b-11]
    if (critical_dropped != 0)
    {
        if (critical_dropped == 1 || critical_dropped % 1000 == 0)
        {
            LOG_ERROR(std::string("[ZMQ] 치명적 메시지 drop! topic=") + topic_name(topic) + " 누적=" +
                      std::to_string(critical_dropped) + " (발행 큐 만석 — 발행 스레드 밀림) — 원장 불일치 위험");
        }

        return;
    }

    // 깨우기는 반드시 queue_mutex_ 를 놓은 뒤에 — WakeGate 는 제 뮤텍스를 잡는다. 락을 쥔 채 부르면
    //  queue_mutex_ → gate 뮤텍스 순서가 생겨, 송신 스레드가 반대 순서로 잡는 날 교착한다. [lock-order]
    mark_work_pending();
}

// 보낼 것이 생겼다고 알린다. 깃발을 먼저 세우고 깨운다 — 송신 스레드는 큐를 비우기 전에 깃발을 내리므로
//  이 순서면 "비운 뒤 들어온 건"이 깃발에 남아 잠들지 않는다. 바쁠 때 비용은 원자 쓰기 하나와
//  WakeGate 의 원자 읽기 하나뿐이다. [why D-129]
void ZmqBridge::mark_work_pending()
{
    // 주문 다리는 REP 를 폴링하느라 게이트를 보지 않는다. 거기서도 깨우면 폴링이 자는 동안 깨우기마다
    //  락을 잡아 부르는 쪽이 26.3 → 61.8 ns/틱이 된다(bench_zmq_publish, 2026-09-24). 발행 전용 다리만
    //  깨운다 — 거기서는 26.8 → 38.0 ns/틱이고, 그 대신 한 바퀴가 타이머 격자에 매이지 않는다.
    if (rep_port_ > 0)
    {
        return;
    }

    work_pending_.store(true, std::memory_order_release);
    send_gate_.notify();
}

// ─── 이벤트별 publish 헬퍼 ─────────────────────────────────────────────────
static int64_t now_ms()
{
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// 3값 enum을 2분기로 접지 않는다 — CANCEL/REPLACE는 side==NONE으로도 여기까지 온다(W-11).
static const char* side_string(OrderSide order_side)
{
    return order_side == OrderSide::BUY ? "BUY" : (order_side == OrderSide::SELL ? "SELL" : "NONE");
}

static const char* action_string(OrderAction order_action)
{
    return order_action == OrderAction::CANCEL ? "CANCEL" : (order_action == OrderAction::REPLACE ? "REPLACE" : "NEW");
}

void ZmqBridge::publish_trade(const TradeData& trade)
{
    // 수신 스레드 쪽은 memcpy 한 번뿐. 링이 차면 버린다 — TRADE는 원장과 무관해 예전 큐 상한과 같은 정책이다.
    if (!trade_queue_.push(TradeEnvelope{now_ms(), trade}))
    {
        ++trade_ring_full_drop_count_;
        return;
    }

    mark_work_pending();
}

// nlohmann dump()와 같은 문자열: 키는 알파벳순, 실수는 최단 표기 + ".0". 티커는 거래소 코드(숫자·영대문자),
//  계좌는 숫자·하이픈이라 이스케이프할 글자가 없다 — 따옴표·역슬래시가 섞인 값은 거래소·증권사가 내지 않는다.
void ZmqBridge::format_trade(const TradeEnvelope& envelope, std::string_view account, std::string& out)
{
    const TradeData& trade = envelope.trade;
    out.clear();
    out.append("{\"account\":\"");
    out.append(account);
    out.append("\",\"direction\":");
    append_number(out, trade.direction); // 1=매수, 5=매도
    out.append(",\"market\":\"");
    out.append(trade.market == Market::US ? "US" : "KR");
    out.append("\",\"price\":");
    append_number(out, trade.price);
    out.append(",\"ticker\":\"");
    out.append(trade.ticker.view());
    out.append("\",\"ts\":");
    append_number(out, envelope.ts_ms);
    out.append(",\"volume\":");
    append_number(out, trade.quantity);
    out.push_back('}');
}

void ZmqBridge::publish_signal(const OrderSignal& signal)
{
    json document;
    document["ts"] = now_ms();
    document["strategy"] = signal.strategy_id;
    document["ticker"] = signal.ticker;
    document["side"] = side_string(signal.side);
    document["action"] = action_string(signal.action);
    document["qty"] = signal.quantity;
    document["price"] = signal.price;
    document["market"] = (signal.market == Market::US ? "US" : "KR");
    document["gated"] = false; // 게이트(OrderGate) 이전 발행 — 거부될 수 있다. 결과는 ORDER 토픽.
    document["account"] = account_no_; // 받는 쪽이 남의 엔진 신호를 거르는 키 — ORDER·FILL과 같은 값
    document["regime"] = current_regime_label(); // 그때의 국면 — 신호를 국면별로 되짚을 때 쓴다
    enqueue(Topic::Signal, document.dump());
}

void ZmqBridge::publish_order(const OrderSignal& signal, bool ok)
{
    json document;
    document["ts"] = now_ms();
    document["strategy"] = signal.strategy_id;
    document["ticker"] = signal.ticker;
    document["side"] = side_string(signal.side);
    document["action"] = action_string(signal.action);
    document["qty"] = signal.quantity;
    document["price"] = signal.price;
    document["ok"] = ok;
    document["market"] = (signal.market == Market::US ? "US" : "KR");
    document["account"] = account_no_;
    enqueue(Topic::Order, document.dump());
}

void ZmqBridge::publish_health(const HealthSnapshot& snapshot)
{
    json document;
    document["ts"]     = now_ms();
    // 역할·계좌를 같이 싣는다 — 갈라 띄운 날에는 세 프로세스가 같은 health 표에 넣는데, 역할마다
    //  채우는 칸이 서로 다르다(시세는 data, 전략은 signal·샤드 큐, 주문은 order·지연). 가르는 열이
    //  없으면 읽는 쪽이 누적 카운터가 역행한 것으로 본다. 계좌는 모의·실계좌 원장을 가르던 키와 같다. [why D-129]
    document["role"]    = role_label_;
    document["account"] = account_no_;
    document["data"]   = snapshot.data_count;
    document["signal"] = snapshot.signal_count;
    document["order"]  = snapshot.order_count;
    document["drop"]   = drop_count();
    // 원인별 내역 — 합이 위 drop 이다. [why D-125]
    document["drop_socket_full"]     = socket_full_drop_count_.load();
    document["drop_socket_error"]    = socket_error_drop_count_.load();
    document["drop_send_queue_full"] = send_queue_full_drop_count_.load();
    document["drop_trade_ring_full"] = trade_ring_full_drop_count_.load();
    // 합에 들어가지 않는 곁수 — 위 drop_socket_full 이 센 프레임 중 체결 묶음에 실려 있던 건수다.
    //  프레임 하나가 500건까지 싣게 되면서(D-139) 프레임 수만으로는 몇 건이 사라졌는지 알 수 없다.
    document["drop_trade_socket"]    = trade_socket_drop_count_.load();
    // 큐와 지연 — 적재기가 health 표의 같은 이름 열에 그대로 넣는다.
    document["queue_shard_high_water"]   = snapshot.shard_high_water;
    document["queue_shard_capacity"]     = snapshot.shard_capacity;
    document["queue_shard_out_size"]     = snapshot.shard_out_size;
    document["queue_shard_out_capacity"] = snapshot.shard_out_capacity;
    document["queue_order_high_water"]   = snapshot.order_queue_high_water;
    document["queue_order_capacity"]     = snapshot.order_queue_capacity;
    document["queue_fill_high_water"]    = snapshot.fill_queue_high_water;
    document["queue_fill_capacity"]      = snapshot.fill_queue_capacity;
    document["dropped_shard"]            = snapshot.shard_dropped;
    document["dropped_order"]            = snapshot.order_dropped;
    // 큐에서 너무 오래 기다려 꺼낼 때 버린 신규 매수. dropped_order(큐가 차서 못 넣은 것)와 원인이 다르다. [why D-127]
    document["stale_order"]              = snapshot.order_stale;
    document["dropped_fill"]             = snapshot.fill_dropped;
    document["latency_samples"]          = snapshot.latency_samples;
    document["tick_to_signal_p50_us"]    = snapshot.tick_to_signal_p50_us;
    document["tick_to_signal_p99_us"]    = snapshot.tick_to_signal_p99_us;
    document["signal_to_pop_p50_us"]     = snapshot.signal_to_pop_p50_us;
    document["signal_to_pop_p99_us"]     = snapshot.signal_to_pop_p99_us;
    document["pop_to_done_p50_us"]       = snapshot.pop_to_done_p50_us;
    document["pop_to_done_p99_us"]       = snapshot.pop_to_done_p99_us;
    document["total_p50_us"]             = snapshot.total_p50_us;
    document["total_p99_us"]             = snapshot.total_p99_us;

    // 구간 분위수 — 이름은 엔진이 실어 보낸다(trace::PipelineLatency::segment_names). 열 이름을 여기서 또
    //  적으면 구간을 하나 늘릴 때마다 두 군데를 고치게 된다. [wire] PYQuant/db/client.py _HEALTH_METRIC_COLUMNS
    document["latency_interval_samples"] = snapshot.interval_samples;

    for (const auto& segment : snapshot.interval_segments)
    {
        if (segment.name.empty())
        {
            continue;
        }

        const std::string prefix(segment.name);
        document[prefix + "_p50_interval_us"] = segment.p50_us;
        document[prefix + "_p99_interval_us"] = segment.p99_us;
    }

    enqueue(Topic::Health, document.dump());
}

void ZmqBridge::publish_fill(const FillNotification& fill_notification, const std::string& strategy_id,
                              double commission, double tax, double average_price, int net_quantity,
                              double realized_pnl)
{
    json document;
    document["ts"]           = now_ms();
    document["odno"]         = fill_notification.kis_order_no;
    document["ticker"]       = fill_notification.ticker;
    document["side"]         = side_string(fill_notification.side);
    document["filled_qty"]   = fill_notification.filled_quantity;
    document["filled_price"] = fill_notification.filled_price;
    document["commission"]   = commission;
    document["tax"]          = tax;
    document["avg_price"]    = average_price;
    document["net_qty"]      = net_quantity;
    document["realized_pnl"] = realized_pnl;
    document["account"]      = account_no_;
    document["strategy"]     = strategy_id;
    document["regime"]       = current_regime_label();
    enqueue(Topic::Fill, document.dump());
}

#endif // HAS_ZMQ
