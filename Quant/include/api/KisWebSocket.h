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
//  국내      H0STASP0 → OrderBook  /  H0STCNT0 → TradeData(KR)
//  국내선물  H0IFASP0 → OrderBook  /  H0IFCNT0 → TradeData  (WatchSpec.is_future=true)
//  미국      HDFSCNT0 → TradeData(US)  (KIS는 미국 호가 미제공)
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
    using TradeCb     = std::function<void(const TradeData&)>;
    using FillCb      = std::function<void(const FillNotification&)>;

    explicit KisWebSocket(const KisConfig& cfg);
    ~KisWebSocket();

    void set_callbacks(OrderBookCb on_ob, TradeCb on_trade);
    void set_fill_callback(FillCb on_fill);
    bool connect(const std::vector<WatchSpec>& specs);
    void disconnect();

    bool is_connected() const
    {
        return connected_.load();
    }

    // 임계 시간(초) 이상 메시지 미수신 시 true — 장 중 호출할 것
    bool is_stale(int threshold_sec = 30) const
    {
        auto now_ns  = std::chrono::steady_clock::now().time_since_epoch().count();
        auto last_ns = last_message_ns_.load(std::memory_order_relaxed);
        return (now_ns - last_ns) / 1'000'000'000LL >= threshold_sec;
    }

private:
    // 메시지 수신 시 호출 — parse_message 진입부에서 갱신
    void on_message_received()
    {
        last_message_ns_.store(
            std::chrono::steady_clock::now().time_since_epoch().count(),
            std::memory_order_relaxed);
    }
    bool get_approval_key();
    void send_text(const std::string& msg);
    void send_subscribe(const std::string& tr_id, const std::string& tr_key);
    // specs_ 전체를 순회하며 채널을 구독한다(최초 연결·재연결 공통). 거래ID(tr_id) 하드코딩
    // 블록이 네 곳(플랫폼×최초/재연결)에 중복돼 있던 것을 한 곳으로 모은다.
    // 재연결 시 선물 채널이 빠지는 불일치를 막는다.
    void subscribe_all();
    void recv_loop();
    void parse_message(const std::string& msg);
    void parse_orderbook(const std::vector<std::string>& f);
    void parse_kr_trade(const std::vector<std::string>& f);
    void parse_us_trade(const std::vector<std::string>& f);
    void parse_fut_trade(const std::vector<std::string>& f);     // H0IFCNT0 선물 체결
    void parse_fut_orderbook(const std::vector<std::string>& f); // H0IFASP0 선물 호가
    void parse_fill_notification(const std::vector<std::string>& f);

    // 체결통보(H0STCNI) 복호화 — base64 + AES-256-CBC (플랫폼별 구현)
    static std::string base64_decode(const std::string& in);
    static std::string aes_cbc_decrypt(const std::string& cipher,
                                       const std::string& key,
                                       const std::string& iv);

    static std::vector<std::string> split_str(const std::string& s, char delim);
    static std::wstring to_wide(const std::string& s);
    static std::string http_post_json(const std::string& url, const std::string& body);

    KisConfig cfg_;
    std::string approval_key_; // KIS 실시간 WS 접속 승인키 (REST로 발급, 세션 내 재사용)
    std::string aes_key_; // 체결통보 복호화 키 (구독 응답에서 획득)
    std::string aes_iv_;  // 체결통보 복호화 IV
    std::vector<WatchSpec> specs_;

    std::atomic<bool>    connected_{false};
    // 나노초 단위 — std::atomic<time_point>는 이식성 문제로 int64_t 사용
    std::atomic<int64_t> last_message_ns_{
        std::chrono::steady_clock::now().time_since_epoch().count()
    };
    std::thread recv_thread_;
    std::mutex send_mtx_;

    OrderBookCb on_orderbook_;
    TradeCb     on_trade_;
    FillCb      on_fill_;

#ifdef _WIN32
    HINTERNET hSession_ = nullptr;
    HINTERNET hConnect_ = nullptr;
    HINTERNET hWebSocket_ = nullptr;
#else
    int sock_fd_ = -1;
#endif
};
