#pragma once
#include "core/MpscQueue.h"
#include "ipc/OpsServer.h"
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>

// ─────────────────────────────────────────────────────────────────────────────
// Engine의 운영 채널(운영단말 서버·수동주문 인테이크)과 ZMQ 포트 설정.
//  Engine.h에서 떼어 낸 것이다 — Engine 안에서는 같은 이름(Engine::ZmqPorts 등)으로 쓴다.
//  관련 결정: D-043·D-114.
// ─────────────────────────────────────────────────────────────────────────────

namespace engine
{

// 운영단말 서버와 수동주문 인테이크. 서버 스레드가 push, order_thread가 pop해
//  OrderSignal(strategy_id="MANUAL")로 바꿔 게이트·장부를 그대로 지난다. 소켓 스레드가 발주 사슬에
//  직접 들어가지 않는 것은 FORCE_LIQ와 같은 이유고, 꺼내는 쪽을 주문 쪽에 둔 것은 전략 프로세스가
//  멎어도 사람이 손으로 낼 수 있어야 해서다. [why D-043][why D-114]
struct OpsChannel
{
    std::unique_ptr<OpsServer>      server;
    static constexpr size_t         kManualInboxCapacity = 256; // 단말 수동주문은 사람이 치는 속도라 이만큼이면 남는다
    MpscQueue<OpsOrderReq>          manual_inbox{kManualInboxCapacity};
    std::string                     bind_address;
    int                             port = 0;
    std::string                     token;
    std::mutex                      manual_client_id_mutex;
    // 재전송 중복 차단(세션 내). cid는 운영단말이 준 문자열 그 자체라 문자열 집합 — 해시로 줄이면 충돌이
    //  정상 주문을 "중복"으로 거부할 수 있고, 세션당 수십 건이라 얻는 것도 없다.
    std::unordered_set<std::string> manual_cids;
};

// ZMQ 포트 넷. 한 기계에 엔진이 둘이면 뒤에 뜬 쪽이 bind에 실패하므로 설정으로 뺐다. 역할마다 발행
//  포트가 하나씩이고(주문·시세·전략) 제어(REP)는 주문 쪽에만 있다 — 한 포트로 모으면 그 자리가
//  죽을 때 셋이 같이 멎어 프로세스를 가른 뜻이 없어진다. [why D-114]
struct ZmqPorts
{
    int order_pub    = 5555; // config `zmq_pub_port`
    int order_rep    = 5556; // config `zmq_rep_port`
    int feed_pub     = 5557; // config `zmq_feed_pub_port`
    int strategy_pub = 5558; // config `zmq_strategy_pub_port`
};

} // namespace engine
