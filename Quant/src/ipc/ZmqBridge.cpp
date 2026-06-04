#ifdef HAS_ZMQ
#include "ipc/ZmqBridge.h"
#include "utils/Logger.h"

#include <chrono>
#include <nlohmann/json.hpp>
#include <sstream>

using json = nlohmann::json;
using namespace std::chrono;
using namespace std::chrono_literals;

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
        return true;
    running_.store(true);
    zmq_thread_ = std::thread(&ZmqBridge::thread_fn, this);
    LOG_INFO("[ZMQ] 브리지 시작 — PUB:" + std::to_string(pub_port_) + " REP:" + std::to_string(rep_port_));
    return true;
}

void ZmqBridge::stop()
{
    if (!running_.load())
        return;
    running_.store(false);
    if (zmq_thread_.joinable())
        zmq_thread_.join();
    LOG_INFO("[ZMQ] 브리지 종료");
}

// ─── 스레드 본체 (ZMQ 소켓은 이 스레드에서만 사용) ─────────────────────────
void ZmqBridge::thread_fn()
{
    zmq::context_t ctx{1};
    zmq::socket_t pub{ctx, zmq::socket_type::pub};
    zmq::socket_t rep{ctx, zmq::socket_type::rep};

    try
    {
        pub.bind("tcp://*:" + std::to_string(pub_port_));
        rep.bind("tcp://*:" + std::to_string(rep_port_));
    }
    catch (const zmq::error_t& e)
    {
        LOG_ERROR(std::string("[ZMQ] 소켓 bind 실패: ") + e.what());
        running_.store(false);
        return;
    }

    // REP 폴링 아이템 (논블로킹)
    zmq::pollitem_t items[] = {{rep, 0, ZMQ_POLLIN, 0}};

    while (running_.load())
    {
        // 1. 송신 큐 소진
        {
            std::lock_guard<std::mutex> lk(queue_mtx_);
            while (!send_queue_.empty())
            {
                auto& m = send_queue_.front();
                // 멀티파트: frame1=topic, frame2=payload
                zmq::message_t t_frame(m.topic.size());
                zmq::message_t p_frame(m.payload.size());
                std::memcpy(t_frame.data(), m.topic.data(), m.topic.size());
                std::memcpy(p_frame.data(), m.payload.data(), m.payload.size());
                try
                {
                    pub.send(t_frame, zmq::send_flags::sndmore);
                    pub.send(p_frame, zmq::send_flags::dontwait);
                }
                catch (const zmq::error_t& e)
                {
                    LOG_WARN(std::string("[ZMQ] publish 실패 topic=") + m.topic + " : " + e.what());
                }
                send_queue_.pop();
            }
        }

        // 2. 명령 수신 (REP, 10ms 타임아웃)
        try
        {
            zmq::poll(items, 1, 10ms);
            if (items[0].revents & ZMQ_POLLIN)
            {
                zmq::message_t req;
                rep.recv(req, zmq::recv_flags::none);
                std::string cmd(static_cast<char*>(req.data()), req.size());

                std::string reply_str = "OK";
                if (cmd_handler_)
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
        catch (const zmq::error_t& e)
        {
            if (e.num() != ETERM)
                LOG_WARN(std::string("[ZMQ] poll 오류: ") + e.what());
        }
    }

    pub.close();
    rep.close();
    ctx.close();
}

// ─── 메시지 enqueue (스레드-안전) ───────────────────────────────────────────
void ZmqBridge::enqueue(std::string topic, std::string payload)
{
    std::lock_guard<std::mutex> lk(queue_mtx_);
    // (C8) 토픽별 drop 차등 — FILL/ORDER/SIGNAL은 원장 정합성에 직결되므로
    // 고빈도 TRADE/HEALTH(1000)보다 훨씬 큰 하드캡(100000)까지 보존한다.
    const bool critical = (topic == "FILL" || topic == "ORDER" || topic == "SIGNAL");
    const size_t cap = critical ? 100000 : 1000;
    if (send_queue_.size() >= cap)
    {
        ++drop_count_;
        if (critical)
            LOG_ERROR("[ZMQ] 치명적 메시지 drop! topic=" + topic +
                      " queue=" + std::to_string(send_queue_.size()) +
                      " (구독자 다운 의심) — 원장 불일치 위험");
        return;
    }
    send_queue_.push({std::move(topic), std::move(payload)});
}

// ─── 이벤트별 publish 헬퍼 ─────────────────────────────────────────────────
static int64_t now_ms()
{
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

void ZmqBridge::publish_trade(const TradeData& td)
{
    json j;
    j["ts"] = now_ms();
    j["ticker"] = td.ticker;
    j["price"] = td.price;
    j["volume"] = td.quantity;
    j["direction"] = td.direction; // 1=매수, 5=매도
    j["market"] = (td.market == Market::US ? "US" : "KR");
    enqueue("TRADE", j.dump());
}

void ZmqBridge::publish_signal(const OrderSignal& sig)
{
    json j;
    j["ts"] = now_ms();
    j["strategy"] = sig.strategy_id;
    j["ticker"] = sig.ticker;
    j["side"] = (sig.side == OrderSide::BUY ? "BUY" : "SELL");
    j["qty"] = sig.quantity;
    j["price"] = sig.price;
    j["market"] = (sig.market == Market::US ? "US" : "KR");
    enqueue("SIGNAL", j.dump());
}

void ZmqBridge::publish_order(const OrderSignal& sig, bool ok)
{
    json j;
    j["ts"] = now_ms();
    j["ticker"] = sig.ticker;
    j["side"] = (sig.side == OrderSide::BUY ? "BUY" : "SELL");
    j["qty"] = sig.quantity;
    j["price"] = sig.price;
    j["ok"] = ok;
    j["market"] = (sig.market == Market::US ? "US" : "KR");
    enqueue("ORDER", j.dump());
}

void ZmqBridge::publish_health(uint64_t data_cnt, uint64_t sig_cnt, uint64_t ord_cnt)
{
    json j;
    j["ts"]    = now_ms();
    j["data"]  = data_cnt;
    j["signal"] = sig_cnt;
    j["order"] = ord_cnt;
    j["drop"]  = drop_count_.load();
    enqueue("HEALTH", j.dump());
}

void ZmqBridge::publish_fill(const FillNotification& fn, double commission,
                              double tax, double avg_price, int net_qty,
                              double realized_pnl)
{
    json j;
    j["ts"]           = now_ms();
    j["odno"]         = fn.odno;
    j["ticker"]       = fn.ticker;
    j["side"]         = (fn.side == OrderSide::BUY ? "BUY" : "SELL");
    j["filled_qty"]   = fn.filled_qty;
    j["filled_price"] = fn.filled_price;
    j["commission"]   = commission;
    j["tax"]          = tax;
    j["avg_price"]    = avg_price;
    j["net_qty"]      = net_qty;
    j["realized_pnl"] = realized_pnl;
    enqueue("FILL", j.dump());
}

#endif // HAS_ZMQ
