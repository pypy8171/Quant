#ifdef HAS_ZMQ
#include "ipc/ZmqBridge.h"
#include "utils/Logger.h"

#include <chrono>
#include <nlohmann/json.hpp>
#include <sstream>

using json = nlohmann::json;
using namespace std::chrono;
using namespace std::chrono_literals;

namespace
{
// 송신 큐 상한(밀림 처리). 원장 정합에 직결되는 토픽(FILL/ORDER/SIGNAL)은 훨씬 크게 잡아
// 구독자 지연에도 최대한 보존하고, 고빈도 TRADE/HEALTH는 작게 잡아 메모리 폭주를 막는다.
constexpr size_t kCriticalQueueCap = 100000; // FILL/ORDER/SIGNAL 하드캡
constexpr size_t kNormalQueueCap   = 1000;   // TRADE/HEALTH 하드캡
constexpr auto   kReplyPollTimeout = 10ms;   // REP 명령 수신 폴링 1회 대기 시간
} // namespace

// ─── 생성자/소멸자 ──────────────────────────────────────────────────────────
ZmqBridge::ZmqBridge(int pub_port, int rep_port) : pub_port_(pub_port), rep_port_(rep_port)
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
    zmq::context_t ctx{1};
    zmq::socket_t pub{ctx, zmq::socket_type::pub};
    zmq::socket_t rep{ctx, zmq::socket_type::rep};

    try
    {
        pub.bind("tcp://" + bind_addr_ + ":" + std::to_string(pub_port_));
        rep.bind("tcp://" + bind_addr_ + ":" + std::to_string(rep_port_));
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
        std::queue<Msg> local;
        {
            std::lock_guard<std::mutex> lock(queue_mtx_);
            std::swap(local, send_queue_);
        }

        while (!local.empty())
        {
            auto& front = local.front();
            // 멀티파트: frame1=topic, frame2=payload. 두 프레임 다 dontwait — 이 스레드가 REP 폴링도 맡아
            //  전송에서 멈추면 명령 채널까지 같이 선다. PUB는 HWM에서 드롭이 정상 동작이다.
            zmq::message_t t_frame(front.topic.size());
            zmq::message_t p_frame(front.payload.size());
            std::memcpy(t_frame.data(), front.topic.data(), front.topic.size());
            std::memcpy(p_frame.data(), front.payload.data(), front.payload.size());

            try
            {
                if (pub.send(t_frame, zmq::send_flags::sndmore | zmq::send_flags::dontwait))
                {
                    pub.send(p_frame, zmq::send_flags::dontwait);
                }
                else
                {
                    ++drop_count_;
                }
            }
            catch (const zmq::error_t& zmq_error)
            {
                ++drop_count_;
                LOG_WARN(std::string("[ZMQ] publish 실패 topic=") + front.topic + " : " + zmq_error.what());
            }

            local.pop();
        }

        // 2. 명령 수신 (REP, kReplyPollTimeout 타임아웃)
        try
        {
            zmq::poll(items, 1, kReplyPollTimeout);

            if (items[0].revents & ZMQ_POLLIN)
            {
                zmq::message_t req;
                rep.recv(req, zmq::recv_flags::none);
                std::string cmd(static_cast<char*>(req.data()), req.size());

                // KILL만 토큰을 요구한다: "KILL <token>". 토큰 미설정·불일치면 핸들러에 닿지 않는다.
                //  REP는 요청마다 응답을 보내야 하므로 거부도 reply로 끝낸다.
                std::string reply_str = "OK";
                bool        allowed   = true;
                const auto  sp        = cmd.find(' ');
                const std::string verb = cmd.substr(0, sp);

                if (verb == "KILL")
                {
                    const std::string given = (sp == std::string::npos) ? std::string() : cmd.substr(sp + 1);

                    if (control_token_.empty() || given != control_token_)
                    {
                        allowed   = false;
                        reply_str = "DENIED";
                        LOG_WARN(std::string("[ZMQ] KILL 거부 — ") +
                                 (control_token_.empty() ? "zmq_control_token 미설정" : "토큰 불일치"));
                    }
                    else
                    {
                        cmd = verb;
                    }
                }

                if (allowed && cmd_handler_)
                {
                    try
                    {
                        reply_str = cmd_handler_(cmd);
                    }
                    catch (...)
                    {
                    }
                }

                zmq::message_t reply_msg(reply_str.size());
                std::memcpy(reply_msg.data(), reply_str.data(), reply_str.size());
                rep.send(reply_msg, zmq::send_flags::none);
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

    pub.close();
    rep.close();
    ctx.close();
}

// ─── 메시지 enqueue (스레드-안전) ───────────────────────────────────────────
void ZmqBridge::enqueue(std::string topic, std::string payload)
{
    std::lock_guard<std::mutex> lock(queue_mtx_);
    // (C8) 토픽별 drop 차등 — 원장 정합성에 직결되는 FILL/ORDER/SIGNAL은
    // 고빈도 TRADE/HEALTH보다 훨씬 큰 하드캡까지 보존한다.
    const bool critical = (topic == "FILL" || topic == "ORDER" || topic == "SIGNAL");
    const size_t capacity = critical ? kCriticalQueueCap : kNormalQueueCap;

    if (send_queue_.size() >= capacity)
    {
        ++drop_count_;

        if (critical)
        {
            LOG_ERROR("[ZMQ] 치명적 메시지 drop! topic=" + topic +
                      " queue=" + std::to_string(send_queue_.size()) +
                      " (구독자 다운 의심) — 원장 불일치 위험");
        }

        return;
    }

    send_queue_.push({std::move(topic), std::move(payload)});
}

// ─── 이벤트별 publish 헬퍼 ─────────────────────────────────────────────────
static int64_t now_ms()
{
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// 3값 enum을 2분기로 접지 않는다 — CANCEL/REPLACE는 side==NONE으로도 여기까지 온다(W-11).
static const char* side_str(OrderSide order_side)
{
    return order_side == OrderSide::BUY ? "BUY" : (order_side == OrderSide::SELL ? "SELL" : "NONE");
}

static const char* action_str(OrderAction order_action)
{
    return order_action == OrderAction::CANCEL ? "CANCEL" : (order_action == OrderAction::REPLACE ? "REPLACE" : "NEW");
}

void ZmqBridge::publish_trade(const TradeData& trade)
{
    json document;
    document["ts"] = now_ms();
    document["ticker"] = trade.ticker;
    document["price"] = trade.price;
    document["volume"] = trade.quantity;
    document["direction"] = trade.direction; // 1=매수, 5=매도
    document["market"] = (trade.market == Market::US ? "US" : "KR");
    enqueue("TRADE", document.dump());
}

void ZmqBridge::publish_signal(const OrderSignal& signal)
{
    json document;
    document["ts"] = now_ms();
    document["strategy"] = signal.strategy_id;
    document["ticker"] = signal.ticker;
    document["side"] = side_str(signal.side);
    document["action"] = action_str(signal.action);
    document["qty"] = signal.quantity;
    document["price"] = signal.price;
    document["market"] = (signal.market == Market::US ? "US" : "KR");
    document["gated"] = false; // 게이트(OrderGate) 이전 발행 — 거부될 수 있다. 결과는 ORDER 토픽.
    enqueue("SIGNAL", document.dump());
}

void ZmqBridge::publish_order(const OrderSignal& signal, bool ok)
{
    json document;
    document["ts"] = now_ms();
    document["strategy"] = signal.strategy_id;
    document["ticker"] = signal.ticker;
    document["side"] = side_str(signal.side);
    document["action"] = action_str(signal.action);
    document["qty"] = signal.quantity;
    document["price"] = signal.price;
    document["ok"] = ok;
    document["market"] = (signal.market == Market::US ? "US" : "KR");
    document["account"] = account_no_;
    enqueue("ORDER", document.dump());
}

void ZmqBridge::publish_health(uint64_t data_cnt, uint64_t sig_cnt, uint64_t ord_cnt)
{
    json document;
    document["ts"]    = now_ms();
    document["data"]  = data_cnt;
    document["signal"] = sig_cnt;
    document["order"] = ord_cnt;
    document["drop"]  = drop_count_.load();
    enqueue("HEALTH", document.dump());
}

void ZmqBridge::publish_fill(const FillNotification& fill_notification, const std::string& strategy_id,
                              double commission, double tax, double average_price, int net_qty,
                              double realized_pnl)
{
    json document;
    document["ts"]           = now_ms();
    document["odno"]         = fill_notification.kis_order_no;
    document["ticker"]       = fill_notification.ticker;
    document["side"]         = side_str(fill_notification.side);
    document["filled_qty"]   = fill_notification.filled_quantity;
    document["filled_price"] = fill_notification.filled_price;
    document["commission"]   = commission;
    document["tax"]          = tax;
    document["avg_price"]    = average_price;
    document["net_qty"]      = net_qty;
    document["realized_pnl"] = realized_pnl;
    document["account"]      = account_no_;
    document["strategy"]     = strategy_id;
    document["regime"]       = regime_label_;
    enqueue("FILL", document.dump());
}

#endif // HAS_ZMQ
