// DbManager 단위 테스트 — COPY 글자(이스케이프·시각·한 줄), 비밀번호 없으면 꺼짐, DB가 없을 때 큐 넘침 계수와
// 종료 시간 한도. TSDB_PASSWORD가 있으면 ticks 표에 넣고 되읽은 뒤 지우고, 표가 잠긴 동안의 종료를 본다. [why D-148]
// 빌드: cmake --build <directory> --target test_db_manager
#include "ipc/DbManager.h"

#include <libpq-fe.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace
{
int g_checks = 0;

#define CHECK(condition)                                                                    \
    do                                                                                      \
    {                                                                                       \
        ++g_checks;                                                                         \
        if (!(condition))                                                                   \
        {                                                                                   \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #condition << "\n"; \
            return 1;                                                                       \
        }                                                                                   \
    } while (0)

void set_environment(const char* name, const std::string& value)
{
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    if (value.empty())
    {
        unsetenv(name);
    }
    else
    {
        setenv(name, value.c_str(), 1);
    }
#endif
}

std::string environment(const char* name)
{
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
}

TradeData make_trade(const char* ticker, int index)
{
    TradeData trade;
    trade.ticker    = ticker;
    trade.symbol_id = static_cast<symbol::SymbolId>(index + 1);
    trade.price     = 70000.5 + index;
    trade.quantity  = 10 + index;
    trade.direction = index % 2 == 0 ? 1 : 5;
    trade.market    = Market::KR;
    return trade;
}

int test_copy_text()
{
    std::string out;
    db::append_copy_text(out, "a\\b\tc\nd\re");
    CHECK(out == "a\\\\b\\tc\\nd\\re");

    out.clear();
    db::append_iso_utc(out, 1'700'000'000'123LL);
    CHECK(out == "2023-11-14T22:13:20.123+00:00");

    out.clear();
    db::append_iso_utc(out, 0);
    CHECK(out == "1970-01-01T00:00:00.000+00:00");

    out.clear();
    TradeData trade = make_trade("005930", 0);
    db::append_trade_row(out, trade, 1'700'000'000'005LL);
    CHECK(out == "2023-11-14T22:13:20.005+00:00\t005930\t70000.5\t10\t1\tKR\n");

    out.clear();
    trade.market = Market::US;
    trade.price = 12.25;
    trade.direction = 0;
    db::append_trade_row(out, trade, 1'700'000'000'005LL);
    CHECK(out == "2023-11-14T22:13:20.005+00:00\t005930\t12.25\t10\t0\tUS\n");
    return 0;
}

int test_disabled_without_password()
{
    const std::string saved = environment("TSDB_PASSWORD");
    set_environment("TSDB_PASSWORD", "");
    {
        db::DbManager manager(db::DbConfig{});
        CHECK(!manager.ok());
        manager.on_trade(make_trade("005930", 0));
        CHECK(manager.statistics().ticks_offered == 0);
    }

    set_environment("TSDB_PASSWORD", saved);
    return 0;
}

// DB가 없는 주소 — 수신 쪽은 막히지 않고 넘치는 만큼 버린다. 워커 둘이 상태표를 나눠 보므로
//  종료는 워커마다 접속을 기다리지 않는다.
int test_without_database()
{
    const std::string saved = environment("TSDB_PASSWORD");
    set_environment("TSDB_PASSWORD", "unused");
    {
        db::DbConfig config;
        config.host = "127.0.0.2";
        config.port = 1;
        config.tick_workers = 2;
        config.tick_queue_capacity = 8;
        db::DbManager manager(config);
        CHECK(manager.ok());

        const auto begin = std::chrono::steady_clock::now();

        for (int index = 0; index < 100; ++index)
        {
            manager.on_trade(make_trade("005930", index));
        }

        CHECK(std::chrono::steady_clock::now() - begin < std::chrono::milliseconds(100));

        const auto stop_begin = std::chrono::steady_clock::now();
        manager.stop();
        CHECK(std::chrono::steady_clock::now() - stop_begin < std::chrono::seconds(4));
        const db::DbStatistics statistics = manager.statistics();
        CHECK(statistics.ticks_offered == 100);
        CHECK(statistics.ticks_dropped > 0);
        CHECK(statistics.ticks_written == 0);
        CHECK(statistics.ticks_dropped + statistics.ticks_failed == 100);
    }

    set_environment("TSDB_PASSWORD", saved);
    return 0;
}

long long count_rows(PGconn* connection, const std::string& statement)
{
    PGresult*       result = PQexec(connection, statement.c_str());
    const long long count = PQresultStatus(result) == PGRES_TUPLES_OK ? std::atoll(PQgetvalue(result, 0, 0)) : -1;
    PQclear(result);
    return count;
}

// 실제 ticks 표에 넣고 되읽는다. 시험 종목 이름은 실제 종목과 겹치지 않고, 끝나면 지운다.
int test_round_trip_with_database()
{
    const std::string password = environment("TSDB_PASSWORD");

    if (password.empty())
    {
        std::cout << "  (TSDB_PASSWORD 없음 — DB 되읽기 건너뜀)\n";
        return 0;
    }

    const std::string ticker = "ZZDBW" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count() % 100000000);
    db::DbConfig config;
    config.tick_workers = 2;
    config.batch_rows = 300;
    {
        db::DbManager manager(config);
        CHECK(manager.ok());

        for (int index = 0; index < 1000; ++index)
        {
            manager.on_trade(make_trade(ticker.c_str(), index));
        }

        manager.flush_ticks();
        manager.stop();
        const db::DbStatistics statistics = manager.statistics();
        CHECK(statistics.ticks_dropped == 0);
        CHECK(statistics.ticks_failed == 0);
        CHECK(statistics.ticks_ambiguous == 0);
        CHECK(statistics.ticks_written == 1000);
    }

    const std::string host = db::resolve_host(config.host);
    const std::string port = std::to_string(config.port);
    const char* const keywords[] = {"host", "port", "dbname", "user", "password", nullptr};
    const char* const values[] = {host.c_str(), port.c_str(), config.dbname.c_str(), config.user.c_str(),
                                  password.c_str(), nullptr};
    PGconn* connection = PQconnectdbParams(keywords, values, 0);
    CHECK(PQstatus(connection) == CONNECTION_OK);

    const std::string where = " FROM ticks WHERE ticker = '" + ticker + "'";
    const long long   total = count_rows(connection, "SELECT count(*)" + where);
    const long long   sells = count_rows(connection, "SELECT count(*)" + where + " AND direction = 5");
    const long long   price_sum = count_rows(connection, "SELECT sum(price * 2)::bigint" + where);
    const long long   recent = count_rows(connection, "SELECT count(*)" + where + " AND ts > now() - interval '5 minutes'");
    PQclear(PQexec(connection, ("DELETE" + where).c_str()));
    PQfinish(connection);

    CHECK(total == 1000);
    CHECK(sells == 500);
    // sum(70000.5 + i) * 2 = 1000 * 140001 + 2 * (0+...+999)
    CHECK(price_sum == 1000LL * 140001 + 999LL * 1000);
    CHECK(recent == 1000);
    return 0;
}

