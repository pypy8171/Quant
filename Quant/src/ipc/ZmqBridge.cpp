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
                catch (...)
                {
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
    // 큐 과부하 방지: 최대 1000개
    if (send_queue_.size() < 1000)
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
    j["ts"] = now_ms();
    j["data"] = data_cnt;
    j["signal"] = sig_cnt;
    j["order"] = ord_cnt;
    enqueue("HEALTH", j.dump());
}

#endif // HAS_ZMQ
