#pragma once
#include "api/KisClient.h"
#include "core/Types.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#ifdef ERROR
#undef ERROR
#endif
#endif

// ─────────────────────────────────────────────────────────────────────────────
// KisWebSocket  —  국내 + 미국 실시간 WebSocket
//
//  국내  H0STASP0 → OrderBook  /  H0STCNT0 → TradeData(KR)
//  미국  HDFSCNT0 → TradeData(US)  (KIS는 미국 호가 미제공)
//
// 사용법:
//   KisWebSocket ws(cfg);
//   ws.set_callbacks(on_ob, on_trade);
//   ws.connect(specs);   // WatchSpec 리스트로 KR/US 혼합 구독
//   ws.disconnect();
// ─────────────────────────────────────────────────────────────────────────────
class KisWebSocket
{
public:
    using OrderBookCb = std::function<void(const OrderBook&)>;
    using TradeCb = std::function<void(const TradeData&)>;

    explicit KisWebSocket(const KisConfig& cfg);
    ~KisWebSocket();

    void set_callbacks(OrderBookCb on_ob, TradeCb on_trade);
    bool connect(const std::vector<WatchSpec>& specs);
    void disconnect();

    bool is_connected() const
    {
        return connected_.load();
    }

private:
    bool get_approval_key();
    void send_text(const std::string& msg);
    void send_subscribe(const std::string& tr_id, const std::string& tr_key);
    void recv_loop();
    void parse_message(const std::string& msg);
    void parse_orderbook(const std::vector<std::string>& f);
    void parse_kr_trade(const std::vector<std::string>& f);
    void parse_us_trade(const std::vector<std::string>& f);

    static std::vector<std::string> split_str(const std::string& s, char delim);
    static std::wstring to_wide(const std::string& s);
    static std::string http_post_json(const std::string& url, const std::string& body);

    KisConfig cfg_;
    std::string approval_key_;
    std::vector<WatchSpec> specs_;

    std::atomic<bool> connected_{false};
    std::thread recv_thread_;
    std::mutex send_mtx_;

    OrderBookCb on_orderbook_;
    TradeCb on_trade_;

#ifdef _WIN32
    HINTERNET hSession_ = nullptr;
    HINTERNET hConnect_ = nullptr;
    HINTERNET hWebSocket_ = nullptr;
#else
    int sock_fd_ = -1;
#endif
};
