// 엔진 DB 적재 처리량 벤치 — 체결 N건을 한꺼번에 넣고 전부 ticks 표에 들어갈 때까지 걸린 시간을 잰다.
//  큐를 N건보다 크게 잡아 버리는 행이 없게 하므로, 잰 값은 적재 워커가 COPY로 넣는 속도다.
//  시험 종목은 ZZB로 시작하고 끝나면 지운다. 비밀번호는 환경변수 TSDB_PASSWORD. [why D-148]
// 빌드: cmake --build <directory> --target bench_db_manager
// 실행: bench_db_manager [행 수=1000000] [적재 워커 수=2] [묶음 행 수=5000]
#include "ipc/DbManager.h"

#include <libpq-fe.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace
{
constexpr int       kSymbolCount = 2700;
constexpr long long kDefaultRows = 1'000'000;
constexpr size_t    kQueueSlack = 1024; // 종목이 워커에 고르게 안 나뉘어도 넘치지 않게 더 잡는 칸
constexpr int       kPriceSteps = 500;

void delete_bench_rows(const db::DbConfig& config, const std::string& password)
{
    const std::string  host = db::resolve_host(config.host);
    const std::string  port = std::to_string(config.port);
    const char* const  keywords[] = {"host", "port", "dbname", "user", "password", nullptr};
    const char* const  values[] = {host.c_str(), port.c_str(), config.dbname.c_str(), config.user.c_str(),
                                   password.c_str(), nullptr};
    PGconn*            connection = PQconnectdbParams(keywords, values, 0);

    if (PQstatus(connection) == CONNECTION_OK)
    {
        PGresult* result = PQexec(connection, "DELETE FROM ticks WHERE ticker LIKE 'ZZB%'");
        std::printf("시험 행 지움: %s행\n", PQcmdTuples(result));
        PQclear(result);
    }
    else
    {
        std::printf("시험 행을 못 지웠다 — %s", PQerrorMessage(connection));
    }

    PQfinish(connection);
}
} // namespace

int main(int argument_count, char** arguments)
{
    const char* password = std::getenv("TSDB_PASSWORD");

    if (password == nullptr || password[0] == '\0')
    {
        std::printf("TSDB_PASSWORD가 없다\n");
        return 1;
    }

    const long long rows = argument_count > 1 ? std::atoll(arguments[1]) : kDefaultRows;
    db::DbConfig config;
    config.enabled = true;
    config.tick_workers = argument_count > 2 ? static_cast<unsigned>(std::atoi(arguments[2])) : 2u;
    config.batch_rows = argument_count > 3 ? static_cast<size_t>(std::atoll(arguments[3])) : 5000u;
    config.tick_queue_capacity = static_cast<size_t>(rows / config.tick_workers) + kQueueSlack;

    std::string tickers[kSymbolCount];

    for (int index = 0; index < kSymbolCount; ++index)
    {
        char name[8];
        std::snprintf(name, sizeof(name), "ZZB%04d", index);
        tickers[index] = name;
    }

    long long settled = 0;
    double    offer_seconds = 0.0;
    double    total_seconds = 0.0;
    {
        db::DbManager manager(config);

        if (!manager.ok())
        {
            return 1;
        }

        // 연결이 먼저 서게 한 행을 넣고 기다린다 — 접속 시간은 처리량에서 뺀다.
        TradeData warmup;
        warmup.ticker = tickers[0].c_str();
        warmup.symbol_id = 1;
        warmup.price = 1.0;
        warmup.quantity = 1;
        manager.on_trade(warmup);
        manager.flush_ticks();

        const auto begin = std::chrono::steady_clock::now();

        for (long long index = 0; index < rows; ++index)
        {
            const int symbol = static_cast<int>(index % kSymbolCount);
            TradeData trade;
            trade.ticker = tickers[symbol].c_str();
            trade.symbol_id = static_cast<symbol::SymbolId>(symbol + 1);
            trade.price = 10000.0 + static_cast<double>(index % kPriceSteps);
            trade.quantity = 1 + index % 100;
            trade.direction = index % 2 == 0 ? 1 : 5;
            trade.market = Market::KR;
            manager.on_trade(trade);
        }

        offer_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        manager.flush_ticks();
        total_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        const db::DbStatistics statistics = manager.statistics();
        settled = static_cast<long long>(statistics.ticks_written);
        std::printf("행 %lld · 적재 워커 %u · 묶음 %zu\n", rows, config.tick_workers, config.batch_rows);
        std::printf("넣기(수신 쪽) %.3f초 = %.0f행/초\n", offer_seconds, static_cast<double>(rows) / offer_seconds);
        std::printf("DB까지 %.3f초 = %.0f행/초 (넣음 %lld, 버림 %llu, 거절 %llu, 모호 %llu)\n", total_seconds,
                    static_cast<double>(rows) / total_seconds, settled,
                    static_cast<unsigned long long>(statistics.ticks_dropped),
                    static_cast<unsigned long long>(statistics.ticks_failed),
                    static_cast<unsigned long long>(statistics.ticks_ambiguous));
    }

    delete_bench_rows(config, password);
    return 0;
}
