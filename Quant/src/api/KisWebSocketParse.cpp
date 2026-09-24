// api/KisWebSocketParse.cpp — KIS 실시간 WebSocket 수신 프레임 파싱(KisWebSocket의 나머지 반).
//  제어 프레임(구독 응답·PINGPONG)·데이터 프레임을 채널별로 나눠 디코더(Quant/src/api/KisWsDecode.cpp)에 넘기고
//  콜백을 부른다. 체결통보는 base64 → AES로 풀어 넘긴다. 연결·재연결·구독은 Quant/src/api/WebSocketClient.cpp.
//  스레드: recv_loop 전용 스레드만 부른다(parse_message는 recv_loop 안에서 불린다). [why D-049]
#include "api/KisWebSocket.h"
#include "WsSocket.h"
#include "api/KisWsDecode.h"
#include "utils/Logger.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <nlohmann/json.hpp>
#include <string_view>

using json = nlohmann::json;

// 수신 시각. 소켓 읽기 스레드가 디코드 직후 찍는다 — 호가·체결이 같은 시계를 쓰므로 뒤 단계(multiplexer·샤드)가 도착 순서를
//  되돌릴 수 있다. 구간 지연 CSV의 출발점도 이 값이다. [why D-071]
static int64_t recv_now_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// ═══════════════════════════════════════════════════════════════════════════
//  공통 유틸
// ═══════════════════════════════════════════════════════════════════════════

// ─── 체결통보 복호화 (KIS H0STCNI: base64 → AES-256-CBC) ────────────────────
// 시세 채널은 평문이나 체결통보는 암호화 전송. key/iv는 구독 응답 body.output에서 획득.
// AES 본체는 플랫폼별(websocket_platform::aes_cbc_decrypt).
namespace
{

// base64 역방향 표. 문자 집합이 고정이라 컴파일 타임에 만든다 — 함수 안 static이면 호출마다 초기화 가드를
//  거치는데(원칙 6, hot path에 런타임 초기화 없음) constexpr는 가드도 초기화도 없이 .rodata에 박힌다.
constexpr std::array<int, 256> make_base64_table()
{
    constexpr std::string_view chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::array<int, 256>       table{};
    table.fill(-1);

    for (int index = 0; index < 64; ++index)
    {
        table[static_cast<unsigned char>(chars[index])] = index;
    }

    return table;
}

constexpr std::array<int, 256> kBase64Table = make_base64_table();

} // namespace

std::string KisWebSocket::base64_decode(std::string_view in)
{
    std::string out;
    int value = 0, value_bits = -8;

    for (unsigned char character : in)
    {
        if (kBase64Table[character] == -1) // '=' / 개행 / 공백 무시
        {
            continue;
        }

        value = (value << 6) + kBase64Table[character];
        value_bits += 6;

        if (value_bits >= 0)
        {
            out.push_back(static_cast<char>((value >> value_bits) & 0xFF));
            value_bits -= 8;
        }
    }

    return out;
}

// ═══════════════════════════════════════════════════════════════════════════
//  수신 프레임 파싱
// ═══════════════════════════════════════════════════════════════════════════

void KisWebSocket::parse_message(const std::string& message)
{
    if (message.empty())
    {
        return;
    }

    on_message_received(); // 모든 수신 메시지에서 stale 타이머 리셋

    if (message[0] == '{')
    {
        handle_control_frame(message);
        return;
    }

    handle_data_frame(message);
}

