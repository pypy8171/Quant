// DbManager 단위 테스트 — COPY 글자(이스케이프·시각·한 줄), 비밀번호 없으면 꺼짐, DB가 없을 때 큐 넘침 계수와
// 종료 시간 한도. TSDB_PASSWORD가 있으면 ticks 표에 넣고 되읽은 뒤 지우고, 표가 잠긴 동안의 종료를 본다. [why D-148]
// 신호·헬스(signals·health) 줄 모양, 계좌 없으면 안 넣음, DB 없을 때 계수, 있으면 되읽기도 본다. [why D-154]
// 빌드: cmake --build <directory> --target test_db_manager
#include "ipc/DbManager.h"

#include <libpq-fe.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

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

OrderSignal make_signal(const std::string& ticker, const std::string& strategy)
{
    OrderSignal signal;
    signal.ticker      = ticker;
    signal.strategy_id = strategy;
    signal.side        = OrderSide::BUY;
    signal.quantity    = 3;
    signal.price       = 71500.5;
    signal.market      = Market::KR;
    return signal;
}

HealthSnapshot make_health()
{
    HealthSnapshot snapshot;
    snapshot.data_count            = 100;
    snapshot.signal_count          = 7;
    snapshot.order_count           = 2;
    snapshot.shard_capacity        = 4096;
    snapshot.latency_samples       = 2;
    snapshot.tick_to_signal_p50_us = 15;
    snapshot.interval_segments[0]  = {"tick_to_signal", 11, 22};
    snapshot.interval_segments[1]  = {"total", -1, -1};
    snapshot.interval_samples      = 1;
    return snapshot;
}

// 신호·헬스 한 줄 — 옛 파이썬 적재기와 같은 열·같은 값이다. [why D-154]
int test_signal_and_health_rows()
{
    std::string out;
    OrderSignal signal = make_signal("005930", "DevScale\tA");
    db::append_signal_row(out, db::make_signal_row(signal, "RISK_ON", 1'700'000'000'005LL), "5020");
    CHECK(out == "2023-11-14T22:13:20.005+00:00\tDevScale\\tA\t005930\tBUY\t3\t71500.5\tKR\tRISK_ON\t5020\n");

    // 전략 이름이 길면 잘리고, 국면이 없으면 빈 글자다(파이썬도 ""를 그대로 넣었다).
    out.clear();
    signal = make_signal("AAPL", std::string(60, 'x'));
    signal.side   = OrderSide::SELL;
    signal.market = Market::US;
    db::append_signal_row(out, db::make_signal_row(signal, {}, 0), "5020");
    CHECK(out == "1970-01-01T00:00:00.000+00:00\t" + std::string(db::SignalRow::kStrategyMax, 'x') +
                     "\tAAPL\tSELL\t3\t71500.5\tUS\t\t5020\n");

    const HealthSnapshot           snapshot = make_health();
    const std::vector<std::string> columns  = db::health_metric_columns(snapshot);
    CHECK(columns.size() == 27 + 4);
    CHECK(columns.front() == "drop_cnt");
    CHECK(columns[26] == "latency_interval_samples");
    CHECK(columns[27] == "tick_to_signal_p50_interval_us");
    CHECK(columns.back() == "total_p99_interval_us");

    // 발행기가 없으면 drop_* 다섯 칸은 NULL이고, 값 칸 수는 열 수와 같다.
    out.clear();
    db::append_health_row(out, snapshot, 1'700'000'000'005LL, "both", "5020");
    const std::string prefix = "2023-11-14T22:13:20.005+00:00\tboth\t5020\t100\t7\t2\t\\N\t\\N\t\\N\t\\N\t\\N\t0\t4096\t";
    CHECK(out.rfind(prefix, 0) == 0);
    CHECK(out.back() == '\n');
    const auto tabs = static_cast<size_t>(std::count(out.begin(), out.end(), '\t'));
    CHECK(tabs == 6 + columns.size() - 1);
    CHECK(out.find("\t1\t11\t22\t-1\t-1\n") != std::string::npos);

    HealthSnapshot published    = snapshot;
    published.has_publish_drops = true;
    published.publish_dropped   = 5;
    published.drop_socket_full  = 5;
    out.clear();
    db::append_health_row(out, published, 1'700'000'000'005LL, "order", "5020");
    CHECK(out.find("\t2\t5\t5\t0\t0\t0\t0\t4096\t") != std::string::npos);
    CHECK(out.find("\\N") == std::string::npos);
    return 0;
}

// 계좌가 없으면 신호·헬스 워커를 띄우지 않는다 — 옛 적재기의 계좌 거르기와 같은 뜻. [why D-154]
int test_events_need_account()
{
    const std::string saved = environment("TSDB_PASSWORD");
    set_environment("TSDB_PASSWORD", "unused");
    {
        db::DbConfig config;
        config.host         = "127.0.0.2";
        config.port         = 1;
        config.tick_workers = 0;
        db::DbManager manager(config);
        CHECK(!manager.ok());
        manager.on_signal(make_signal("005930", "S"), "RISK_ON");
        manager.on_health(make_health());
        CHECK(manager.statistics().signals_offered == 0);
        CHECK(manager.statistics().health_offered == 0);
    }

    set_environment("TSDB_PASSWORD", saved);
    return 0;
}

