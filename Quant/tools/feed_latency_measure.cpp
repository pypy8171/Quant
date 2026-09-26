// tools/feed_latency_measure.cpp
// 실 KIS 실시간 시세 수신·처리 지연 측정 — 멀티세션 샤딩(300+종목 확장)
//
// 목적: bench_feed_ingest(합성 부하, TCP loopback)의 자매 실증. 실제 KIS WebSocket
//   실시간 시세를 다수 구독해, 실데이터가 파이프라인을 통과함과 수신 콜백→주문 결정까지의
//   내부 처리 지연을 라이브로 잰다.
//
//   왜 멀티세션인가: KIS 실시간 WS는 app_key당 1세션·약 41건 등록 상한이다. 호가+체결
//     동시 구독이면 ~20종목, 체결전용(H0STCNT0만)이면 ~40종목이 세션 한계다. 300+종목은
//     app_key를 여러 개 발급해 세션 N개를 병렬로(샤딩) 돌려 결과를 합친다. 300종목
//     체결전용이면 ~8세션.
//
//   RingBuffer는 SPSC(단일 생산자/소비자)다. WS 세션당 수신 스레드가 하나이므로 세션당
//     큐·소비자를 1:1로 두면 SPSC를 지킨다. 지연 표본은 세션별로 모아 종료 후 병합한다.
//
//   측정 범위 — 무료 OpenAPI 실시간 시세에는 µs 해상도의 거래소 원천 타임스탬프가 없어
//     (체결시각은 초/HHMMSS 단위) "거래소→KIS→우리" 물리 wire 지연은 이 경로로 측정 불가다.
//     이 점검가 재는 것은 (1) 수신 콜백→주문 결정까지의 내부 처리 지연과 (2) 다수 구독의
//     실제 메시지 rate·집계 처리량이다. wire 지연은 bench_feed_ingest가 담당(합성).
//
// 실행 (반드시 장 중 09:00–15:30 KST — 장외에는 틱이 없어 샘플 0):
//   feed_latency_measure --sessions creds.json --universe universe_full.json --count 300
//   feed_latency_measure --configs a.json,b.json --count 80 --trade-only 1
//   feed_latency_measure [config.json] --symbols "005930,000660,..."   (단일세션 하위호환)
//
//   자격증명(세션) 소스 — 아래 중 하나:
//     --sessions <path>  : JSON 배열 [{app_key,app_secret,is_paper?,hts_id?}, ...]
//                          또는 {"sessions":[...]}
//     --configs a,b,...  : 각 파일의 "kis" 블록을 세션 하나로(쉼표 구분)
//     <config.json>      : 위치 인자 하나 = 단일 세션(기존 동작)
//
//   종목 소스 — 우선순위: --symbols > --universe > 내장 기본 15종목
//     --symbols "A,B,.." : 명시 리스트
//     --universe <path>  : universe_full.json 의 codes 배열 로드
//   수량 조절:
//     --count N          : 실제 구독할 종목 수 상한(0=전부). 세션 용량을 넘으면 잘라내고 경고.
//     --per-session K    : 세션당 종목 상한(기본: 체결전용 40 / 호가+체결 20)
//     --trade-only 0|1   : 1(기본)=체결만(H0STCNT0, 밀도↑), 0=호가+체결
//     --duration SEC     : 수신 시간(기본 60)

#include "api/KisClient.h"
#include "api/KisWebSocket.h"
#include "core/RingBuffer.h"
#include "core/Types.h"
#include "utils/Logger.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
// KisWebSocket.h가 windows.h를 끌어오던 때의 조건(LEAN_AND_MEAN·NOMINMAX·ERROR 해제)을 여기서 직접 맞춘다. [why D-049]
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef ERROR
#undef ERROR // wingdi.h — LogLevel::ERROR와 부딪힌다
#endif
#endif

using steady_clock = std::chrono::steady_clock;
static inline int64_t now_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

// 수신 콜백 → 소비자로 넘기는 경량 레코드 (recv 시각만 필요).
struct SampleMessage
{
    int64_t recv_ts_ns;
    char    type; // 'O'=orderbook, 'T'=trade
};