void KisWebSocket::handle_control_frame(const std::string& message)
{
    try
    {
        auto document = json::parse(message);
        std::string transaction_id = document["header"].value("tr_id", "");

        // PINGPONG: KIS가 주기적으로 보내는 연결 유지 신호(heartbeat). 받은 그대로 되돌려준다.
        if (transaction_id == "PINGPONG")
        {
            send_text(message);
            return;
        }

        if (document.contains("body"))
        {
            std::string rt = document["body"].value("rt_cd", "?");
            std::string msg1 = document["body"].value("msg1", "");
            std::string ticker = document["header"].value("tr_key", "");
            LOG_INFO("[WS] " + transaction_id + "(" + ticker + ") rt=" + rt + " " + msg1);

            // 체결통보 구독 응답: AES key/initialization_vector 확보 → 이후 암호화 프레임 복호화에 사용
            if ((transaction_id == "H0STCNI0" || transaction_id == "H0STCNI9") &&
                document["body"].contains("output"))
            {
                const auto& out = document["body"]["output"];
                std::string key = out.value("key", "");
                std::string value = out.value("iv", "");

                // AES-256-CBC: key는 정확히 32바이트, iv는 16바이트여야 함.
                // 길이가 다르면(서버 포맷 변경 등) 앞 N바이트만 써서 잘못된 키로
                // 복호→쓰레기 평문이 원장에 들어가므로 등호 검증 후 거부 (C-2)
                if (key.size() == 32 && value.size() == 16)
                {
                    aes_key_ = std::move(key);
                    aes_iv_  = std::move(value);
                    LOG_INFO("[WS] 체결통보 AES key/iv 확보 — 복호화 준비 완료");
                }
                else
                {
                    aes_key_.clear();
                    aes_iv_.clear();
                    LOG_WARN("[WS] 체결통보 key/iv 길이 비정상 (key=" +
                             std::to_string(key.size()) + " iv=" + std::to_string(value.size()) +
                             ") — 복호화 불가, 암호프레임 drop");
                }
            }
        }
    }
    catch (...)
    {
    }
}

void KisWebSocket::handle_data_frame(const std::string& message)
{
    // 데이터 메시지: TYPE|TR_ID|COUNT|DATA (^-구분 필드). parts_·fields_는 message를 가리키는 뷰라
    //  message보다 오래 살지 않는다 — 콜백은 이 함수 안에서 끝난다.
    kis_websocket::split_fields(message, '|', parts_);

    if (parts_.size() < 4)
    {
        return;
    }

    const std::string_view transaction_id = parts_[1];
    std::string_view data = parts_[3];
    std::string plain; // 암호화 프레임의 복호문. data가 이쪽을 가리키게 되므로 같은 범위에 둔다

    // 암호화 프레임(체결통보 H0STCNI): parts[0]=="1" → base64 + AES-256-CBC 복호화
    if (parts_[0] == "1")
    {
        if (aes_key_.empty() || aes_iv_.empty())
        {
            LOG_WARN(std::string("[WS] 암호화 프레임 수신했으나 key/iv 미확보 — drop tr_id=").append(transaction_id));
            return;
        }

        plain = websocket_platform::aes_cbc_decrypt(base64_decode(data), aes_key_, aes_iv_);

        if (plain.empty())
        {
            LOG_WARN(std::string("[WS] 체결통보 복호화 실패 tr_id=").append(transaction_id));
            return;
        }

        data = plain;
    }

    kis_websocket::split_fields(data, '^', fields_);

    // 복호 평문은 ^구분 다필드(체결통보 26칸 — 공식 예제 대조). 너무 적으면 키 불일치/손상 의심 (C-1)
    if (!plain.empty() && fields_.size() < kis_websocket::kMinFieldsFill)
    {
        LOG_WARN(std::string("[WS] 체결통보 복호 평문 비정상(필드부족) — 키 불일치/손상 의심 tr_id=").append(transaction_id));
        return;
    }

    const kis_websocket::Fields fields(fields_);

    // [wire] parts[2] = 이 프레임에 실린 레코드 수(COUNT). 1이면 기존 단건 경로 그대로. 못 읽으면 1.
    //  COUNT>1인데 자르지 못하면(폭이 안 맞음) 첫 레코드만 처리하던 종전 동작을 유지하고 한 번만 경고한다.
    int rec_count = 1;

    if (!kis_websocket::detail::to_int(parts_[2], rec_count))
    {
        rec_count = 1;
    }

    if (rec_count > 1)
    {
        const auto records = kis_websocket::split_records(fields, rec_count, min_fields_for(transaction_id));

        if (!records.empty())
        {
            for (size_t rec_index = 0; rec_index < records.size(); ++rec_index)
            {
                dispatch_record(transaction_id, records[rec_index]);
            }

            return;
        }

        if (multi_rec_warned_ < 1)
        {
            ++multi_rec_warned_;
            LOG_WARN("[WS] 다건 프레임 분리 실패 tr_id=" + std::string(transaction_id) + " count=" +
                     std::to_string(rec_count) + " fields=" + std::to_string(fields.size()) +
                     " — 첫 레코드만 처리(이 경고는 1회만)");
        }
    }

    dispatch_record(transaction_id, fields);
}

