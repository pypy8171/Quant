#pragma once
#include "api/KisClient.h"
#include "api/KisWsDecode.h"
#include "core/Types.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

class WsSocket; // 플랫폼 소켓(Quant/src/api/WsSocket.h). 이 헤더는 플랫폼 헤더를 끌어오지 않는다. [why D-049]

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
//
// 스레드: recv_loop 스레드가 소켓을 소유하고 재연결·백오프·구독 복원을 한다(플랫폼 공통 한 벌).
//   소켓 열기·닫기·프레임 송수신은 WsSocket 구현(WinHTTP / POSIX)이 맡는다. [why D-049]
// ─────────────────────────────────────────────────────────────────────────────
class KisWebSocket
{
public:
    using OrderBookCb = std::function<void(const OrderBook&)>;
    using TradeCb     = std::function<void(const TradeData&)>;
    using FillCb      = std::function<void(const FillNotification&)>;

    explicit KisWebSocket(const KisConfig& cfg);
    ~KisWebSocket();
    // 스레드·뮤텍스를 소유한다 — 복사는 원본과 사본이 같은 자원을 두 번 닫는 길이라 막는다.
    KisWebSocket(const KisWebSocket&)            = delete;
    KisWebSocket& operator=(const KisWebSocket&) = delete;

    void set_callbacks(OrderBookCb on_ob, TradeCb on_trade);
    void set_fill_callback(FillCb on_fill);
    bool connect(const std::vector<WatchSpec>& specs);
    void disconnect();

    // 연결을 유지한 채 종목 하나를 더 구독한다(장중 유니버스 재스캔으로 늘어난 종목용).
    //  connect()는 최초 1회만 도는데, 재스캔으로 등록된 전략의 WatchSpec은 그때 목록에 없었다.
    //  이 함수로 specs_에 넣어 두면 다음 재연결에서도 함께 살아난다. 이미 있는 종목이면
    //  아무것도 하지 않는다(중복 구독은 등록 한도만 갉아먹는다).
    //  연결 전이면 목록에만 넣고 실제 구독은 connect()가 한다.
    //  data_thread에서 호출하고 specs_는 연결 스레드가 읽으므로 specs_mtx_로 보호한다.
    // 반환: 구독 프레임을 실제로 보냈으면 true.
    bool subscribe_incremental(const WatchSpec& spec);

    // 다건 프레임 분리는 kis_ws::split_records(api/KisWsDecode.h). 여기 이름은 테스트·호출부 호환용.
    static kis_ws::Records split_records(kis_ws::Fields fields, int count, size_t min_fields) noexcept
    {
        return kis_ws::split_records(fields, count, min_fields);
    }

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
    // spec 하나의 채널을 구독한다(현·선물·미국 분기 한 곳). subscribe_all과 증분 구독이 공유한다.
    void subscribe_spec(const WatchSpec& spec);
    // spec 하나가 소비하는 구독 슬롯 수(호가+체결이면 2, trade_only면 1).
    static int spec_channel_count(const WatchSpec& spec);
    // KIS 세션 구독 상한. 문서상 41건이며, 넘기면 이후 구독이 rt=1 MAX SUBSCRIBE OVER로 잘린다.
    static constexpr int kMaxWsSubs = 40;
    // 현재 세션이 사용 중인 구독 슬롯 수(subscribe_all이 리셋, 증분 구독이 증가).
    std::atomic<int> sub_used_{0};
    void recv_loop();
    void parse_message(const std::string& msg);
    // 레코드 한 건을 tr_id에 맞는 파서로 보낸다(단건·다건 프레임이 공유).
    void dispatch_record(std::string_view tr_id, kis_ws::Fields f);
    // 채널별 파서가 요구하는 최소 필드 수(각 parse_*의 가드와 같은 값). 모르는 채널은 0.
    static size_t min_fields_for(std::string_view tr_id) noexcept;
    // 프레임 분해 뷰 벡터. 수신 스레드만 만지고 용량을 재사용해 정상 상태에서 할당이 없다. [why D-042]
    std::vector<std::string_view> parts_;
    std::vector<std::string_view> fields_;
    // 다건 프레임을 자르지 못해 1건만 처리했을 때의 경고 횟수. 수신 스레드만 만진다.
    int multi_rec_warned_ = 0;
    void parse_orderbook(kis_ws::Fields f);
    void parse_kr_trade(kis_ws::Fields f);
    void parse_us_trade(kis_ws::Fields f);
    void parse_fut_trade(kis_ws::Fields f);     // H0IFCNT0 선물 체결
    void parse_fut_orderbook(kis_ws::Fields f); // H0IFASP0 선물 호가
    void parse_fill_notification(kis_ws::Fields f);

    // 체결통보(H0STCNI) 복호화 — base64는 여기, AES-256-CBC는 플랫폼별(ws_platform::aes_cbc_decrypt)
    static std::string base64_decode(const std::string& in);

    KisConfig cfg_;
    std::string approval_key_; // KIS 실시간 WS 접속 승인키 (REST로 발급, 세션 내 재사용)
    std::string aes_key_; // 체결통보 복호화 키 (구독 응답에서 획득)
    std::string aes_iv_;  // 체결통보 복호화 IV
    std::vector<WatchSpec> specs_;
    // specs_ 보호 — 연결 스레드가 읽고(subscribe_all) data_thread가 쓴다(subscribe_incremental).
    mutable std::mutex specs_mtx_;

    std::atomic<bool>    connected_{false};
    // 나노초 단위 — std::atomic<time_point>는 이식성 문제로 int64_t 사용
    std::atomic<int64_t> last_message_ns_{
        std::chrono::steady_clock::now().time_since_epoch().count()
    };
    std::thread recv_thread_;
    // sock_ 교체·close와 send_text를 갈라 놓는다. recv_message는 락 없이 블로킹한다(close가 깨운다).
    std::mutex send_mtx_;
    // [inv] recv_loop 스레드만 바꾼다. connect()는 스레드를 띄우기 전, disconnect()는 join한 뒤에 만진다.
    std::unique_ptr<WsSocket> sock_;

    OrderBookCb on_orderbook_;
    TradeCb     on_trade_;
    FillCb      on_fill_;
};