// DB가 없는 주소 — 전략·데이터 스레드는 막히지 않고, 넘친 행은 버리고 센다. 종료는 시간 안에 돌아온다.
int test_events_without_database()
{
    const std::string saved = environment("TSDB_PASSWORD");
    set_environment("TSDB_PASSWORD", "unused");
    {
        db::DbConfig config;
        config.host                  = "127.0.0.2";
        config.port                  = 1;
        config.tick_workers          = 0;
        config.account               = "ZZTEST";
        config.signal_queue_capacity = 8;
        config.health_queue_capacity = 2;
        db::DbManager manager(config);
        CHECK(manager.ok());

        const auto begin = std::chrono::steady_clock::now();

        for (int index = 0; index < 100; ++index)
        {
            manager.on_signal(make_signal("005930", "S"), "RISK_ON");
        }

        for (int index = 0; index < 10; ++index)
        {
            manager.on_health(make_health());
        }

        CHECK(std::chrono::steady_clock::now() - begin < std::chrono::milliseconds(100));

        const auto stop_begin = std::chrono::steady_clock::now();
        manager.stop();
        CHECK(std::chrono::steady_clock::now() - stop_begin < std::chrono::seconds(4));
        const db::DbStatistics statistics = manager.statistics();
        CHECK(statistics.signals_offered == 100);
        CHECK(statistics.signals_dropped > 0);
        CHECK(statistics.signals_written == 0);
        CHECK(statistics.signals_dropped + statistics.signals_failed == 100);
        CHECK(statistics.health_offered == 10);
        CHECK(statistics.health_dropped + statistics.health_failed == 10);
        CHECK(statistics.ticks_offered == 0);
    }

    set_environment("TSDB_PASSWORD", saved);
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

// 실제 signals·health 표에 넣고 되읽는다. 시험 계좌는 실제 계좌와 겹치지 않고, 끝나면 지운다. [why D-154]
int test_events_round_trip_with_database()
{
    const std::string password = environment("TSDB_PASSWORD");

    if (password.empty())
    {
        std::cout << "  (TSDB_PASSWORD 없음 — 신호·헬스 되읽기 건너뜀)\n";
        return 0;
    }

    const std::string account = "ZZDBE" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count() % 100000000);
    db::DbConfig config;
    config.tick_workers = 0;
    config.account      = account;
    config.role         = "strategy";
    {
        db::DbManager manager(config);
        CHECK(manager.ok());

        for (int index = 0; index < 20; ++index)
        {
            manager.on_signal(make_signal("005930", "ZZSTRAT"), "RISK_ON");
        }

        manager.on_health(make_health());
        manager.flush_events();
        manager.stop();
        const db::DbStatistics statistics = manager.statistics();
        CHECK(statistics.signals_written == 20);
        CHECK(statistics.signals_failed == 0);
        CHECK(statistics.health_written == 1);
        CHECK(statistics.health_failed == 0);
    }

    const std::string host = db::resolve_host(config.host);
    const std::string port = std::to_string(config.port);
    const char* const keywords[] = {"host", "port", "dbname", "user", "password", nullptr};
    const char* const values[] = {host.c_str(), port.c_str(), config.dbname.c_str(), config.user.c_str(),
                                  password.c_str(), nullptr};
    PGconn* connection = PQconnectdbParams(keywords, values, 0);
    CHECK(PQstatus(connection) == CONNECTION_OK);

    const std::string where   = " WHERE account = '" + account + "'";
    const long long   signals = count_rows(connection, "SELECT count(*) FROM signals" + where +
                                                           " AND regime = 'RISK_ON' AND side = 'BUY' AND qty = 3");
    const long long   health  = count_rows(connection, "SELECT count(*) FROM health" + where +
                                                           " AND role = 'strategy' AND data_cnt = 100 AND drop_cnt IS NULL"
                                                           " AND tick_to_signal_p99_interval_us = 22");
    PQclear(PQexec(connection, ("DELETE FROM signals" + where).c_str()));
    PQclear(PQexec(connection, ("DELETE FROM health" + where).c_str()));
    PQfinish(connection);

    CHECK(signals == 20);
    CHECK(health == 1);
    return 0;
}
} // namespace

int main()
{
    if (test_copy_text() != 0 || test_disabled_without_password() != 0 || test_without_database() != 0 ||
        test_round_trip_with_database() != 0 || test_stop_while_database_hangs() != 0 ||
        test_signal_and_health_rows() != 0 || test_events_need_account() != 0 || test_events_without_database() != 0 ||
        test_events_round_trip_with_database() != 0)
    {
        return 1;
    }

    std::cout << "test_db_manager: " << g_checks << " checks passed\n";
    return 0;
}
