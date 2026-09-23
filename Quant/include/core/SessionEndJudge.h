// 마감 자기 종료 판정 — 마지막 매매 창이 닫힌 뒤 "지금 프로세스를 끝내도 되는가"만 답한다. 파일 쓰기·로그·
// request_shutdown은 Engine(control_thread)이 하고, 여기는 시각(자정부터의 초)과 "주문 큐가 비었는가"만 받는다 —
// 유예·강제 문턱을 시계 없이 전수 시험하려고 뗐다. control_thread 전용이라 동기화는 없다. [why D-098]
//
//  그전에는 엔진이 스스로 내려가는 길이 없어 감시견 -Until(마감+5분) 강제 종료에 기댔고, 감시견은 그 종료와 크래시를
//  가르지 못했다. 판정이 서면 Engine이 _private/state/session_done[_<instance>]_<날짜>를 쓰고(D-122), 감시견은 그 파일이 있으면
//  재기동하지 않는다.
#pragma once

namespace session_end
{

struct Config
{
    int close_min       = 0;   // 마지막 매매 창이 닫히는 분(자정부터). 0이면 판정 없음 — 리플레이·테스트 기본
    int grace_sec       = 120; // 창이 닫힌 뒤 이만큼은 기다린다 — 마감 청산 주문·체결통보가 도는 시간
    int drain_limit_sec = 600; // 유예 뒤에도 큐가 안 비면 이 시간까지만 더 기다렸다가 강제로 끝낸다
};

class Judge
{
public:
    // 한 주기의 답. kClosed는 창이 닫힌 첫 관찰(로그 한 줄 자리), kShutdown·kShutdownForced는 종료를 시작할 자리.
    //  종료를 한 번 답한 뒤에는 다시 답하지 않는다(엔진이 이미 내려가는 중).
    enum class Step
    {
        kNone,           // 창이 안 닫혔거나(개장 전·장중), 유예 대기 중이거나, 너무 늦은 기동
        kClosed,         // 창이 방금 닫혔다 — 유예 시작
        kShutdown,       // 유예가 끝났고 주문 큐가 비었다
        kShutdownForced  // 유예 + 배출 한도까지 지났는데도 큐가 안 비었다
    };

    explicit Judge(Config config = {}) : config_(config) {}

    const Config& config() const { return config_; }

    // now_sec_of_day: KST 자정부터의 초. orders_pending: 주문 큐에 아직 꺼내지 않은 신호가 있는가.
    //  창이 닫힌 뒤 grace+drain_limit보다 늦게 처음 관찰되면(밤에 손으로 띄운 TRADE 기동) 판정하지 않는다 —
    //  그런 기동은 예전처럼 사용자가 끈다. 감시견이 창 안에서 재기동한 엔진은 유예 뒤 정상으로 내려간다.
    Step observe(int now_sec_of_day, bool orders_pending);

private:
    Config config_;
    bool   closed_seen_ = false;
    bool   done_        = false;
};

} // namespace session_end