size_t KisWebSocket::min_fields_for(std::string_view transaction_id) noexcept
{
    if (transaction_id == "H0STASP0" || transaction_id == "H0UNASP0")
    {
        return 38;
    }

    if (transaction_id == "H0STCNT0" || transaction_id == "H0UNCNT0")
    {
        return 22;
    }

    if (transaction_id == "H0IFASP0")
    {
        return 32;
    }

    if (transaction_id == "H0IFCNT0")
    {
        return 19;
    }

    if (transaction_id == "HDFSCNT0")
    {
        return 9;
    }

    if (transaction_id == "H0STCNI0" || transaction_id == "H0STCNI9")
    {
        return 14;
    }

    return 0;
}

void KisWebSocket::dispatch_record(std::string_view transaction_id, kis_websocket::Fields fields)
{
    if (transaction_id == "H0STASP0" || transaction_id == "H0UNASP0")
    {
        parse_orderbook(fields);
    }
    else if (transaction_id == "H0STCNT0" || transaction_id == "H0UNCNT0")
    {
        parse_kr_trade(fields);
    }
    else if (transaction_id == "H0IFASP0")
    {
        parse_future_orderbook(fields);
    }
    else if (transaction_id == "H0IFCNT0")
    {
        parse_future_trade(fields);
    }
    else if (transaction_id == "HDFSCNT0")
    {
        parse_us_trade(fields);
    }
    else if (transaction_id == "H0STCNI0" || transaction_id == "H0STCNI9")
    {
        parse_fill_notification(fields);
    }
}

// ─── 채널 파서 ─────────────────────────────────────────────────────────
// 필드 위치·최소 길이·숫자 변환은 디코더가 갖는다(테스트 대상) — Quant/include/api/KisWsDecode.h에 선언,
//  Quant/src/api/KisWsDecode.cpp에 구현.
// 여기는 진단 로그와 콜백 호출만 남긴다. [why D-037]

// 채널별 첫 수신 레코드를 한 번만 통째로 찍는다. 전문 필드 순서를 실데이터로 확인하는 용도라
// 채널당 한 줄이면 충분하다(종목마다 찍던 set 조회를 틱 경로에서 뺐다, D-042). max_fields=0이면 전 필드.
static void log_first_record(bool& logged, const char* channel, kis_websocket::Fields fields, size_t max_fields,
                             const char* sep)
{
    if (logged)
    {
        return;
    }

    logged = true;
    std::string debug = std::string("[WS] ") + channel + " 첫 수신 [" + std::string(fields[0]) + "] 총 " +
                      std::to_string(fields.size()) + "필드:";
    const size_t count = (max_fields == 0) ? fields.size() : std::min(fields.size(), max_fields);

    for (size_t index = 0; index < count; ++index)
    {
        debug += sep + std::string("[") + std::to_string(index) + "]=";
        debug.append(fields[index].data(), fields[index].size());
    }

    LOG_INFO(debug);
}

// 호가·체결은 숫자 하나가 비어도 흘려보낸다(kBadNumber → 0으로 남긴 채 콜백). 이 관대함은
// 옛 동작을 그대로 옮긴 것이고, 버릴지는 C-4에서 정한다.
void KisWebSocket::parse_orderbook(kis_websocket::Fields fields)
{
    static bool first_logged = false;
    OrderBook order_book;
    const auto result_code = kis_websocket::decode_orderbook(fields, order_book);

    if (result_code == kis_websocket::Decode::kShort)
    {
        LOG_WARN("[WS] H0STASP0 필드 부족: " + std::to_string(fields.size()) + " (" +
                 std::to_string(kis_websocket::kMinFieldsOrderbook) + " 필요)");
        return;
    }

    order_book.received_ns = recv_now_ns();

    log_first_record(first_logged, "H0STASP0", fields, 0, " ");

    if (on_orderbook_)
    {
        on_orderbook_(order_book);
    }
}

void KisWebSocket::parse_kr_trade(kis_websocket::Fields fields)
{
    static bool first_logged = false;
    TradeData trade;

    if (kis_websocket::decode_kr_trade(fields, trade) == kis_websocket::Decode::kShort)
    {
        return;
    }

    trade.received_ns = recv_now_ns();

    log_first_record(first_logged, "H0STCNT0", fields, 13, "\n  ");

    if (on_trade_)
    {
        on_trade_(trade);
    }
}

