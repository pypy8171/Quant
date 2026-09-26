// ZmqBridge::publish_trade가 부르는 쪽(수신 스레드)에 얹는 비용 — 원칙 3(수신 스레드는 얇게)의 전후 측정.
//  브리지는 실제로 기동하되 구독자는 없다(PUB는 구독자가 없으면 소켓에서 조용히 버린다). 재는 것은
//  호출 스레드가 publish_trade에 머무는 시간뿐이고, 송신 스레드가 뒤에서 큐를 비우는 동안의 경합도 포함된다.
//
//  실측 2026-09-20, MSVC /O2, 200만 틱:
//    이전(nlohmann json dump + 뮤텍스 std::queue<string>) : ~1,000 ns/틱
//    이후(MpscQueue<TradeEnvelope> memcpy, JSON은 송신 스레드) :   ~26 ns/틱
//  드롭 수는 이 벤치의 산물이다 — 200만 틱을 수십 ms에 밀어 넣으니 링(8,192칸 × 초당 100바퀴)이 넘친다.
//  같은 실행이 와이어 포맷 검사도 한다 — format_trade 결과가 예전 nlohmann dump()와 글자 단위로 같아야 한다
//  (건 단위 형식은 그대로다. 묶어 보낼 때는 그 문자열을 JSON 배열 원소로 넣는다). 다르면 1을 돌려 ctest가 잡는다.
//
//  세 번째 측정은 발행 스레드 쪽 천장이다 — 부르는 쪽이 아무리 얇아도 비우는 쪽이 초당 몇 건인지가
//  실제 상한이라 거기서 버림이 났다(docs/reports/stresstest/OVERVIEW.md 10절 (ㄱ)). 건마다 한 프레임을
//  보낼 때와 500건을 한 프레임에 실을 때를 같은 자로 잰다. [why D-071 원칙 7 · D-139]
#include "ipc/ZmqBridge.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <nlohmann/json.hpp>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include <zmq.hpp>

namespace
{

constexpr size_t kTickerCount = 2700;
constexpr size_t kTickCount   = 2'000'000;

// 예전 publish_trade가 만들던 문자열 그대로(계좌 필드만 뒤에 얹었다).
std::string legacy_payload(const ZmqBridge::TradeEnvelope& envelope, const std::string& account)
{
    nlohmann::json document;
    document["account"]   = account;
    document["ts"]        = envelope.ts_ms;
    document["ticker"]    = envelope.trade.ticker;
    document["price"]     = envelope.trade.price;
    document["volume"]    = envelope.trade.quantity;
    document["direction"] = envelope.trade.direction;
    document["market"]    = (envelope.trade.market == Market::US ? "US" : "KR");
    return document.dump();
}

bool wire_format_matches()
{
    const double prices[] = {71500.0, 12.345, 0.1, 1e-7, 123456789.5, 2.5e21, 0.0, 1234.56789012345};
    bool         all_same = true;
    std::string  actual;

    for (size_t index = 0; index < sizeof(prices) / sizeof(prices[0]); ++index)
    {
        ZmqBridge::TradeEnvelope envelope;
        envelope.ts_ms           = 1758340000000 + static_cast<int64_t>(index);
        envelope.trade.ticker    = std::string_view(index % 2 == 0 ? "005930" : "0N123A");
        envelope.trade.price     = prices[index];
        envelope.trade.quantity  = static_cast<int64_t>(index) * 1000;
        envelope.trade.direction = index % 2 == 0 ? 1 : 5;
        envelope.trade.market    = index == 3 ? Market::US : Market::KR;
        const std::string account = index % 3 == 0 ? "" : "acct-01";
        ZmqBridge::format_trade(envelope, account, actual);
        const std::string expected = legacy_payload(envelope, account);

        if (actual != expected)
        {
            std::printf("wire format differs\n  new: %s\n  old: %s\n", actual.c_str(), expected.c_str());
            all_same = false;
        }
    }

    return all_same;
}

// 발행 스레드가 하는 일만 그대로 옮긴 측정 — 체결을 문자열로 만들고 PUB 소켓으로 내보낸다.
//  받는 쪽이 없으면 PUB 은 소켓에서 바로 버려 TCP 쓰기 비용이 빠지므로, 같은 프로세스에 SUB 을 붙여
//  실제로 흘려 보낸다. 재는 것은 보내는 쪽 시간뿐이다.
double measure_drain(const char* label, const std::vector<TradeData>& ticks, int port, size_t batch_max)
{
    zmq::context_t context{1};
    zmq::socket_t  publish_socket{context, zmq::socket_type::pub};
    publish_socket.set(zmq::sockopt::sndhwm, 200000);
    publish_socket.bind("tcp://127.0.0.1:" + std::to_string(port));

    zmq::socket_t subscribe_socket{context, zmq::socket_type::sub};
    subscribe_socket.set(zmq::sockopt::rcvhwm, 200000);
    subscribe_socket.set(zmq::sockopt::subscribe, "");
    subscribe_socket.connect("tcp://127.0.0.1:" + std::to_string(port));
    // SUB 이 붙기 전에 보낸 것은 사라진다 — 연결이 설 때까지 기다린다.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    std::atomic<bool>     reading{true};
    std::atomic<uint64_t> frames_read{0};
    std::thread           reader(
        [&]
        {
            subscribe_socket.set(zmq::sockopt::rcvtimeo, 100);

            while (reading.load())
            {
                zmq::message_t frame;

                if (subscribe_socket.recv(frame, zmq::recv_flags::none))
                {
                    ++frames_read;
                }
            }
        });

    std::string payload;
    std::string batch;
    batch.reserve(batch_max * 128);
    size_t     batch_count = 0;
    uint64_t   frames_sent = 0;

    const auto send_frames = [&](std::string_view body)
    {
        zmq::message_t topic_frame(5);
        zmq::message_t payload_frame(body.size());
        std::memcpy(topic_frame.data(), "TRADE", 5);
        std::memcpy(payload_frame.data(), body.data(), body.size());

        if (publish_socket.send(topic_frame, zmq::send_flags::sndmore | zmq::send_flags::dontwait))
        {
            (void)publish_socket.send(payload_frame, zmq::send_flags::dontwait);
        }

        ++frames_sent;
    };

    const auto start = std::chrono::high_resolution_clock::now();

    for (const auto& trade : ticks)
    {
        ZmqBridge::TradeEnvelope envelope;
        envelope.ts_ms = 1758340000000;
        envelope.trade = trade;
        ZmqBridge::format_trade(envelope, "00000000", payload);

        if (batch_max <= 1)
        {
            send_frames(payload);
            continue;
        }

        batch.push_back(batch_count == 0 ? '[' : ',');
        batch.append(payload);

        if (++batch_count >= batch_max)
        {
            batch.push_back(']');
            send_frames(batch);
            batch.clear();
            batch_count = 0;
        }
    }

    if (batch_count > 0)
    {
        batch.push_back(']');
        send_frames(batch);
    }

    const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::high_resolution_clock::now() - start)
                                .count();
    reading.store(false);
    reader.join();

