// api/WebSocketClient.cpp — KIS 실시간 WebSocket 클라이언트의 플랫폼 독립 부분.
//  연결·재연결·백오프·구독 복원·프레임 파싱이 한 벌이고, 소켓 자체는 WsSocket(WsSocketWin/WsSocketPosix)이 맡는다.
//  스레드: recv_loop 전용 스레드가 sock_를 소유하고 바꾼다. data_thread는 send_text(send_mtx_)만 지난다. [why D-049]
#include "api/KisWebSocket.h"
#include "WsSocket.h"
#include "api/KisWsDecode.h"
#include "utils/Logger.h"
#include <algorithm>
#include <chrono>
#include <nlohmann/json.hpp>
#include <sstream>
#include <thread>

using json = nlohmann::json;

// KIS 실시간 WebSocket 접속점 — 모의투자와 실계좌는 포트만 다르다.
static constexpr const char* kWsHost = "ops.koreainvestment.com";
static constexpr int kWsPortPaper = 31000; // 모의투자
static constexpr int kWsPortReal  = 21000; // 실계좌

// ═══════════════════════════════════════════════════════════════════════════
//  공통 유틸
// ═══════════════════════════════════════════════════════════════════════════

// ─── 체결통보 복호화 (KIS H0STCNI: base64 → AES-256-CBC) ────────────────────
// 시세 채널은 평문이나 체결통보는 암호화 전송. key/iv는 구독 응답 body.output에서 획득.
// AES 본체는 플랫폼별(ws_platform::aes_cbc_decrypt).
std::string KisWebSocket::base64_decode(const std::string& in)
{
    static const std::string chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int T[256];

    for (int i = 0; i < 256; ++i)
    {
        T[i] = -1;
    }

    for (int i = 0; i < 64; ++i)
    {
        T[static_cast<unsigned char>(chars[i])] = i;
    }

    std::string out;
    int val = 0, valb = -8;

    for (unsigned char c : in)
    {
        if (T[c] == -1) // '=' / 개행 / 공백 무시
        {
            continue;
        }

        val = (val << 6) + T[c];
        valb += 6;

        if (valb >= 0)
        {
            out.push_back(static_cast<char>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }

    return out;
}

// ═══════════════════════════════════════════════════════════════════════════
//  연결 · 수신 스레드 · 재연결
// ═══════════════════════════════════════════════════════════════════════════

bool KisWebSocket::connect(const std::vector<WatchSpec>& specs)
{
    {
        std::lock_guard<std::mutex> lk(specs_mtx_);
        specs_ = specs;
    }

    if (!get_approval_key())
    {
        return false;
    }

    const int port = cfg_.is_paper ? kWsPortPaper : kWsPortReal;
    auto sock = ws_platform::make_socket();

    if (!sock->open(kWsHost, port))
    {
        LOG_ERROR("[WS] WebSocket 연결 실패: " + std::string(kWsHost) + ":" + std::to_string(port));
        return false;
    }

    // 구 수신 스레드가 자체 종료(connected_=false)로 join되지 않은 채 남아 있을 수 있다.
    // joinable 상태에서 재대입하면 std::terminate → 재대입 전 반드시 reap. sock_도 그 스레드가 만지므로 그 뒤에 바꾼다.
    if (recv_thread_.joinable())
    {
        recv_thread_.join();
    }

    {
        std::lock_guard<std::mutex> lk(send_mtx_);
        sock_ = std::move(sock);
    }

    LOG_INFO(std::string("[WS] WebSocket 연결 성공 (") + (cfg_.is_paper ? "모의투자" : "실거래") + ")");
    connected_.store(true);

    subscribe_all();

    recv_thread_ = std::thread(&KisWebSocket::recv_loop, this);
    return true;
}

void KisWebSocket::send_text(const std::string& msg)
{
    std::lock_guard<std::mutex> lk(send_mtx_);

    if (sock_)
    {
        sock_->send_text(msg);
    }
}

// [inv] sock_는 이 스레드가 바꾼다. connect()는 스레드를 띄우기 전, disconnect()는 join한 뒤에만 만지므로
//       여기서 락 없이 읽어도 된다. data_thread의 send_text와는 교체·close를 send_mtx_ 아래서 해서 갈린다.
void KisWebSocket::recv_loop()
{
    LOG_INFO("[WS] 수신 스레드 시작");
    std::string msg;
    int retry_sec = 1;

    while (connected_.load())
    {
        // 백오프는 "메시지 수신"이 아니라 "연결 유지 시간"으로 판단한다. 재연결 직후
        // 구독응답/에러프레임(ALREADY IN USE)이 곧바로 수신되면 메시지 기반 리셋은
        // 백오프를 매번 1초로 되돌려 폭주한다. 연결이 얼마나 살아있었는지로 구분한다.
        const auto conn_start = std::chrono::steady_clock::now();

        while (connected_.load() && sock_ && sock_->recv_message(msg))
        {
            parse_message(msg);
        }

        if (!connected_.load())
        {
            break;
        }

        if (sock_)
        {
            LOG_WARN("[WS] 수신 오류 (" + sock_->last_error() + ") — 재연결 준비");
        }

        // 충분히 오래(≥5s) 유지된 연결이 끊긴 것이면 일시 장애로 보고 백오프 리셋 후
        // 빠르게 재시도. 즉시 죽는 연결(=서버가 appkey 세션 미해제/off-hours abort)은
        // 지수적으로 물러서서 서버가 직전 세션을 놓을 시간을 준다. 직전 재연결이 실패해
        // sock_가 비어 있으면 conn_start가 방금이라 리셋되지 않는다 — 의도한 동작.
        if (std::chrono::steady_clock::now() - conn_start > std::chrono::seconds(5))
        {
            retry_sec = 1;
        }

        // 기존 소켓은 자기 전에 닫는다 — close 프레임이 먼저 가야 KIS가 이 approval_key 세션을
        // 놓고, 백오프로 자는 시간이 그 해제 대기가 된다. 핸들만 닫으면 세션이 남아
        // 다음 접속이 "ALREADY IN USE appkey"(rt=9)로 거부된다.
        {
            std::lock_guard<std::mutex> lk(send_mtx_);

            if (sock_)
            {
                sock_->close();
            }

            sock_.reset();
        }

        // ── 지수 백오프 재연결 ─────────────────────────────────────────
        LOG_WARN("[WS] " + std::to_string(retry_sec) + "초 후 재연결 시도");
        std::this_thread::sleep_for(std::chrono::seconds(retry_sec));
        retry_sec = std::min(retry_sec * 2, 30);

        if (!connected_.load())
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

        auto fresh = ws_platform::make_socket();

        if (!fresh->open(kWsHost, cfg_.is_paper ? kWsPortPaper : kWsPortReal))
        {
            LOG_ERROR("[WS] 재연결: WebSocket 연결 실패");
            continue;
        }

        {
            std::lock_guard<std::mutex> lk(send_mtx_);
            sock_ = std::move(fresh);
        }

        // 재연결: 옛 연결의 체결통보 key/iv는 무효 — 새 구독응답 도착 전까지
        // 암호프레임을 drop해 stale 키 복호를 막는다 (C-3)
        aes_key_.clear();
        aes_iv_.clear();

        // 채널 재구독 — 최초 연결과 동일 경로(subscribe_all). trade_only·선물 분기가
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
        std::lock_guard<std::mutex> lk(send_mtx_);

        if (sock_)
        {
            // graceful close 프레임을 먼저 보내 KIS가 approval_key 세션을 즉시 해제하게 한다.
            // (생략 시 다음 실행이 "ALREADY IN USE appkey" rt=9로 거부됨) 블로킹 중인 수신도 여기서 깨어난다.
            sock_->close();
        }
    }

    if (recv_thread_.joinable())
    {
        recv_thread_.join();
    }

    {
        std::lock_guard<std::mutex> lk(send_mtx_);
        sock_.reset(); // 소켓이 쥔 나머지 자원(WinHTTP 세션·커넥션 핸들)까지 놓는다
    }

    LOG_INFO("[WS] WebSocket 연결 해제 완료");
}

// ═══════════════════════════════════════════════════════════════════════════
//  공통 KisWebSocket 구현 (플랫폼 독립)
// ═══════════════════════════════════════════════════════════════════════════

KisWebSocket::KisWebSocket(const KisConfig& cfg) : cfg_(cfg)
{
}

KisWebSocket::~KisWebSocket()
{
    disconnect();
}

void KisWebSocket::set_callbacks(OrderBookCb on_ob, TradeCb on_trade)
{
    on_orderbook_ = std::move(on_ob);
    on_trade_     = std::move(on_trade);
}

void KisWebSocket::set_fill_callback(FillCb on_fill)
{
    on_fill_ = std::move(on_fill);
}

bool KisWebSocket::get_approval_key()
{
    std::string base =
        cfg_.is_paper ? "https://openapivts.koreainvestment.com:29443" : "https://openapi.koreainvestment.com:9443";

    json req = {{"grant_type", "client_credentials"}, {"appkey", cfg_.app_key}, {"secretkey", cfg_.app_secret}};

    std::string resp = ws_platform::http_post_json(base + "/oauth2/Approval", req.dump());

    if (resp.empty())
    {
        LOG_ERROR("[WS] Approval key 요청 실패");
        return false;
    }

    try
    {
        auto j = json::parse(resp);
        approval_key_ = j["approval_key"].get<std::string>();
        LOG_INFO("[WS] Approval key 발급 성공");
        return true;
    }
    catch (...)
    {
        LOG_ERROR("[WS] Approval key 파싱 실패: " + resp.substr(0, 300));
        return false;
    }
}

void KisWebSocket::send_subscribe(const std::string& tr_id, const std::string& ticker)
{
    json msg = {
        {"header", {{"approval_key", approval_key_}, {"custtype", "P"}, {"tr_type", "1"}, {"content-type", "utf-8"}}},
        {"body", {{"input", {{"tr_id", tr_id}, {"tr_key", ticker}}}}}};
    send_text(msg.dump());
    LOG_INFO("[WS] 구독: " + tr_id + " / " + ticker);
}

// specs_ 전체를 순회해 채널을 구독한다. 최초 연결·재연결에서 공통으로 호출한다.
// 재연결 시 trade_only를 준수해야 등록 한도(약 41)를 갉아먹지 않는다(호가 미필요 종목은
// 체결만). 선물은 WatchSpec.is_future로 골라 H0IFASP0/H0IFCNT0을 구독한다.
void KisWebSocket::subscribe_spec(const WatchSpec& spec)
{
    if (spec.is_future)
    {
        // 국내 선물: tr_key = 선물 종목코드(예 101W09), 미국과 달리 exchange prefix 없음.
        if (!spec.trade_only)
        {
            send_subscribe("H0IFASP0", spec.ticker);
        }

        send_subscribe("H0IFCNT0", spec.ticker);
    }
    else if (spec.market == Market::KR)
    {
        if (!spec.trade_only)
        {
            send_subscribe("H0STASP0", spec.ticker);
        }

        send_subscribe("H0STCNT0", spec.ticker);
    }
    else
    {
        // 미국: HDFSCNT0, tr_key = "EXCH|SYMBOL"
        std::string exch = spec.exchange.empty() ? "NAS" : spec.exchange;
        send_subscribe("HDFSCNT0", exch + "|" + spec.ticker);
    }
}

int KisWebSocket::spec_channel_count(const WatchSpec& spec)
{
    if (spec.market == Market::KR || spec.is_future)
    {
        return spec.trade_only ? 1 : 2;
    }

    return 1; // 미국은 체결 한 채널
}

bool KisWebSocket::subscribe_incremental(const WatchSpec& spec)
{
    {
        std::lock_guard<std::mutex> lk(specs_mtx_);

        for (const auto& w : specs_)
        {
            if (w.market == spec.market && w.exchange == spec.exchange &&
                w.ticker == spec.ticker && w.is_future == spec.is_future)
            {
                return false; // 이미 구독 중
            }
        }

        specs_.push_back(spec);
    }

    if (!connected_.load())
    {
        return false; // 목록에만 넣어 둔다. 실제 구독은 connect()/재연결의 subscribe_all이 한다.
    }

    const int need = spec_channel_count(spec);

    if (sub_used_.load() + need > kMaxWsSubs)
    {
        // 상한 도달 — 시세는 REST 폴링으로 대체된다(체결통보 슬롯을 지킨다).
        // 목록에 남겨 두면 다음 재연결의 subscribe_all이 이 spec을 먼저 세어 뒤쪽 종목을 밀어내므로 되돌린다.
        std::lock_guard<std::mutex> lk(specs_mtx_);

        for (auto it = specs_.begin(); it != specs_.end(); ++it)
        {
            if (it->market == spec.market && it->exchange == spec.exchange &&
                it->ticker == spec.ticker && it->is_future == spec.is_future)
            {
                specs_.erase(it);
                break;
            }
        }

        return false;
    }

    subscribe_spec(spec);
    sub_used_.fetch_add(need);
    return true;
}

void KisWebSocket::subscribe_all()
{
    // 전송 중에 락을 쥐지 않도록 스냅샷을 뜬다(증분 구독이 data_thread에서 들어올 수 있다).
    std::vector<WatchSpec> snapshot;
    {
        std::lock_guard<std::mutex> lk(specs_mtx_);
        snapshot = specs_;
    }

    bool has_kr = false;

    for (const auto& spec : snapshot)
    {
        if (!spec.is_future && spec.market == Market::KR)
        {
            has_kr = true;
        }
    }

    int used = 0;

    // 체결통보를 시세보다 먼저 구독한다. 세션 구독 상한(kMaxWsSubs)을 넘으면 뒤에 오는 채널이
    //  rt=1 MAX SUBSCRIBE OVER로 잘리는데, 시세가 잘리면 REST 폴링이 대신하지만 체결통보가
    //  잘리면 OrderRouter가 체결을 못 받아 reserved_가 해제되지 않는다. 그러면 같은 체결이
    //  positions_와 reserved_에 동시에 잡혀 총노출이 이중계상되고 신규 매수가 통째로 막힌다.
    if (has_kr && on_fill_ && !cfg_.hts_id.empty())
    {
        std::string fill_tr = cfg_.is_paper ? "H0STCNI9" : "H0STCNI0";
        send_subscribe(fill_tr, cfg_.hts_id);
        ++used;
    }
    else if (has_kr && on_fill_ && cfg_.hts_id.empty())
    {
        LOG_WARN("[WS] hts_id 미설정 — 체결통보(H0STCNI9/0) 구독 건너뜀. 주문 실행은 정상 동작.");
    }

    int skipped = 0;

    for (const auto& spec : snapshot)
    {
        const int need = spec_channel_count(spec);

        if (used + need > kMaxWsSubs) { ++skipped; continue; }
        subscribe_spec(spec);
        used += need;
    }

    sub_used_.store(used);

    if (skipped > 0)
    {
        LOG_WARN("[WS] 구독 상한 " + std::to_string(kMaxWsSubs) + " 도달 — 시세 " +
                 std::to_string(skipped) + "종목 구독 생략(REST 폴링으로 대체). 체결통보는 유지.");
    }
}

void KisWebSocket::parse_message(const std::string& msg)
{
    if (msg.empty())
    {
        return;
    }

    on_message_received(); // 모든 수신 메시지에서 stale 타이머 리셋

    if (msg[0] == '{')
    {
        try
        {
            auto j = json::parse(msg);
            std::string tr_id = j["header"].value("tr_id", "");

            // PINGPONG: KIS가 주기적으로 보내는 연결 유지 신호(heartbeat). 받은 그대로 되돌려준다.
            if (tr_id == "PINGPONG")
            {
                send_text(msg);
                return;
            }

            if (j.contains("body"))
            {
                std::string rt = j["body"].value("rt_cd", "?");
                std::string msg1 = j["body"].value("msg1", "");
                std::string tk = j["header"].value("tr_key", "");
                LOG_INFO("[WS] " + tr_id + "(" + tk + ") rt=" + rt + " " + msg1);

                // 체결통보 구독 응답: AES key/iv 확보 → 이후 암호화 프레임 복호화에 사용
                if ((tr_id == "H0STCNI0" || tr_id == "H0STCNI9") &&
                    j["body"].contains("output"))
                {
                    const auto& out = j["body"]["output"];
                    std::string k = out.value("key", "");
                    std::string v = out.value("iv", "");

                    // AES-256-CBC: key는 정확히 32바이트, iv는 16바이트여야 함.
                    // 길이가 다르면(서버 포맷 변경 등) 앞 N바이트만 써서 잘못된 키로
                    // 복호→쓰레기 평문이 원장에 들어가므로 등호 검증 후 거부 (C-2)
                    if (k.size() == 32 && v.size() == 16)
                    {
                        aes_key_ = std::move(k);
                        aes_iv_  = std::move(v);
                        LOG_INFO("[WS] 체결통보 AES key/iv 확보 — 복호화 준비 완료");
                    }
                    else
                    {
                        aes_key_.clear();
                        aes_iv_.clear();
                        LOG_WARN("[WS] 체결통보 key/iv 길이 비정상 (key=" +
                                 std::to_string(k.size()) + " iv=" + std::to_string(v.size()) +
                                 ") — 복호화 불가, 암호프레임 drop");
                    }
                }
            }
        }
        catch (...)
        {
        }

        return;
    }

    // 데이터 메시지: TYPE|TR_ID|COUNT|DATA (^-구분 필드). parts_·fields_는 msg를 가리키는 뷰라
    //  msg보다 오래 살지 않는다 — 콜백은 이 함수 안에서 끝난다.
    kis_ws::split_fields(msg, '|', parts_);

    if (parts_.size() < 4)
    {
        return;
    }

    const std::string_view tr_id = parts_[1];
    std::string_view data = parts_[3];
    std::string plain; // 암호화 프레임의 복호문. data가 이쪽을 가리키게 되므로 같은 범위에 둔다

    // 암호화 프레임(체결통보 H0STCNI): parts[0]=="1" → base64 + AES-256-CBC 복호화
    if (parts_[0] == "1")
    {
        if (aes_key_.empty() || aes_iv_.empty())
        {
            LOG_WARN("[WS] 암호화 프레임 수신했으나 key/iv 미확보 — drop tr_id=" + std::string(tr_id));
            return;
        }

        plain = ws_platform::aes_cbc_decrypt(base64_decode(std::string(data)), aes_key_, aes_iv_);

        if (plain.empty())
        {
            LOG_WARN("[WS] 체결통보 복호화 실패 tr_id=" + std::string(tr_id));
            return;
        }

        data = plain;
    }

    kis_ws::split_fields(data, '^', fields_);

    // 복호 평문은 ^구분 다필드(체결통보 23필드). 너무 적으면 키 불일치/손상 의심 (C-1)
    if (!plain.empty() && fields_.size() < kis_ws::kMinFieldsFill)
    {
        LOG_WARN("[WS] 체결통보 복호 평문 비정상(필드부족) — 키 불일치/손상 의심 tr_id=" + std::string(tr_id));
        return;
    }

    const kis_ws::Fields fields(fields_);

    // [wire] parts[2] = 이 프레임에 실린 레코드 수(COUNT). 1이면 기존 단건 경로 그대로. 못 읽으면 1.
    //  COUNT>1인데 자르지 못하면(폭이 안 맞음) 첫 레코드만 처리하던 종전 동작을 유지하고 한 번만 경고한다.
    int rec_count = 1;

    if (!kis_ws::detail::to_int(parts_[2], rec_count))
    {
        rec_count = 1;
    }

    if (rec_count > 1)
    {
        const auto recs = kis_ws::split_records(fields, rec_count, min_fields_for(tr_id));

        if (!recs.empty())
        {
            for (size_t r = 0; r < recs.size(); ++r)
            {
                dispatch_record(tr_id, recs[r]);
            }

            return;
        }

        if (multi_rec_warned_ < 1)
        {
            ++multi_rec_warned_;
            LOG_WARN("[WS] 다건 프레임 분리 실패 tr_id=" + std::string(tr_id) + " count=" +
                     std::to_string(rec_count) + " fields=" + std::to_string(fields.size()) +
                     " — 첫 레코드만 처리(이 경고는 1회만)");
        }
    }

    dispatch_record(tr_id, fields);
}

size_t KisWebSocket::min_fields_for(std::string_view tr_id) noexcept
{
    if (tr_id == "H0STASP0")
    {
        return 38;
    }

    if (tr_id == "H0STCNT0")
    {
        return 22;
    }

    if (tr_id == "H0IFASP0")
    {
        return 32;
    }

    if (tr_id == "H0IFCNT0")
    {
        return 19;
    }

    if (tr_id == "HDFSCNT0")
    {
        return 9;
    }

    if (tr_id == "H0STCNI0" || tr_id == "H0STCNI9")
    {
        return 14;
    }

    return 0;
}

void KisWebSocket::dispatch_record(std::string_view tr_id, kis_ws::Fields f)
{
    if (tr_id == "H0STASP0")
    {
        parse_orderbook(f);
    }
    else if (tr_id == "H0STCNT0")
    {
        parse_kr_trade(f);
    }
    else if (tr_id == "H0IFASP0")
    {
        parse_fut_orderbook(f);
    }
    else if (tr_id == "H0IFCNT0")
    {
        parse_fut_trade(f);
    }
    else if (tr_id == "HDFSCNT0")
    {
        parse_us_trade(f);
    }
    else if (tr_id == "H0STCNI0" || tr_id == "H0STCNI9")
    {
        parse_fill_notification(f);
    }
}

// ─── 채널 파서 ─────────────────────────────────────────────────────────
// 필드 위치·최소 길이·숫자 변환은 api/KisWsDecode.h(헤더 전용, 테스트 대상)가 갖는다.
// 여기는 진단 로그와 콜백 호출만 남긴다. [why D-037]

// 채널별 첫 수신 레코드를 한 번만 통째로 찍는다. 전문 필드 순서를 실데이터로 확인하는 용도라
// 채널당 한 줄이면 충분하다(종목마다 찍던 set 조회를 틱 경로에서 뺐다, D-042). max_fields=0이면 전 필드.
static void log_first_record(bool& logged, const char* channel, kis_ws::Fields f, size_t max_fields,
                             const char* sep)
{
    if (logged)
    {
        return;
    }

    logged = true;
    std::string dbg = std::string("[WS] ") + channel + " 첫 수신 [" + std::string(f[0]) + "] 총 " +
                      std::to_string(f.size()) + "필드:";
    const size_t n = (max_fields == 0) ? f.size() : std::min(f.size(), max_fields);

    for (size_t i = 0; i < n; ++i)
    {
        dbg += sep + std::string("[") + std::to_string(i) + "]=";
        dbg.append(f[i].data(), f[i].size());
    }

    LOG_INFO(dbg);
}

// 호가·체결은 숫자 하나가 비어도 흘려보낸다(kBadNumber → 0으로 남긴 채 콜백). 이 관대함은
// 옛 동작을 그대로 옮긴 것이고, 버릴지는 C-4에서 정한다.
void KisWebSocket::parse_orderbook(kis_ws::Fields f)
{
    static bool first_logged = false;
    OrderBook ob;
    const auto rc = kis_ws::decode_orderbook(f, ob);

    if (rc == kis_ws::Decode::kShort)
    {
        LOG_WARN("[WS] H0STASP0 필드 부족: " + std::to_string(f.size()) + " (" +
                 std::to_string(kis_ws::kMinFieldsOrderbook) + " 필요)");
        return;
    }

    log_first_record(first_logged, "H0STASP0", f, 0, " ");

    if (on_orderbook_)
    {
        on_orderbook_(ob);
    }
}

void KisWebSocket::parse_kr_trade(kis_ws::Fields f)
{
    static bool first_logged = false;
    TradeData td;

    if (kis_ws::decode_kr_trade(f, td) == kis_ws::Decode::kShort)
    {
        return;
    }

    log_first_record(first_logged, "H0STCNT0", f, 13, "\n  ");

    if (on_trade_)
    {
        on_trade_(td);
    }
}

// tr_key 형식: "NAS|AAPL" → ticker = "AAPL". 방향 필드 f[20]은 실데이터 미검증(보류 목록).
void KisWebSocket::parse_us_trade(kis_ws::Fields f)
{
    static bool first_us_logged = false;
    TradeData td;

    if (kis_ws::decode_us_trade(f, td) == kis_ws::Decode::kShort)
    {
        return;
    }

    log_first_record(first_us_logged, "HDFSCNT0", f, 15, "\n  ");

    if (on_trade_)
    {
        on_trade_(td);
    }
}

// 선물 체결엔 매수/매도 구분 코드가 없어 direction=0으로 나간다.
void KisWebSocket::parse_fut_trade(kis_ws::Fields f)
{
    static bool first_logged = false;
    TradeData td;

    if (kis_ws::decode_fut_trade(f, td) == kis_ws::Decode::kShort)
    {
        return;
    }

    log_first_record(first_logged, "H0IFCNT0", f, 19, "\n  ");

    if (on_trade_)
    {
        on_trade_(td);
    }
}

void KisWebSocket::parse_fut_orderbook(kis_ws::Fields f)
{
    static bool first_logged = false;
    OrderBook ob;
    const auto rc = kis_ws::decode_fut_orderbook(f, ob);

    if (rc == kis_ws::Decode::kShort)
    {
        LOG_WARN("[WS] H0IFASP0 필드 부족: " + std::to_string(f.size()) + " (" +
                 std::to_string(kis_ws::kMinFieldsFutOrderbook) + " 필요)");
        return;
    }

    log_first_record(first_logged, "H0IFASP0", f, 0, " ");

    if (on_orderbook_)
    {
        on_orderbook_(ob);
    }
}

// 체결통보는 원장에 들어가므로 관대하지 않다 — 읽지 못한 레코드는 버리고 WARN을 남긴다.
void KisWebSocket::parse_fill_notification(kis_ws::Fields f)
{
    if (f.size() < kis_ws::kMinFieldsFill)
    {
        // 1~2필드: KIS 서버 제어 메시지(ack/heartbeat) — 정상 동작이라 조용히 넘긴다
        if (f.size() > 2)
        {
            LOG_WARN("[WS] H0STCNI 필드 부족: " + std::to_string(f.size()));
        }

        return;
    }

    if (!on_fill_) // 콜백 미등록 시 즉시 반환 (파싱 비용 절감)
    {
        return;
    }

    FillNotification fn;

    switch (kis_ws::decode_fill(f, fn))
    {
    case kis_ws::Decode::kOk:
        break;

    case kis_ws::Decode::kBadSide:
        LOG_WARN("[WS] H0STCNI 매매구분 알 수 없음 '" + std::string(f[4]) + "' ODNO=" + std::string(f[2]) +
                 " — 체결 무시");
        return;

    case kis_ws::Decode::kBadNumber:
        LOG_WARN("[WS] H0STCNI0 수량/단가 파싱 오류 ODNO=" + fn.odno);
        return;

    default: // kSkip: 접수/정정/취소/거부 통보(CNTG_YN=1), kShort는 위에서 걸렀다
        return;
    }

    LOG_INFO("[WS] 체결통보 ODNO=" + fn.odno + " " + fn.ticker +
             (fn.side == OrderSide::BUY ? " BUY " : " SELL ") +
             std::to_string(fn.filled_qty) + "주 @" +
             std::to_string(static_cast<int>(fn.filled_price)));
    on_fill_(fn);
}