struct PercentileSummary
{
    int64_t p50 = 0, p99 = 0, p999 = 0, max_value = 0;
    size_t  count = 0;
};
static PercentileSummary percentiles(std::vector<int64_t>& values)
{
    PercentileSummary percentiles;
    percentiles.count = values.size();

    if (values.empty())
    {
        return percentiles;
    }

    std::sort(values.begin(), values.end());
    auto at = [&](double price)
    {
        return values[static_cast<size_t>(price * (values.size() - 1))];
    };
    percentiles.p50 = at(0.50);
    percentiles.p99 = at(0.99);
    percentiles.p999 = at(0.999);
    percentiles.max_value = values.back();
    return percentiles;
}

static std::string format_ns(int64_t count)
{
    char byte_value[32];

    if (count < 1000)
    {
        std::snprintf(byte_value, sizeof(byte_value), "%lld ns", static_cast<long long>(count));
    }
    else if (count < 1'000'000)
    {
        std::snprintf(byte_value, sizeof(byte_value), "%.2f us", count / 1000.0);
    }
    else
    {
        std::snprintf(byte_value, sizeof(byte_value), "%.2f ms", count / 1'000'000.0);
    }

    return std::string(byte_value);
}

static std::string argument_string(int argc, char** argv, const char* key, const std::string& default_value)
{
    for (int index = 1; index + 1 < argc; ++index)
    {
        if (std::strcmp(argv[index], key) == 0)
        {
            return argv[index + 1];
        }
    }

    return default_value;
}

static int64_t argument_int64(int argc, char** argv, const char* key, int64_t default_value)
{
    std::string text = argument_string(argc, argv, key, "");
    return text.empty() ? default_value : std::atoll(text.c_str());
}

static std::vector<std::string> split_csv(const std::string& text)
{
    std::vector<std::string> out;
    std::stringstream stream(text);
    std::string token;

    while (std::getline(stream, token, ','))
    {
        // 앞뒤 공백 제거
        size_t first_value = token.find_first_not_of(" \t\r\n");
        size_t bit_value = token.find_last_not_of(" \t\r\n");

        if (first_value != std::string::npos)
        {
            out.push_back(token.substr(first_value, bit_value - first_value + 1));
        }
    }

    return out;
}

// kis 블록(app_key 등)을 담은 JSON 오브젝트 → KisConfig
static KisConfig kis_config_from_kis_object(const nlohmann::json& node)
{
    KisConfig kis_config;
    kis_config.app_key      = node.at("app_key").get<std::string>();
    kis_config.app_secret   = node.at("app_secret").get<std::string>();
    kis_config.account_no   = node.value("account_no", "");
    kis_config.account_type = node.value("account_type", "01");
    kis_config.hts_id       = node.value("hts_id", "");
    kis_config.is_paper     = node.value("is_paper", true);
    return kis_config;
}

// 세션 하나: 자격증명 + 담당 종목 + 큐 + WS + 소비자 스레드 + 표본.
struct Session
{
    KisConfig                          kis_config;
    std::vector<std::string>           symbols;
    std::unique_ptr<RingBuffer<SampleMessage>> queue;
    std::unique_ptr<KisWebSocket>      websocket;
    std::thread                        consumer;
    std::vector<int64_t>               latencies;      // recv→decision (세션 전용, race 없음)
    std::atomic<uint64_t>              order_book{0}, trade{0}, dropped{0}, decided{0};
    bool                               connected = false;
};

