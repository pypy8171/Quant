#pragma once
// 운영단말(ops terminal) ↔ 엔진 TCP 프레이밍. 구현은 Quant/src/ipc/OpsProtocol.cpp, 표준 라이브러리
//  밖 의존은 없다 — 서버(OpsServer)와 단말(콘솔 ops_client·MFC)이 같은 두 파일을 컴파일한다. 소켓·JSON은 여기 없다: 바이트열을
//  프레임으로 자르고 붙이는 일만 한다. [why D-043]
//
//  [wire] 헤더 8바이트, 빅엔디언. 본문은 UTF-8 JSON.
//    0..1  매직 'Q' 'P'
//    2     버전(=1)
//    3     메시지 타입(OpsMsg)
//    4..7  본문 길이 uint32 (0 허용, 상한 kMaxBody)
//  매직·버전·길이 상한을 벗어나면 그 연결은 끊는 것이 규약이다(재동기 시도 없음 — TCP는
//  바이트가 빠지지 않으므로 어긋났다면 상대가 규약을 모르는 것이다).

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace ops
{

constexpr uint8_t  kMagic0   = 'Q';
constexpr uint8_t  kMagic1   = 'P';
constexpr uint8_t  kVersion  = 1;
constexpr size_t   kHeaderLen = 8;
constexpr uint32_t kMaxBody  = 1u << 20; // 1 MiB — 포지션 스냅샷도 수십 KB면 충분하다

// 타입 번호는 한 번 정하면 바꾸지 않는다(단말과 엔진이 따로 배포된다). 0x0_ 세션,
//  0x1_ 조회, 0x2_ 주문, 0x3_ 제어, 0x7F 오류.
enum class OpsMsg : uint8_t
{
    // 이름 규칙 — 요청/응답 쌍은 *_REQ(c→s)/*_ACK(s→c), 서버가 먼저 미는 통보는 *_NTF(s→c).
    HELLO_REQ         = 0x01, // c→s {"token","client"}  첫 프레임이어야 한다
    HELLO_ACK         = 0x02, // s→c {"ok","auth","engine","paper"}
    PING_REQ          = 0x03, // c→s {}
    PING_ACK          = 0x04, // s→c {"ts"}
    STATUS_REQ        = 0x10, // c→s {}
    STATUS_ACK        = 0x11, // s→c {"running","data","signal","order","kill","entry_halt","manual_buy_halt","manual_sell_halt","force_liq","paper","strategies","equity","cash","daily_pnl","position_value","unrealized_pnl"} — 뒤 다섯은 계좌 요약(원). 단말이 1초마다 묻는다
    POSITIONS_REQ     = 0x12, // c→s {}
    POSITIONS_ACK     = 0x13, // s→c {"positions":[{account,ticker,name,qty,avg_price,reserved,last}]} — 키 이름은 와이어 규약이라 약어를 그대로 둔다. reserved: 미체결 매도 음수·매수 양수, last: 최근 체결가(틱 없으면 0)
    POSITIONS_NTF     = 0x14, // s→c 본문은 POSITIONS_ACK와 같다. HELLO_ACK 바로 뒤 1회, 이후 1초마다 보고 바뀌었을 때만 민다
    ORDER_REQ         = 0x20, // c→s {"cid","ticker","side","qty","price","ref_price","account"}
    ORDER_ACK         = 0x21, // s→c {"cid","accepted","msg"} — 인테이크 적재 여부(게이트 통과 아님)
    ORDER_RESULT_NTF  = 0x22, // s→c {"cid","order_id","odno","strategy","ticker","side","qty","price","ok","msg"} — 게이트·브로커 결과. 전략 주문도 같은 채널로 온다
    FILL_NTF          = 0x23, // s→c {"odno","ticker","side","qty","price","time"}
    KILL_REQ          = 0x30, // c→s {}
    KILL_ACK          = 0x31, // s→c {"ok"}
    HALT_REQ          = 0x32, // c→s {"side","on"} — 수동 정지 on/off. side는 "BUY"(신규 진입, 없으면 이것)·"SELL"(전략 매도). kill과 달리 되돌릴 수 있다 [why D-091, D-095]
    HALT_ACK          = 0x33, // s→c {"ok","manual_buy_halt","manual_sell_halt"}
    SHUTDOWN_REQ      = 0x34, // c→s {"who"} — 배포 교체용 곱게 내리기. KILL_REQ와 달리 킬스위치를 켜지 않고 표지 파일도 안 쓴다 [why D-114]
    SHUTDOWN_ACK      = 0x35, // s→c {"ok","msg"}
    ERROR_NTF         = 0x7F, // s→c {"msg"}
};

const char* message_name(uint8_t message_type);

struct Frame
{
    uint8_t     type = 0;
    std::string body;
};

// 헤더+본문을 한 버퍼로. 본문이 상한을 넘으면 빈 벡터(호출자가 보내지 않는다).
std::vector<uint8_t> encode(OpsMsg type, const std::string& body);

// 수신 바이트를 누적해 완성된 프레임을 꺼내는 조립기. recv()가 헤더 중간·본문 중간에서
//  끊겨 돌아오는 것이 TCP의 정상 동작이라, 호출자는 받은 만큼 feed()하고 next()가 false를
//  줄 때까지 꺼낸다. 규약 위반은 bad()로 굳고 그 뒤는 무엇을 넣어도 프레임이 나오지 않는다.
class FrameReader
{
public:
    void feed(const uint8_t* data, size_t length);

    // 완성된 프레임이 있으면 out에 채우고 true. 없거나 bad()면 false.
    bool next(Frame& out);

    bool   bad() const
    {
        return bad_;
    }

    size_t pending() const
    {
        return buffer_.size();
    }

private:
    std::vector<uint8_t> buffer_;
    bool                 bad_ = false;
};

} // namespace ops
