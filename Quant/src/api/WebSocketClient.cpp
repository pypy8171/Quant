// api/WebSocketClient.cpp — KIS 실시간 WebSocket 클라이언트의 플랫폼 독립 부분.
//  연결·재연결·백오프·구독 복원이 한 벌이고, 소켓 자체는 WsSocket(WsSocketWin/WsSocketPosix)이 맡는다.
//  수신 프레임 파싱은 Quant/src/api/KisWebSocketParse.cpp.
//  스레드: recv_loop 전용 스레드가 socket_를 소유하고 바꾼다. data_thread는 send_text(send_mutex_)만 지난다. [why D-049]
#include "api/KisEndpoints.h"
#include "api/KisWebSocket.h"
#include "WsSocket.h"
#include "core/WakeGate.h"
#include "utils/Logger.h"
#include "utils/ThreadName.h"
#include <algorithm>
#include <chrono>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string_view>
#include <thread>

using json = nlohmann::json;

// ═══════════════════════════════════════════════════════════════════════════
//  연결 · 수신 스레드 · 재연결
// ═══════════════════════════════════════════════════════════════════════════

bool KisWebSocket::connect(const std::vector<WatchSpec>& specifications)
{
    {
        std::lock_guard<std::mutex> lock(specifications_mutex_);
        specifications_ = specifications;
    }

    if (!get_approval_key())
    {
        return false;
    }

    const int         port = kis_endpoints::websocket_port(config_.is_paper);
    const std::string host(kis_endpoints::kWebSocketHost);
    auto              socket = websocket_platform::make_socket();

    if (!socket->open(host, port))
    {
        LOG_ERROR("[WS] WebSocket 연결 실패: " + host + ":" + std::to_string(port));
        return false;
    }

    // 구 수신 스레드가 자체 종료(connected_=false)로 join되지 않은 채 남아 있을 수 있다.
    // joinable 상태에서 재대입하면 std::terminate → 재대입 전 반드시 reap. socket_도 그 스레드가 만지므로 그 뒤에 바꾼다.
    if (recv_thread_.joinable())
    {
        recv_thread_.join();
    }

    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        socket_ = std::move(socket);
    }

    ++session_generation_;
    LOG_INFO(std::string("[WS] WebSocket 연결 성공 (") + (config_.is_paper ? "모의투자" : "실거래") + ")");
    connected_.store(true);

    subscribe_all();

    recv_thread_ = std::jthread([this](std::stop_token stop_token)
    {
        recv_loop(stop_token);
    });
    return true;
}

void KisWebSocket::send_text(const std::string& message)
{
    std::lock_guard<std::mutex> lock(send_mutex_);

    if (socket_)
    {
        socket_->send_text(message);
    }
}