    const double per_trade_ns = static_cast<double>(elapsed_ns) / static_cast<double>(ticks.size());
    const double per_second   = per_trade_ns > 0.0 ? 1e9 / per_trade_ns : 0.0;
    std::printf("%s: %.0f ns/trade → 초당 %.0f건 (프레임 %llu, 받은 프레임 %llu)\n", label, per_trade_ns,
                per_second, static_cast<unsigned long long>(frames_sent),
                static_cast<unsigned long long>(frames_read.load()));
    return per_second;
}

} // namespace

int main()
{
    if (!wire_format_matches())
    {
        return 1;
    }

    std::puts("wire format: same as legacy dump()");
    std::vector<TradeData> ticks;
    ticks.reserve(kTickCount);

    std::mt19937                          random_engine(42);
    std::uniform_int_distribution<size_t> pick(0, kTickerCount - 1);

    for (size_t iteration = 0; iteration < kTickCount; ++iteration)
    {
        const size_t index = pick(random_engine);
        char         buffer[8];
        std::snprintf(buffer, sizeof(buffer), "%06zu", index * 37 % 1'000'000);

        TradeData trade;
        trade.ticker    = std::string_view(buffer);
        trade.symbol_id = static_cast<symbol::SymbolId>(index + 1);
        trade.price     = 1000.0 + static_cast<double>(index) * 0.5;
        trade.quantity  = static_cast<int64_t>(index % 97 + 1);
        trade.direction = (index % 2 == 0) ? 1 : 5;
        trade.hhmmss    = 93000;
        ticks.push_back(trade);
    }

    // 부르는 쪽 비용을 잰다. 다리를 두 벌 돌리는 이유는 발행 전용 다리(REP 없음)가 송신 스레드를
    //  조건변수로 깨우기 때문이다 — 그 깨우기가 수신 스레드에 얼마를 얹는지 같은 자로 봐야 한다.
    //  [why D-071 원칙 7]
    const auto measure = [&ticks](const char* label, int pub_port, int rep_port)
    {
        ZmqBridge bridge(pub_port, rep_port);

        if (!bridge.start())
        {
            std::printf("%s: start 실패\n", label);
            return false;
        }

        const auto start = std::chrono::high_resolution_clock::now();

        for (const auto& trade : ticks)
        {
            bridge.publish_trade(trade);
        }

        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::high_resolution_clock::now() - start)
                                 .count();
        std::printf("%s: publish_trade caller cost %.1f ns/tick over %zu ticks (drops %llu)\n", label,
                    static_cast<double>(elapsed) / static_cast<double>(kTickCount), kTickCount,
                    static_cast<unsigned long long>(bridge.drop_count()));
        bridge.stop();
        return true;
    };

    // 운영 포트(5555/5556)와 겹치지 않게 15555 대를 쓴다.
    if (!measure("PUB+REP", 15555, 15556))
    {
        return 1;
    }

    // rep_port 0 = 발행 전용. 시세·전략 프로세스가 여는 모습이다 — REP 소켓을 아예 안 만들고도
    //  start → publish → stop 이 깨끗한지 본다. [why D-129]
    if (!measure("PUB only", 15557, 0))
    {
        return 1;
    }

    // 발행 스레드 천장 — 전후 측정. 200만 건은 한 건씩 보내면 몇 초가 걸려 벤치가 길어지므로 20만 건만 쓴다.
    const std::vector<TradeData> drain_ticks(ticks.begin(), ticks.begin() + 200'000);
    const double                 one_by_one = measure_drain("체결 한 건 한 프레임", drain_ticks, 15558, 1);
    const double                 batched    = measure_drain("체결 500건 한 프레임", drain_ticks, 15559, 500);

    if (one_by_one > 0.0)
    {
        std::printf("발행 천장 %.1f배\n", batched / one_by_one);
    }

    return 0;
}