int main(int argc, char** argv)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    const int  duration    = static_cast<int>(argument_int64(argc, argv, "--duration", 60));
    const bool trade_only  = argument_int64(argc, argv, "--trade-only", 1) != 0;
    const int  count_cap   = static_cast<int>(argument_int64(argc, argv, "--count", 0)); // 0 = 전부
    const int  per_default = trade_only ? 40 : 20;
    const int  per_session = static_cast<int>(argument_int64(argc, argv, "--per-session", per_default));
    const std::string symbol_csv   = argument_string(argc, argv, "--symbols", "");
    const std::string universe  = argument_string(argc, argv, "--universe", "");
    const std::string sessions_path = argument_string(argc, argv, "--sessions", "");
    const std::string configs_csv   = argument_string(argc, argv, "--configs", "");

    // ---- 1) 자격증명(세션) 로드 ----
    std::vector<KisConfig> creds;

    try
    {
        if (!sessions_path.empty())
        {
            std::ifstream file(sessions_path);

            if (!file)
            {
                std::printf("[measure] --sessions 열기 실패: %s\n", sessions_path.c_str());
                return 1;
            }

            nlohmann::json document; file >> document;
            const nlohmann::json& array = document.is_array() ? document : document.at("sessions");

            for (const auto& element : array)
            {
                creds.push_back(kis_config_from_kis_object(element));
            }
        }
        else if (!configs_csv.empty())
        {
            for (const auto& path : split_csv(configs_csv))
            {
                std::ifstream file(path);

                if (!file)
                {
                    std::printf("[measure] --configs 항목 열기 실패: %s\n", path.c_str());
                    return 1;
                }

                nlohmann::json document; file >> document;
                creds.push_back(kis_config_from_kis_object(document.at("kis")));
            }
        }
        else
        {
            // 위치 인자 하나 = 단일 config(기존 동작)
            std::string config_path = "config/config_dev_paper.json";

            for (int index = 1; index < argc; ++index)
            {
                if (argv[index][0] == '-')  // 플래그 + 그 값 건너뜀(모든 플래그가 값 1개)
                {
                    ++index;
                    continue;
                }

                config_path = argv[index]; break;
            }

            std::ifstream file(config_path);

            if (!file)
            {
                std::printf("[measure] config 열기 실패: %s\n", config_path.c_str());
                return 1;
            }

            nlohmann::json document; file >> document;
            creds.push_back(kis_config_from_kis_object(document.at("kis")));
        }
    }
    catch (const std::exception& exception)
    {
        std::printf("[measure] 자격증명 파싱 실패: %s\n", exception.what());
        return 1;
    }

    if (creds.empty())
    {
        std::printf("[measure] 세션 자격증명이 없다.\n");
        return 1;
    }

    // ---- 2) 종목 리스트 로드 ----
    std::vector<std::string> all_symbols = split_csv(symbol_csv);

    if (all_symbols.empty() && !universe.empty())
    {
        try
        {
            std::ifstream file(universe);

            if (!file)
            {
                std::printf("[measure] --universe 열기 실패: %s\n", universe.c_str());
                return 1;
            }

            nlohmann::json document; file >> document;

            if (document.contains("universe") && document.at("universe").is_array())
            {
                // universe_scan.json 스키마: [{"ticker":"005930",...}, ...]
                for (const auto& element : document.at("universe"))
                {
                    all_symbols.push_back(element.at("ticker").get<std::string>());
                }
            }
            else
            {
                // {"codes":[...]} 또는 바로 배열
                const nlohmann::json& codes = document.contains("codes") ? document.at("codes") : document;

                for (const auto& code : codes)
                {
                    all_symbols.push_back(code.get<std::string>());
                }
            }
        }
        catch (const std::exception& exception)
        {
            std::printf("[measure] universe 파싱 실패: %s\n", exception.what());
            return 1;
        }
    }

    if (all_symbols.empty())
    {
        all_symbols = {"005930", "000660", "373220", "207940", "005380", "000270",
                       "005490", "035420", "051910", "006400", "035720", "105560",
                       "055550", "012330", "028260"};
    }

    // ---- 3) 수량 조절 + 세션 용량 대조 ----
    const int sessions_avail = static_cast<int>(creds.size());
    const int capacity       = sessions_avail * per_session; // 라이브로 받을 수 있는 상한
    int wanted_count = static_cast<int>(all_symbols.size());

    if (count_cap > 0)
    {
        wanted_count = std::min(wanted_count, count_cap);
    }

    int use = std::min(wanted_count, capacity);

    if (use < wanted_count)
    {
        int need_sessions = (wanted_count + per_session - 1) / per_session;
        std::printf("[measure] ⚠ 요청 %d종목 > 세션 용량 %d (%d세션 × %d/세션). %d종목만 구독.\n",
                    wanted_count, capacity, sessions_avail, per_session, use);
        std::printf("        %d종목을 라이브로 받으려면 app_key %d개(현재 %d개)가 필요하다.\n",
                    wanted_count, need_sessions, sessions_avail);
    }

    all_symbols.resize(use);

    const int used_sessions = (use + per_session - 1) / per_session;
    const int used_sessions_clamped = std::max(1, std::min(used_sessions, sessions_avail));

    std::printf("=== KIS 실시간 시세 멀티세션 점검 ===\n");
    std::printf("세션(app_key)   : %d개 발견, %d개 사용\n", sessions_avail, used_sessions_clamped);
    std::printf("구독 밀도       : %s (%d등록/종목, 세션당 최대 %d종목)\n",
                trade_only ? "체결전용 H0STCNT0" : "호가+체결", trade_only ? 1 : 2, per_session);
    std::printf("종목            : %d개 구독 (요청 %d)\n", use, wanted_count);
    std::printf("duration        : %d sec\n", duration);
    std::printf("NOTE: 내부(수신콜백→주문결정) 지연 + 관측 실 msg rate만 측정. 거래소 wire\n");
    std::printf("      지연은 무료 API에 µs 원천 ts가 없어 측정 불가(정직 경계).\n");
    std::printf("      장외(09:00–15:30 KST 밖)에는 틱이 없어 샘플 0.\n\n");

    // ---- 4) 종목을 세션에 분배(연속 청크) + 세션 구성 ----
    std::vector<std::unique_ptr<Session>> sessions;

    for (int used_session_index = 0; used_session_index < used_sessions_clamped; ++used_session_index)
    {
        auto session = std::make_unique<Session>();
        session->kis_config = creds[used_session_index];
        session->queue  = std::make_unique<RingBuffer<SampleMessage>>(1u << 16);
        int begin = used_session_index * per_session;
        int end   = std::min(static_cast<int>(all_symbols.size()), begin + per_session);

        for (int end_index = begin; end_index < end; ++end_index)
        {
            session->symbols.push_back(all_symbols[end_index]);
        }

        sessions.push_back(std::move(session));
    }

    std::atomic<bool> stop{false};

    // 세션별 소비자 스레드 — 각 큐는 SPSC(WS 수신 스레드 1 : 소비자 1).
    for (auto& session_pointer : sessions)
    {
        Session* session = session_pointer.get();
        session->latencies.reserve(1u << 21);
        session->consumer = std::thread([session, &stop] {
            while (!stop.load(std::memory_order_relaxed) || !session->queue->empty())
            {
                auto option = session->queue->pop();

                if (!option)
                {
                    continue;
                }

                // 트레이딩 결정 대리 연산(전략 hot path 근사).
                volatile int64_t sink = option->recv_ts_ns ^ 0x5a5a;
                (void)sink;
                const int64_t time_value = now_ns();

                if (session->latencies.size() < session->latencies.capacity())
                {
                    session->latencies.push_back(time_value - option->recv_ts_ns);
                }

                session->decided.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // ---- 5) 세션별 WS 연결 ----
    for (auto& session_pointer : sessions)
    {
        Session* session = session_pointer.get();
        session->websocket = std::make_unique<KisWebSocket>(session->kis_config);
        session->websocket->set_callbacks(
            [session](const OrderBook&) {
                SampleMessage sample_message{now_ns(), 'O'};

                if (session->queue->push(sample_message))
                {
                    session->order_book.fetch_add(1, std::memory_order_relaxed);
                }
                else
                {
                    session->dropped.fetch_add(1, std::memory_order_relaxed);
                }
            },
            [session](const TradeData&) {
                SampleMessage sample_message{now_ns(), 'T'};

                if (session->queue->push(sample_message))
                {
                    session->trade.fetch_add(1, std::memory_order_relaxed);
                }
                else
                {
                    session->dropped.fetch_add(1, std::memory_order_relaxed);
                }
            });
    }

    const int64_t start_time = now_ns();
    int connected_sessions = 0;

    for (size_t session_index = 0; session_index < sessions.size(); ++session_index)
    {
        Session* session = sessions[session_index].get();
        std::vector<WatchSpec> specifications;

        for (const auto& symbol : session->symbols)
        {
            WatchSpec specification; specification.ticker = symbol; specification.market = Market::KR; specification.trade_only = trade_only;
            specifications.push_back(specification);
        }

        std::printf("[measure] 세션 %zu/%zu 연결 시도 (%zu종목)...\n",
                    session_index + 1, sessions.size(), session->symbols.size());

        if (session->websocket->connect(specifications))
        {
            session->connected = true;
            ++connected_sessions;
        }
        else
        {
            std::printf("[measure] 세션 %zu 연결 실패 (approval key/세션/상한 확인).\n", session_index + 1);
        }
    }

    if (connected_sessions == 0)
    {
        std::printf("[measure] 전 세션 연결 실패.\n");
        stop.store(true);

        for (auto& session : sessions)
        {
            if (session->consumer.joinable())
            {
                session->consumer.join();
            }
        }

        return 1;
    }

    std::printf("[measure] %d/%zu 세션 연결. %d초 수신...\n\n", connected_sessions, sessions.size(), duration);

    for (int duration_index = 0; duration_index < duration; ++duration_index)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    for (auto& session : sessions)
    {
        if (session->connected)
        {
            session->websocket->disconnect();
        }
    }

    stop.store(true, std::memory_order_relaxed);

    for (auto& session : sessions)
    {
        if (session->consumer.joinable())
        {
            session->consumer.join();
        }
    }

    const double elapsed = (now_ns() - start_time) / 1e9;

    // ---- 6) 집계 + 표본 병합 ----
    uint64_t order_book_total = 0, trade_total = 0, drop_total = 0, dec_total = 0;
    std::vector<int64_t> latencies;

    for (auto& session : sessions)
    {
        order_book_total   += session->order_book.load();
        trade_total   += session->trade.load();
        drop_total += session->dropped.load();
        dec_total  += session->decided.load();
        latencies.insert(latencies.end(), session->latencies.begin(), session->latencies.end());
    }

    const uint64_t total = order_book_total + trade_total;

    std::printf("=== 수신 결과 (전 세션 집계) ===\n");
    std::printf("elapsed         : %.1f sec\n", elapsed);
    std::printf("세션 연결       : %d/%zu\n", connected_sessions, sessions.size());
    std::printf("orderbook/trade : %llu / %llu (drop %llu)\n",
                static_cast<unsigned long long>(order_book_total), static_cast<unsigned long long>(trade_total),
                static_cast<unsigned long long>(drop_total));
    std::printf("decided         : %llu\n", static_cast<unsigned long long>(dec_total));
    std::printf("관측 msg rate   : %.1f msg/sec (실 라이브, %d종목/%d세션)\n",
                total / (elapsed > 0 ? elapsed : 1), use, connected_sessions);

    if (latencies.empty())
    {
        std::printf("\n[주의] 샘플 0 — 장외이거나 틱 미수신. 장 중(09:00–15:30 KST)에 재실행.\n");
        return 0;
    }

    PercentileSummary percentile_values = percentiles(latencies);
    std::printf("\n=== 내부 지연 (수신콜백 → 주문결정, 실데이터, 전 세션 병합) ===\n");
    std::printf("n=%zu  p50=%s  p99=%s  p999=%s  max=%s\n", percentile_values.count, format_ns(percentile_values.p50).c_str(),
                format_ns(percentile_values.p99).c_str(), format_ns(percentile_values.p999).c_str(), format_ns(percentile_values.max_value).c_str());
    std::printf("\n비교: bench_feed_ingest(합성)의 proc(recv→order)와 같은 구간 — 실데이터로 재확인.\n");
    return 0;
}