// [inv] socket_는 이 스레드가 바꾼다. connect()는 스레드를 띄우기 전, disconnect()는 join한 뒤에만 만지므로
//       여기서 락 없이 읽어도 된다. data_thread의 send_text와는 교체·close를 send_mutex_ 아래서 해서 갈린다.
void KisWebSocket::recv_loop(std::stop_token stop_token)
{
    thread_name::set_current("WsRecv");
    LOG_INFO("[WS] 수신 스레드 시작");
    std::string message;
    int retry_sec = 1;

    while (connected_.load())
    {
        // 백오프는 "메시지 수신"이 아니라 "연결 유지 시간"으로 판단한다. 재연결 직후
        // 구독응답/에러프레임(ALREADY IN USE)이 곧바로 수신되면 메시지 기반 리셋은
        // 백오프를 매번 1초로 되돌려 폭주한다. 연결이 얼마나 살아있었는지로 구분한다.
        const auto connection_start = std::chrono::steady_clock::now();

        while (connected_.load() && socket_ && socket_->recv_message(message))
        {
            parse_message(message);
        }

        if (!connected_.load())
        {
            break;
        }

        if (socket_)
        {
            LOG_WARN("[WS] 수신 오류 (" + socket_->last_error() + ") — 재연결 준비");
        }

        // 충분히 오래(≥5s) 유지된 연결이 끊긴 것이면 일시 장애로 보고 백오프 리셋 후
        // 빠르게 재시도. 즉시 죽는 연결(=서버가 appkey 세션 미해제/off-hours abort)은
        // 지수적으로 물러서서 서버가 직전 세션을 놓을 시간을 준다. 직전 재연결이 실패해
        // socket_가 비어 있으면 connection_start가 방금이라 리셋되지 않는다 — 의도한 동작.
        if (std::chrono::steady_clock::now() - connection_start > std::chrono::seconds(5))
        {
            retry_sec = 1;
        }

        // 기존 소켓은 자기 전에 닫는다 — close 프레임이 먼저 가야 KIS가 이 approval_key 세션을
        // 놓고, 백오프로 자는 시간이 그 해제 대기가 된다. 핸들만 닫으면 세션이 남아
        // 다음 접속이 "ALREADY IN USE appkey"(rt=9)로 거부된다.
        {
            std::lock_guard<std::mutex> lock(send_mutex_);

            if (socket_)
            {
                socket_->close();
            }

            socket_.reset();
        }

        // ── 지수 백오프 재연결 ─────────────────────────────────────────
        LOG_WARN("[WS] " + std::to_string(retry_sec) + "초 후 재연결 시도");
        wake::sleep_unless_stopped(stop_token, std::chrono::seconds(retry_sec));
        retry_sec = std::min(retry_sec * 2, 30);

        if (!connected_.load() || stop_token.stop_requested())
        {
            break; // 자는 동안 disconnect()가 왔다 — 새로 붙지 않는다
        }

        // Approval key 재사용: 재연결마다 신규 발급하면 KIS가 직전 세션의 appkey를
        // 아직 해제하지 않은 상태에서 새 키로 접속 → "ALREADY IN USE appkey"(rt=9)
        // 충돌이 반복돼 재연결 폭주가 된다. 최초 연결의 approval_key_를 유지하고,
        // 비어있을 때만(발급 실패 이력 등) 재발급한다.
        if (approval_key_.empty() && !get_approval_key())
        {
            LOG_ERROR("[WS] approval key 발급 실패");
            continue;
        }

        auto fresh = websocket_platform::make_socket();

        if (!fresh->open(std::string(kis_endpoints::kWebSocketHost), kis_endpoints::websocket_port(config_.is_paper)))
        {
            LOG_ERROR("[WS] 재연결: WebSocket 연결 실패");
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(send_mutex_);
            socket_ = std::move(fresh);
        }

        ++session_generation_;

        // 재연결: 옛 연결의 체결통보 key/iv는 무효 — 새 구독응답 도착 전까지
        // 암호프레임을 drop해 stale 키 복호를 막는다 (C-3)
        aes_key_.clear();
        aes_iv_.clear();

        // 채널 재구독 — 최초 연결과 동일 경로(subscribe_all). trade_only 분기가
        // 한 곳에 모여 있어 재연결에서도 동일하게 복원된다.
        subscribe_all();
        LOG_INFO("[WS] 재연결 성공");
        // 여기서 retry_sec을 리셋하지 않는다. '재연결 성공'은 소켓 업그레이드 성공일
        // 뿐, 직후 곧바로 끊기는(ALREADY IN USE) 경우 백오프가 매번 1초로 되돌아가
        // 폭주한다. 백오프 리셋은 연결이 실제로 ≥5s 유지됐을 때만(위 uptime 판정).
    }

    connected_.store(false);
    LOG_INFO("[WS] 수신 스레드 종료");
}

void KisWebSocket::disconnect()
{
    if (!connected_.exchange(false))
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(send_mutex_);

        if (socket_)
        {
            // graceful close 프레임을 먼저 보내 KIS가 approval_key 세션을 즉시 해제하게 한다.
            // (생략 시 다음 실행이 "ALREADY IN USE appkey" rt=9로 거부됨) 블로킹 중인 수신도 여기서 깨어난다.
            socket_->close();
        }
    }

    recv_thread_.request_stop(); // 백오프 sleep 중이면 여기서 깬다

    if (recv_thread_.joinable())
    {
        recv_thread_.join();
    }

    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        socket_.reset(); // 소켓이 쥔 나머지 자원(WinHTTP 세션·커넥션 핸들)까지 놓는다
    }

    LOG_INFO("[WS] WebSocket 연결 해제 완료");
}

// ═══════════════════════════════════════════════════════════════════════════
//  공통 KisWebSocket 구현 (플랫폼 독립)
// ═══════════════════════════════════════════════════════════════════════════

KisWebSocket::KisWebSocket(const KisConfig& config) : config_(config)
{
}

KisWebSocket::~KisWebSocket()
{
    disconnect();
}

void KisWebSocket::set_callbacks(OrderBookCb on_order_book, TradeCb on_trade)
{
    on_orderbook_ = std::move(on_order_book);
    on_trade_     = std::move(on_trade);
}

void KisWebSocket::set_fill_callback(FillCb on_fill)
{
    on_fill_ = std::move(on_fill);
}

