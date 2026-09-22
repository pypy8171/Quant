#ifdef HAS_ZMQ
#include "ipc/ZmqBridge.h"
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
// TRADE 링 용량. 송신 루프가 한 바퀴에 REP 폴링 10 ms를 쉬므로 초당 처리량 상한은 (용량 × 100)건이다 —
//  1,000이면 10만 건/s로 장 초반 전 시장 피드가 넘친다. 봉투 하나 ~100 B라 8,192칸은 1 MB가 안 된다.
constexpr size_t kTradeQueueCap    = 8192;
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
    LOG_INFO("[ZMQ] 브리지 시작 — PUB:" + std::to_string(pub_port_) + " REP:" + std::to_string(rep_port_));
    return true;
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
    zmq::socket_t rep{context, zmq::socket_type::rep};

    try
    {
        publish_socket.bind("tcp://" + bind_address_ + ":" + std::to_string(pub_port_));
        rep.bind("tcp://" + bind_address_ + ":" + std::to_string(rep_port_));
    }
    catch (const zmq::error_t& zmq_error)
    {
        LOG_ERROR(std::string("[ZMQ] 소켓 bind 실패: ") + zmq_error.what());
        running_.store(false);
        return;
    }

    // REP 소켓 폴링 대상 (아래 루프에서 타임아웃 폴링으로 확인)
    zmq::pollitem_t items[] = {{rep, 0, ZMQ_POLLIN, 0}};

    while (running_.load())
    {
        // 1. 송신 큐 소진 — 락 안에서는 스왑만 하고 전송은 락 밖에서(Logger writer와 같은 패턴).
        //    락을 쥔 채 큐 상한(10만 건)까지 밀어내면 그동안 전략·주문·WS 콜백의 enqueue가 전부 선다.
        std::queue<Message> local;
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            std::swap(local, send_queue_);
        }

        // 멀티파트: frame1=topic, frame2=payload. 두 프레임 다 dontwait — 이 스레드가 REP 폴링도 맡아
        //  전송에서 멈추면 명령 채널까지 같이 선다. PUB는 HWM에서 드롭이 정상 동작이다.
        const auto send_frames = [&](Topic topic, std::string_view payload)
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
                }
                else
                {
                    ++drop_count_;
                }
            }
            catch (const zmq::error_t& zmq_error)
            {
                ++drop_count_;
                LOG_WARN(std::string("[ZMQ] publish 실패 topic=") + topic_name(topic) + " : " + zmq_error.what());
            }
        };

        while (!local.empty())
        {
            const auto& front = local.front();
            send_frames(front.topic, front.payload);
            local.pop();
        }

        // 1b. TRADE 링 소진 — 문자열은 여기서 만든다(버퍼 하나를 돌려 쓴다). 링 용량이 한 바퀴 상한이다.
        while (const auto envelope = trade_queue_.pop())
        {
            format_trade(*envelope, trade_payload_);
            send_frames(Topic::Trade, trade_payload_);
        }

        // 2. 명령 수신 (REP, kReplyPollTimeout 타임아웃)
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
                    catch (...)
                    {
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

void ZmqBridge::enqueue(Topic topic, std::string payload)
{
    std::lock_guard<std::mutex> lock(queue_mutex_);
    // (C8) 토픽별 drop 차등 — 원장 정합성에 직결되는 FILL/ORDER/SIGNAL은
    // HEALTH보다 훨씬 큰 하드캡까지 보존한다. TRADE는 이 큐를 거치지 않는다(trade_queue_).
    const bool   critical = (topic == Topic::Fill || topic == Topic::Order || topic == Topic::Signal);
    const size_t capacity = critical ? kCriticalQueueCap : kNormalQueueCap;

    if (send_queue_.size() >= capacity)
    {
        ++drop_count_;

        if (critical)
        {
            LOG_ERROR(std::string("[ZMQ] 치명적 메시지 drop! topic=") + topic_name(topic) +
                      " queue=" + std::to_string(send_queue_.size()) +
                      " (구독자 다운 의심) — 원장 불일치 위험");
        }

        return;
    }

    send_queue_.push({topic, std::move(payload)});
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
        ++drop_count_;
    }
}

// 예전 nlohmann dump()와 같은 문자열: 키는 알파벳순, 실수는 최단 표기 + ".0". 티커는 거래소 코드(숫자·영대문자)라
//  이스케이프할 글자가 없다 — 따옴표·역슬래시가 섞인 코드는 거래소가 내지 않는다.
void ZmqBridge::format_trade(const TradeEnvelope& envelope, std::string& out)
{
    const TradeData& trade = envelope.trade;
    out.clear();
    out.append("{\"direction\":");
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

void ZmqBridge::publish_health(uint64_t data_count, uint64_t signal_count, uint64_t order_count)
{
    json document;
    document["ts"]    = now_ms();
    document["data"]  = data_count;
    document["signal"] = signal_count;
    document["order"] = order_count;
    document["drop"]  = drop_count_.load();
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
    document["regime"]       = regime_label_;
    enqueue(Topic::Fill, document.dump());
}

#endif // HAS_ZMQ
