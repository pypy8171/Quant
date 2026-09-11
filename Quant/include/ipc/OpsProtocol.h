#pragma once
// 운영단말(ops terminal) ↔ 엔진 TCP 프레이밍. 헤더 전용·의존성 0 — 서버(OpsServer)와
//  단말(콘솔 ops_client·MFC)이 같은 파일을 컴파일한다. 소켓·JSON은 여기 없다: 바이트열을
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
    HELLO        = 0x01, // c→s {"token","client"}  첫 프레임이어야 한다
    WELCOME      = 0x02, // s→c {"ok","auth","engine","paper"}
    PING         = 0x03, // c→s {}
    PONG         = 0x04, // s→c {"ts"}
    STATUS_REQ   = 0x10, // c→s {}
    STATUS       = 0x11, // s→c {"running","data","signal","order","kill","entry_halt","force_liq"}
    POS_REQ      = 0x12, // c→s {}
    POSITIONS    = 0x13, // s→c {"positions":[{account,ticker,name,qty,avg_price,reserved}]} — 변경 시 push도 한다
    ORDER_REQ    = 0x20, // c→s {"cid","ticker","side","qty","price","ref_price"}
    ORDER_ACK    = 0x21, // s→c {"cid","accepted","msg"} — 인테이크 적재 여부(게이트 통과 아님)
    ORDER_RESULT = 0x22, // s→c {"cid","order_id","strategy","ticker","side","qty","ok","msg"} — 게이트·브로커 결과
    FILL         = 0x23, // s→c {"odno","ticker","side","qty","price","time"}
    KILL         = 0x30, // c→s {}
    KILL_ACK     = 0x31, // s→c {"ok"}
    ERROR_MSG    = 0x7F, // s→c {"msg"}
};

inline const char* msg_name(uint8_t t)
{
    switch (static_cast<OpsMsg>(t))
    {
        case OpsMsg::HELLO:        return "HELLO";
        case OpsMsg::WELCOME:      return "WELCOME";
        case OpsMsg::PING:         return "PING";
        case OpsMsg::PONG:         return "PONG";
        case OpsMsg::STATUS_REQ:   return "STATUS_REQ";
        case OpsMsg::STATUS:       return "STATUS";
        case OpsMsg::POS_REQ:      return "POS_REQ";
        case OpsMsg::POSITIONS:    return "POSITIONS";
        case OpsMsg::ORDER_REQ:    return "ORDER_REQ";
        case OpsMsg::ORDER_ACK:    return "ORDER_ACK";
        case OpsMsg::ORDER_RESULT: return "ORDER_RESULT";
        case OpsMsg::FILL:         return "FILL";
        case OpsMsg::KILL:         return "KILL";
        case OpsMsg::KILL_ACK:     return "KILL_ACK";
        case OpsMsg::ERROR_MSG:    return "ERROR";
    }

    return "?";
}

struct Frame
{
    uint8_t     type = 0;
    std::string body;
};

// 헤더+본문을 한 버퍼로. 본문이 상한을 넘으면 빈 벡터(호출자가 보내지 않는다).
inline std::vector<uint8_t> encode(OpsMsg type, const std::string& body)
{
    if (body.size() > kMaxBody)
    {
        return {};
    }

    const uint32_t n = static_cast<uint32_t>(body.size());
    std::vector<uint8_t> out;
    out.reserve(kHeaderLen + n);
    out.push_back(kMagic0);
    out.push_back(kMagic1);
    out.push_back(kVersion);
    out.push_back(static_cast<uint8_t>(type));
    out.push_back(static_cast<uint8_t>((n >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((n >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((n >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(n & 0xFF));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

// 수신 바이트를 누적해 완성된 프레임을 꺼내는 조립기. recv()가 헤더 중간·본문 중간에서
//  끊겨 돌아오는 것이 TCP의 정상 동작이라, 호출자는 받은 만큼 feed()하고 next()가 false를
//  줄 때까지 꺼낸다. 규약 위반은 bad()로 굳고 그 뒤는 무엇을 넣어도 프레임이 나오지 않는다.
class FrameReader
{
public:
    void feed(const uint8_t* data, size_t len)
    {
        if (bad_)
        {
            return;
        }

        buf_.insert(buf_.end(), data, data + len);
    }

    // 완성된 프레임이 있으면 out에 채우고 true. 없거나 bad()면 false.
    bool next(Frame& out)
    {
        if (bad_ || buf_.size() < kHeaderLen)
        {
            return false;
        }

        if (buf_[0] != kMagic0 || buf_[1] != kMagic1 || buf_[2] != kVersion)
        {
            bad_ = true;
            return false;
        }

        const uint32_t n = (static_cast<uint32_t>(buf_[4]) << 24) | (static_cast<uint32_t>(buf_[5]) << 16) |
                           (static_cast<uint32_t>(buf_[6]) << 8) | static_cast<uint32_t>(buf_[7]);

        if (n > kMaxBody)
        {
            bad_ = true;
            return false;
        }

        if (buf_.size() < kHeaderLen + n)
        {
            return false;
        }

        out.type = buf_[3];
        out.body.assign(reinterpret_cast<const char*>(buf_.data() + kHeaderLen), n);
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(kHeaderLen + n));
        return true;
    }

    bool   bad() const { return bad_; }
    size_t pending() const { return buf_.size(); }

private:
    std::vector<uint8_t> buf_;
    bool                 bad_ = false;
};

} // namespace ops