bool KisWebSocket::get_approval_key()
{
    const std::string base(kis_endpoints::rest_base_url(config_.is_paper));

    json request = {{"grant_type", "client_credentials"}, {"appkey", config_.app_key}, {"secretkey", config_.app_secret}};

    std::string response = websocket_platform::http_post_json(base + "/oauth2/Approval", request.dump());

    if (response.empty())
    {
        LOG_ERROR("[WS] Approval key 요청 실패");
        return false;
    }

    try
    {
        auto document = json::parse(response);
        approval_key_ = document["approval_key"].get<std::string>();
        LOG_INFO("[WS] Approval key 발급 성공");
        return true;
    }
    catch (...)
    {
        LOG_ERROR("[WS] Approval key 파싱 실패: " + response.substr(0, 300));
        return false;
    }
}

void KisWebSocket::send_subscribe(const std::string& transaction_id, const std::string& ticker, std::string_view tr_type)
{
    json message = {
        {"header", {{"approval_key", approval_key_}, {"custtype", "P"}, {"tr_type", std::string(tr_type)}, {"content-type", "utf-8"}}},
        {"body", {{"input", {{"tr_id", transaction_id}, {"tr_key", ticker}}}}}};
    send_text(message.dump());
    LOG_INFO(std::string(tr_type == kRelease ? "[WS] 구독 해제: " : "[WS] 구독: ") + transaction_id + " / " + ticker);
}

// specifications_ 전체를 순회해 채널을 구독한다. 최초 연결·재연결에서 공통으로 호출한다.
// 재연결 시 trade_only를 준수해야 등록 한도(약 41)를 갉아먹지 않는다(호가 미필요 종목은
// 체결만).
void KisWebSocket::subscribe_specification(const WatchSpec& specification, std::string_view tr_type)
{
    if (specification.market == Market::KR)
    {
        // KRX 전용(H0ST*)이냐 KRX+NXT 통합(H0UN*)이냐는 config.exchange가 정한다 — 필드 배열이 같아
        //  파서는 공유한다. [why D-096]
        const bool unified = kis_unified_feed(config_);

        if (!specification.trade_only)
        {
            send_subscribe(unified ? "H0UNASP0" : "H0STASP0", specification.ticker, tr_type);
        }

        send_subscribe(unified ? "H0UNCNT0" : "H0STCNT0", specification.ticker, tr_type);
    }
    else
    {
        // 미국: HDFSCNT0, tr_key = "EXCH|SYMBOL"
        std::string tr_key(specification.exchange.empty() ? std::string_view("NAS") : std::string_view(specification.exchange));
        tr_key += '|';
        tr_key += specification.ticker;
        send_subscribe("HDFSCNT0", tr_key, tr_type);
    }
}

int KisWebSocket::specification_channel_count(const WatchSpec& specification)
{
    if (specification.market == Market::KR)
    {
        return specification.trade_only ? 1 : 2;
    }

    return 1; // 미국은 체결 한 채널
}

bool KisWebSocket::subscribe_incremental(const WatchSpec& specification)
{
    {
        std::lock_guard<std::mutex> lock(specifications_mutex_);

        for (const auto& watch : specifications_)
        {
            if (watch.market == specification.market && watch.exchange == specification.exchange &&
                watch.ticker == specification.ticker)
            {
                return false; // 이미 구독 중
            }
        }

        specifications_.push_back(specification);
    }

    if (!connected_.load())
    {
        return false; // 목록에만 넣어 둔다. 실제 구독은 connect()/재연결의 subscribe_all이 한다.
    }

    const int need = specification_channel_count(specification);

    if (sub_used_.load() + need > kMaxWsSubs)
    {
        // 상한 도달 — 여기서는 구독만 거른다(체결통보 슬롯을 지킨다). 시세 대체는 Engine이
        //  has_specification()==false를 보고 ws_overflow_specs_로 REST 폴링을 돈다.
        // 목록에 남겨 두면 다음 재연결의 subscribe_all이 이 spec을 먼저 세어 뒤쪽 종목을 밀어내므로 되돌린다.
        std::lock_guard<std::mutex> lock(specifications_mutex_);

        for (auto iterator = specifications_.begin(); iterator != specifications_.end(); ++iterator)
        {
            if (iterator->market == specification.market && iterator->exchange == specification.exchange &&
                iterator->ticker == specification.ticker)
            {
                specifications_.erase(iterator);
                break;
            }
        }

        return false;
    }

    subscribe_specification(specification);
    sub_used_.fetch_add(need);
    return true;
}