// fields[0]을 그대로 ticker로 쓴다(거래소 접두어를 떼는 변환은 없다). 방향 필드 f[20]은 실데이터 미검증(보류 목록).
void KisWebSocket::parse_us_trade(kis_websocket::Fields fields)
{
    static bool first_us_logged = false;
    TradeData trade;

    if (kis_websocket::decode_us_trade(fields, trade) == kis_websocket::Decode::kShort)
    {
        return;
    }

    trade.received_ns = recv_now_ns();

    log_first_record(first_us_logged, "HDFSCNT0", fields, 15, "\n  ");

    if (on_trade_)
    {
        on_trade_(trade);
    }
}

// 선물 체결엔 매수/매도 구분 코드가 없어 direction=0으로 나간다.
void KisWebSocket::parse_future_trade(kis_websocket::Fields fields)
{
    static bool first_logged = false;
    TradeData trade;

    if (kis_websocket::decode_future_trade(fields, trade) == kis_websocket::Decode::kShort)
    {
        return;
    }

    trade.received_ns = recv_now_ns();

    log_first_record(first_logged, "H0IFCNT0", fields, 19, "\n  ");

    if (on_trade_)
    {
        on_trade_(trade);
    }
}

void KisWebSocket::parse_future_orderbook(kis_websocket::Fields fields)
{
    static bool first_logged = false;
    OrderBook order_book;
    const auto result_code = kis_websocket::decode_future_orderbook(fields, order_book);

    if (result_code == kis_websocket::Decode::kShort)
    {
        LOG_WARN("[WS] H0IFASP0 필드 부족: " + std::to_string(fields.size()) + " (" +
                 std::to_string(kis_websocket::kMinFieldsFutOrderbook) + " 필요)");
        return;
    }

    order_book.received_ns = recv_now_ns();

    log_first_record(first_logged, "H0IFASP0", fields, 0, " ");

    if (on_orderbook_)
    {
        on_orderbook_(order_book);
    }
}

// 체결통보는 원장에 들어가므로 관대하지 않다 — 읽지 못한 레코드는 버리고 WARN을 남긴다.
void KisWebSocket::parse_fill_notification(kis_websocket::Fields fields)
{
    if (fields.size() < kis_websocket::kMinFieldsFill)
    {
        // 1~2필드: KIS 서버 제어 메시지(acknowledgement/heartbeat) — 정상 동작이라 조용히 넘긴다
        if (fields.size() > 2)
        {
            LOG_WARN("[WS] H0STCNI 필드 부족: " + std::to_string(fields.size()));
        }

        return;
    }

    if (!on_fill_) // 콜백 미등록 시 즉시 반환 (파싱 비용 절감)
    {
        return;
    }

    FillNotification fill_notification;

    switch (kis_websocket::decode_fill(fields, fill_notification))
    {
    case kis_websocket::Decode::kOk:
        break;

    case kis_websocket::Decode::kBadSide:
        LOG_WARN("[WS] H0STCNI 매매구분 알 수 없음 '" + std::string(fields[4]) + "' ODNO=" + std::string(fields[2]) +
                 " — 체결 무시");
        return;

    case kis_websocket::Decode::kBadNumber:
        LOG_WARN("[WS] H0STCNI0 수량/단가 파싱 오류 ODNO=" + fill_notification.kis_order_no);
        return;

    default: // kSkip: 접수/정정/취소/거부 통보(CNTG_YN=1), kShort는 위에서 걸렀다
        return;
    }

    fill_notification.session_generation = session_generation_;

    // 주문수량·거래소는 전문 뒤쪽 칸이라 짧은 전문에서는 안 온다. 실제로 오는지를 로그로 확인할 수 있게
    //  받은 때만 덧붙인다 — 안 오면 미연결 잔량 상한이 종전(키 중복 제거)으로 떨어진다.
    std::string extra;

    if (fill_notification.order_quantity > 0)
    {
        extra += " 주문수량=" + std::to_string(fill_notification.order_quantity) + "주";
    }

    if (!fill_notification.exchange.empty())
    {
        extra += " 거래소=" + fill_notification.exchange;
    }

    LOG_INFO("[WS] 체결통보 ODNO=" + fill_notification.kis_order_no + " " + fill_notification.ticker +
             (fill_notification.side == OrderSide::BUY ? " BUY " : " SELL ") +
             std::to_string(fill_notification.filled_quantity) + "주 @" +
             std::to_string(static_cast<int>(fill_notification.filled_price)) + extra);
    on_fill_(fill_notification);
}
