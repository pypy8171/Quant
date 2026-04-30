#pragma once
#include "core/Types.h"
#include "api/KisClient.h"
#include <functional>
#include <atomic>
#include <thread>
#include <string>
#include <vector>
#include <mutex>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <winhttp.h>
// windows.h가 ERROR를 매크로로 정의 → LogLevel::ERROR 충돌 방지
#  ifdef ERROR
#    undef ERROR
#  endif
#endif

// ─────────────────────────────────────────────────────────────────────────────
// KisWebSocket  —  KIS 실시간 WebSocket 클라이언트
//   H0STASP0 : 실시간 호가 (위아래 5단계)
//   H0STCNT0 : 실시간 체결
//
// 사용법:
//   KisWebSocket ws(cfg);
//   ws.set_callbacks(on_orderbook, on_trade);
//   ws.connect({"005930", "000660", ...});
//   ...
//   ws.disconnect();
// ─────────────────────────────────────────────────────────────────────────────
class KisWebSocket {
public:
    using OrderBookCb = std::function<void(const OrderBook&)>;
    using TradeCb     = std::function<void(const TradeData&)>;

    explicit KisWebSocket(const KisConfig& cfg);
    ~KisWebSocket();

    void set_callbacks(OrderBookCb on_ob, TradeCb on_trade);
    bool connect(const std::vector<std::string>& tickers);
    void disconnect();

    bool is_connected() const { return connected_.load(); }

private:
    bool get_approval_key();
    void send_text(const std::string& msg);
    void send_subscribe(const std::string& tr_id, const std::string& ticker);
    void recv_loop();
    void parse_message(const std::string& msg);
    void parse_orderbook(const std::vector<std::string>& f);
    void parse_trade(const std::vector<std::string>& f);

    static std::vector<std::string> split_str(const std::string& s, char delim);
    static std::wstring             to_wide(const std::string& s);
    static std::string              http_post_json(const std::string& url,
                                                   const std::string& body);

    KisConfig                cfg_;
    std::string              approval_key_;
    std::vector<std::string> tickers_;

    std::atomic<bool>  connected_{false};
    std::thread        recv_thread_;
    std::mutex         send_mtx_;

    OrderBookCb on_orderbook_;
    TradeCb     on_trade_;

#ifdef _WIN32
    HINTERNET hSession_   = nullptr;
    HINTERNET hConnect_   = nullptr;
    HINTERNET hWebSocket_ = nullptr;
#else
    int sock_fd_ = -1;
#endif
};