bool KisWebSocket::unsubscribe_incremental(const WatchSpec& specification)
{
    {
        std::lock_guard<std::mutex> lock(specifications_mutex_);
        const auto iterator = std::find_if(specifications_.begin(), specifications_.end(), [&specification](const WatchSpec& watch)
        {
            return watch.market == specification.market && watch.exchange == specification.exchange &&
                   watch.ticker == specification.ticker;
        });

        if (iterator == specifications_.end())
        {
            return false;
        }

        specifications_.erase(iterator);
    }

    if (!connected_.load())
    {
        return true; // 목록에서 빠졌으니 connect()/재연결이 걸지 않는다
    }

    subscribe_specification(specification, kRelease);
    sub_used_.fetch_sub(specification_channel_count(specification));
    return true;
}

int KisWebSocket::free_slots() const
{
    return connected_.load() ? kMaxWsSubs - sub_used_.load() : 0;
}

bool KisWebSocket::has_specification(const WatchSpec& specification) const
{
    std::lock_guard<std::mutex> lock(specifications_mutex_);

    for (const auto& watch : specifications_)
    {
        if (watch.market == specification.market && watch.exchange == specification.exchange &&
            watch.ticker == specification.ticker)
        {
            return true;
        }
    }

    return false;
}

void KisWebSocket::subscribe_all()
{
    // 전송 중에 락을 쥐지 않도록 스냅샷을 뜬다(증분 구독이 data_thread에서 들어올 수 있다).
    std::vector<WatchSpec> snapshot;
    {
        std::lock_guard<std::mutex> lock(specifications_mutex_);
        snapshot = specifications_;
    }

    int used = 0;

    // 체결통보를 시세보다 먼저 구독한다. 세션 구독 상한(kMaxWsSubs)을 넘으면 뒤에 오는 채널이
    //  rt=1 MAX SUBSCRIBE OVER로 잘리는데, 시세가 잘리면 REST 폴링이 대신하지만 체결통보가
    //  잘리면 OrderRouter가 체결을 못 받아 reserved_가 해제되지 않는다. 그러면 같은 체결이
    //  positions_와 reserved_에 동시에 잡혀 총노출이 이중계상되고 신규 매수가 통째로 막힌다.
    //  구독 종목이 하나도 없어도 건다 — 주문만 맡은 프로세스는 목록이 빈 채로 연결하고 종목은 뒤에
    //  요청으로 온다. 종목이 있을 때만 걸면 그 프로세스는 체결통보를 영영 못 듣는다. [why D-114]
    if (on_fill_ && !config_.hts_id.empty())
    {
        std::string fill_notice_tr_id = config_.is_paper ? "H0STCNI9" : "H0STCNI0";
        send_subscribe(fill_notice_tr_id, config_.hts_id);
        ++used;
    }
    else if (on_fill_ && config_.hts_id.empty())
    {
        LOG_WARN("[WS] hts_id 미설정 — 체결통보(H0STCNI9/0) 구독 건너뜀. 주문 실행은 정상 동작.");
    }

    std::vector<WatchSpec> skipped;

    for (auto& specification : snapshot) // 밀린 항목은 snapshot에서 옮긴다 — 이 뒤로 snapshot을 안 쓴다
    {
        const int need = specification_channel_count(specification);

        if (used + need > kMaxWsSubs)
        {
            skipped.push_back(std::move(specification));
            continue;
        }

        subscribe_specification(specification);
        used += need;
    }

    sub_used_.store(used);

    if (!skipped.empty())
    {
        // 밀린 종목은 목록에서도 빼고 넘침 목록에 둔다 — subscribe_incremental의 상한 처리와 같은
        //  규약이다. 목록에 남기면 has_specification()이 true라 Engine이 REST 대체를 걸지 않아 이 종목은
        //  틱 없이 조용히 매매하지 않는다(09-11 청산 관리 시드 13종목, 465770 −6%에도 무반응).
        std::lock_guard<std::mutex> lock(specifications_mutex_);

        for (auto& specification : skipped)
        {
            for (auto iterator = specifications_.begin(); iterator != specifications_.end(); ++iterator)
            {
                if (iterator->market == specification.market && iterator->exchange == specification.exchange &&
                    iterator->ticker == specification.ticker)
                {
                    specifications_.erase(iterator);
                    break;
                }
            }

            overflow_specifications_.push_back(std::move(specification));
        }

        LOG_WARN("[WS] 구독 상한 " + std::to_string(kMaxWsSubs) + " 도달 — 시세 " +
                 std::to_string(skipped.size()) + "종목 구독 생략(REST 폴링으로 대체). 체결통보는 유지.");
    }
}

std::vector<WatchSpec> KisWebSocket::take_overflow_specifications()
{
    std::lock_guard<std::mutex> lock(specifications_mutex_);
    std::vector<WatchSpec> out;
    out.swap(overflow_specifications_);
    return out;
}
