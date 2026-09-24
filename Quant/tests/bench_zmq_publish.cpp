// ZmqBridge::publish_trade가 부르는 쪽(수신 스레드)에 얹는 비용 — 원칙 3(수신 스레드는 얇게)의 전후 측정.
//  브리지는 실제로 기동하되 구독자는 없다(PUB는 구독자가 없으면 소켓에서 조용히 버린다). 재는 것은
//  호출 스레드가 publish_trade에 머무는 시간뿐이고, 송신 스레드가 뒤에서 큐를 비우는 동안의 경합도 포함된다.
//
//  실측 2026-09-20, MSVC /O2, 200만 틱:
//    이전(nlohmann json dump + 뮤텍스 std::queue<string>) : ~1,000 ns/틱
//    이후(MpscQueue<TradeEnvelope> memcpy, JSON은 송신 스레드) :   ~26 ns/틱
//  드롭 수는 이 벤치의 산물이다 — 200만 틱을 수십 ms에 밀어 넣으니 링(8,192칸 × 초당 100바퀴)이 넘친다.
//  같은 실행이 와이어 포맷 검사도 한다 — format_trade 결과가 예전 nlohmann dump()와 글자 단위로 같아야 한다
//  (구독자 PYQuant/ipc/subscriber.py·DB 적재는 손대지 않았다). 다르면 1을 돌려 ctest가 잡는다.
#include "ipc/ZmqBridge.h"

#include <chrono>
#include <cstdio>
#include <nlohmann/json.hpp>
#include <random>
#include <string>
#include <vector>

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

    return 0;
}
