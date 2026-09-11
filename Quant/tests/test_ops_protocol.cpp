// 운영단말 프레이밍(ipc/OpsProtocol.h) 단위 테스트. 한 바이트씩 오는 부분 수신, 한 recv에 여러
// 프레임, 빈 본문, 매직·버전·길이 상한 위반을 고정한다. 헤더 전용이라 소켓 없이 돈다. 관련 결정: D-043.
#include "ipc/OpsProtocol.h"
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using ops::Frame;
using ops::FrameReader;
using ops::OpsMsg;

static void t_roundtrip_whole()
{
    auto bytes = ops::encode(OpsMsg::ORDER_REQ, "{\"cid\":\"a1\"}");
    assert(bytes.size() == ops::kHeaderLen + 12);
    assert(bytes[0] == 'Q' && bytes[1] == 'P' && bytes[2] == ops::kVersion);
    assert(bytes[3] == static_cast<uint8_t>(OpsMsg::ORDER_REQ));
    assert(bytes[7] == 12 && bytes[4] == 0);

    FrameReader r;
    r.feed(bytes.data(), bytes.size());
    Frame f;
    assert(r.next(f));
    assert(f.type == static_cast<uint8_t>(OpsMsg::ORDER_REQ));
    assert(f.body == "{\"cid\":\"a1\"}");
    assert(!r.next(f));
    assert(r.pending() == 0);
}

// TCP는 recv 경계를 보장하지 않는다 — 헤더 중간·본문 중간에서 끊겨도 프레임이 하나만 나와야 한다.
static void t_byte_by_byte()
{
    auto bytes = ops::encode(OpsMsg::STATUS, "{\"running\":true}");
    FrameReader r;
    Frame f;
    int got = 0;

    for (size_t i = 0; i < bytes.size(); ++i)
    {
        r.feed(&bytes[i], 1);

        while (r.next(f))
        {
            ++got;
            assert(f.body == "{\"running\":true}");
        }

        if (i + 1 < bytes.size())
        {
            assert(got == 0);
        }
    }

    assert(got == 1);
}

// 한 recv에 프레임 두 개 반 — 둘은 나오고 나머지 반은 다음 feed까지 기다린다.
static void t_two_and_half()
{
    auto a = ops::encode(OpsMsg::PING, "");
    auto b = ops::encode(OpsMsg::POS_REQ, "{}");
    auto c = ops::encode(OpsMsg::KILL, "{\"x\":1}");
    std::vector<uint8_t> wire;
    wire.insert(wire.end(), a.begin(), a.end());
    wire.insert(wire.end(), b.begin(), b.end());
    wire.insert(wire.end(), c.begin(), c.begin() + 5);

    FrameReader r;
    r.feed(wire.data(), wire.size());
    Frame f;
    assert(r.next(f) && f.type == static_cast<uint8_t>(OpsMsg::PING) && f.body.empty());
    assert(r.next(f) && f.type == static_cast<uint8_t>(OpsMsg::POS_REQ) && f.body == "{}");
    assert(!r.next(f));
    assert(r.pending() == 5);

    r.feed(c.data() + 5, c.size() - 5);
    assert(r.next(f) && f.type == static_cast<uint8_t>(OpsMsg::KILL) && f.body == "{\"x\":1}");
    assert(!r.next(f) && r.pending() == 0);
}

static void t_bad_magic_sticks()
{
    auto bytes = ops::encode(OpsMsg::PING, "");
    bytes[1]   = 'X';
    FrameReader r;
    r.feed(bytes.data(), bytes.size());
    Frame f;
    assert(!r.next(f));
    assert(r.bad());

    // 이후 정상 프레임을 넣어도 살아나지 않는다 — 호출자가 끊어야 한다
    auto ok = ops::encode(OpsMsg::PING, "");
    r.feed(ok.data(), ok.size());
    assert(!r.next(f));
}

static void t_bad_version()
{
    auto bytes = ops::encode(OpsMsg::PING, "");
    bytes[2]   = 2;
    FrameReader r;
    r.feed(bytes.data(), bytes.size());
    Frame f;
    assert(!r.next(f) && r.bad());
}

static void t_oversize_rejected()
{
    // 인코더: 상한 초과 본문은 빈 벡터
    std::string big(ops::kMaxBody + 1, 'x');
    assert(ops::encode(OpsMsg::POSITIONS, big).empty());
    assert(!ops::encode(OpsMsg::POSITIONS, std::string(ops::kMaxBody, 'x')).empty());

    // 디코더: 길이 필드만 상한 넘게 조작 — 본문을 기다리지 않고 즉시 bad
    uint8_t hdr[8] = {'Q', 'P', ops::kVersion, 0x03, 0x00, 0x10, 0x00, 0x01};
    FrameReader r;
    r.feed(hdr, 8);
    Frame f;
    assert(!r.next(f) && r.bad());
}

static void t_names()
{
    assert(std::string(ops::msg_name(0x20)) == "ORDER_REQ");
    assert(std::string(ops::msg_name(0x7F)) == "ERROR");
    assert(std::string(ops::msg_name(0x55)) == "?");
}

int main()
{
    t_roundtrip_whole();
    t_byte_by_byte();
    t_two_and_half();
    t_bad_magic_sticks();
    t_bad_version();
    t_oversize_rejected();
    t_names();
    std::cout << "test_ops_protocol: 7/7 PASS\n";
    return 0;
}
