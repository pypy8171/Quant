// HEALTH 한 건에 싣는 엔진 내부 수치. 데이터 스레드가 주기마다 채워 ZMQ 발행기와 DB 적재기(DbManager)에
//  같이 넘긴다 — 둘 다 이 값을 복사해 제 스레드에서 글자로 바꾼다. ZmqBridge.h는 HAS_ZMQ일 때만 보이므로
//  ZMQ 없이 빌드해도 DB로 넣을 수 있게 여기 따로 둔다. [why D-071] [why D-154]
//  큐 고수위와 지연 분위수는 기동 후 누적이라 줄지 않는다 — 구간 값이 필요하면 읽는 쪽이 직전 행과 뺀다.
//  표본이 없는 지연은 -1. trivially copyable — 적재 큐에 memcpy로 들어간다.
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

struct HealthSnapshot
{
    uint64_t data_count             = 0;
    uint64_t signal_count           = 0;
    uint64_t order_count            = 0;
    uint64_t shard_high_water       = 0;   // 샤드 셀 가운데 가장 높았던 값
    uint64_t shard_capacity         = 0;
    uint64_t shard_out_size         = 0;   // 지금 쌓여 있는 깊이(누적 최대가 아니다)
    uint64_t shard_out_capacity     = 0;
    uint64_t order_queue_high_water = 0;
    uint64_t order_queue_capacity   = 0;
    uint64_t fill_queue_high_water  = 0;
    uint64_t fill_queue_capacity    = 0;
    uint64_t shard_dropped          = 0;
    uint64_t order_dropped          = 0;
    uint64_t order_stale            = 0;
    uint64_t fill_dropped           = 0;
    uint64_t latency_samples        = 0;
    int64_t  tick_to_signal_p50_us  = -1;
    int64_t  tick_to_signal_p99_us  = -1;
    int64_t  signal_to_pop_p50_us   = -1;
    int64_t  signal_to_pop_p99_us   = -1;
    int64_t  pop_to_done_p50_us     = -1;
    int64_t  pop_to_done_p99_us     = -1;
    int64_t  total_p50_us           = -1;
    int64_t  total_p99_us           = -1;

    // ZMQ 발행기가 버린 메시지 수(원인별). 발행기가 있을 때만 채운다 — 없으면 has_publish_drops가 false고
    //  DB의 drop_* 열은 비운다(NULL). 0으로 적으면 "버린 것이 없었다"는 없는 사실이 된다. [why D-125]
    bool     has_publish_drops    = false;
    uint64_t publish_dropped      = 0;   // 아래 넷의 합(health.drop_cnt)
    uint64_t drop_socket_full     = 0;
    uint64_t drop_socket_error    = 0;
    uint64_t drop_send_queue_full = 0;
    uint64_t drop_trade_ring_full = 0;

    // 직전 HEALTH 이후에 들어온 표본만의 구간 분위수. 누적 분위수는 한 번 튀면 안 내려와
    //  "언제 느려졌나"를 못 본다 — 그래서 같은 구간을 두 벌 싣는다. 표본이 없으면 -1. [why D-071]
    struct IntervalSegment
    {
        std::string_view name;        // health 열 이름 앞머리(<name>_p50_interval_us). [inv] 정적 문자열만
        int64_t          p50_us = -1;
        int64_t          p99_us = -1;
    };

    // [inv] name이 빈 칸은 안 싣는다 — 엔진이 구간 수만큼만 채운다.
    std::array<IntervalSegment, 16> interval_segments{};
    uint64_t                        interval_samples = 0; // 이번 구간에 들어온 주문 표본 수
};
