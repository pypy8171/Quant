#include "api/KisWebSocket.h"

bool KisWebSocket::is_stale(int threshold_sec) const
{
    auto now_ns = std::chrono::steady_clock::now().time_since_epoch().count();
    auto last_ns = last_message_ns_.load(std::memory_order_relaxed);
    return (now_ns - last_ns) / 1'000'000'000LL >= threshold_sec;
}

void KisWebSocket::on_message_received()
{
    last_message_ns_.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_relaxed);
}