// DB가 COPY 도중 멈춘 경우 — 다른 연결이 ticks 표를 잠가 COPY를 붙잡아 둔다. 수신 쪽은 막히지 않고,
//  stop()은 stop_grace_ms 뒤 연결을 끊어 돌아온다. 잠금은 2초 안쪽이라 돌고 있는 적재기가 있어도 잠깐 기다릴 뿐이다.
int test_stop_while_database_hangs()
{
    const std::string password = environment("TSDB_PASSWORD");

    if (password.empty())
    {
        std::cout << "  (TSDB_PASSWORD 없음 — DB 멈춤 시험 건너뜀)\n";
        return 0;
    }

    db::DbConfig config;
    config.tick_workers = 1;
    config.stop_grace_ms = 300;
    const std::string host = db::resolve_host(config.host);
    const std::string port = std::to_string(config.port);
    const char* const keywords[] = {"host", "port", "dbname", "user", "password", nullptr};
    const char* const values[] = {host.c_str(), port.c_str(), config.dbname.c_str(), config.user.c_str(),
                                  password.c_str(), nullptr};
    PGconn* locker = PQconnectdbParams(keywords, values, 0);
    CHECK(PQstatus(locker) == CONNECTION_OK);
    PQclear(PQexec(locker, "BEGIN"));
    PQclear(PQexec(locker, "LOCK TABLE ticks IN ACCESS EXCLUSIVE MODE"));
    {
        db::DbManager manager(config);
        CHECK(manager.ok());

        const auto begin = std::chrono::steady_clock::now();

        for (int index = 0; index < 100; ++index)
        {
            manager.on_trade(make_trade("ZZHANG", index));
        }

        CHECK(std::chrono::steady_clock::now() - begin < std::chrono::milliseconds(100));

        // 적재 워커가 잠금에 걸릴 때까지 기다린다(최대 10초). pg_stat_activity는 트랜잭션 안에서 첫 조회 값에
        //  굳으므로 pg_locks의 못 받은 잠금으로 본다.
        long long waiting = 0;

        for (int round = 0; round < 100 && waiting == 0; ++round)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            waiting = count_rows(locker, "SELECT count(*) FROM pg_locks WHERE NOT granted AND relation = "
                                         "'ticks'::regclass");
        }

        CHECK(waiting == 1);

        // 막혀 있는 동안에도 수신 쪽은 그대로 넣는다.
        const auto more = std::chrono::steady_clock::now();
        manager.on_trade(make_trade("ZZHANG", 100));
        CHECK(std::chrono::steady_clock::now() - more < std::chrono::milliseconds(10));

        const auto stop_begin = std::chrono::steady_clock::now();
        manager.stop();
        CHECK(std::chrono::steady_clock::now() - stop_begin < std::chrono::seconds(2));
        const db::DbStatistics statistics = manager.statistics();
        CHECK(statistics.ticks_written == 0);
        CHECK(statistics.ticks_failed + statistics.ticks_ambiguous + statistics.ticks_dropped == 101);
    }

    PQclear(PQexec(locker, "ROLLBACK"));
    PQfinish(locker);
    return 0;
}
} // namespace

int main()
{
    if (test_copy_text() != 0 || test_disabled_without_password() != 0 || test_without_database() != 0 ||
        test_round_trip_with_database() != 0 || test_stop_while_database_hangs() != 0)
    {
        return 1;
    }

    std::cout << "test_db_manager: " << g_checks << " checks passed\n";
    return 0;
}
