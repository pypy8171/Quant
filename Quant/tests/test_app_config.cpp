// config.json → AppConfig 경계 시험. json을 읽는 곳이 parse_config 하나뿐이라 키 이름·기본값·검증이 여기서 고정된다 —
//  키를 옮기거나 기본값을 바꾸면 이 파일도 같이 바뀌어야 한다.
#include "core/AppConfig.h"

#include <iostream>
#include <stdexcept>
#include <string>

using json = nlohmann::json;

namespace
{
int g_checks = 0;

#define CHECK(condition)                                                                        \
    do                                                                                     \
    {                                                                                      \
        ++g_checks;                                                                        \
        if (!(condition))                                                                       \
        {                                                                                  \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #condition << "\n";     \
            return 1;                                                                      \
        }                                                                                  \
    } while (0)

json minimal_document()
{
    return json::parse(R"({
        "kis": {"app_key": "k", "app_secret": "s", "account_no": "12345678", "account_type": "01", "is_paper": true}
    })");
}
} // namespace

int main()
{
    // 1. 최소 문서 — 기본값이 Engine·OrderGate 기본과 같고, 매매 창은 risk 노드 없이도 켜진다.
    {
        const AppConfig app = parse_config(minimal_document(), "");
        CHECK(app.mode == "FEED");
        CHECK(!app.debug_log);
        CHECK(app.kis.account_no == "12345678");
        CHECK(app.kis.is_paper);
        CHECK(app.kis.exchange == "KRX");
        CHECK(app.fetch_interval_sec == 60);
        CHECK(app.strategy_shards == 1);
        CHECK(app.feed_keys.empty());
        CHECK(app.replay_file.empty());
        CHECK(app.regime_stale_sec == kDefaultRegimeStaleSec);
        CHECK(!app.has_regime_strategies);
        CHECK(!app.quote_kis.has_value());
        CHECK(!app.has_risk);
        CHECK(app.risk.session_open_min == 9 * 60);
        CHECK(app.risk.session_close_min == 15 * 60 + 30);
        CHECK(app.risk.after_open_min == 0 && app.risk.after_close_min == 0); // 모의는 애프터 창 없음
        CHECK(app.strategies.is_array() && app.strategies.empty());
    }

    // 1b. 실계좌면 애프터마켓 창 16:00~20:00이 기본으로 켜진다.
    {
        json document           = minimal_document();
        document["kis"]["is_paper"] = false;
        const AppConfig app     = parse_config(document, "");
        CHECK(app.risk.after_open_min == 16 * 60);
        CHECK(app.risk.after_close_min == 20 * 60);
    }

    // 2. 모드 오버라이드가 문서의 mode를 이기고, log_level=DEBUG가 읽힌다.
    {
        json document     = minimal_document();
        document["mode"]  = "TRADE";
        document["log_level"] = "DEBUG";
        CHECK(parse_config(document, "").mode == "TRADE");
        CHECK(parse_config(document, "FEED").mode == "FEED");
        CHECK(parse_config(document, "").debug_log);
    }

    // 3. kis.exchange 검증 — 허용 밖이면 던진다(네트워크 전에 멈추는 자리).
    {
        json document = minimal_document();
        document["kis"]["exchange"] = "NYSE";
        bool thrown = false;

        try
        {
            parse_config(document, "");
        }
        catch (const std::runtime_error&)
        {
            thrown = true;
        }

        CHECK(thrown);
        document["kis"]["exchange"] = "SOR";
        CHECK(parse_config(document, "").kis.exchange == "SOR");
    }

    // 4. feed_keys는 기본 키의 계좌·모의 여부를 물려받고 hts_id는 비운다(체결통보는 기본 키가 맡는다).
    //    quote_kis는 실전 도메인 + 주문 쪽 exchange.
    {
        json document = minimal_document();
        document["kis"]["hts_id"]   = "hts";
        document["kis"]["exchange"] = "NXT";
        document["feed_keys"]       = json::array({{{"app_key", "k2"}, {"app_secret", "s2"}}});
        document["quote_kis"]       = {{"app_key", "qk"}, {"app_secret", "qs"}};
        const AppConfig app = parse_config(document, "");
        CHECK(app.feed_keys.size() == 1);
        CHECK(app.feed_keys[0].app_key == "k2");
        CHECK(app.feed_keys[0].account_no == "12345678");
        CHECK(app.feed_keys[0].is_paper);
        CHECK(app.feed_keys[0].hts_id.empty());
        CHECK(app.quote_kis.has_value());
        CHECK(!app.quote_kis->is_paper);
        CHECK(app.quote_kis->account_type == "01");
        CHECK(app.quote_kis->exchange == "NXT");
        CHECK(app.kis.hts_id == "hts"); // 아무도 안 가져가면 기본 키가 체결통보를 맡는다
    }

    // 4b. feed_keys 항목이 fill_notice를 켜면 체결통보를 맡는 자리가 그 키로 넘어간다 — 기본 키는 hts_id를 잃고,
    //     둘이 켜도 첫 번째만 맡는다(둘이 받으면 원장이 체결을 두 번 센다). [why D-114]
    {
        json document = minimal_document();
        document["kis"]["hts_id"] = "hts";
        document["feed_keys"]     = json::array({{{"app_key", "k2"}, {"app_secret", "s2"}, {"fill_notice", true}},
                                                 {{"app_key", "k3"}, {"app_secret", "s3"}, {"fill_notice", true}}});
        const AppConfig app = parse_config(document, "");
        CHECK(app.feed_keys.size() == 2);
        CHECK(app.feed_keys[0].hts_id == "hts");
        CHECK(app.feed_keys[1].hts_id.empty());
        CHECK(app.kis.hts_id.empty());
    }

    // 5. risk — 지정한 키만 덮고, hhmm은 분으로, after_market=false면 실계좌라도 애프터 창 0/0, 리플레이면 창 전부 0.
    {
        json document = minimal_document();
        document["kis"]["is_paper"] = false;
        document["risk"] = {{"max_qty_per_ticker", 7},        {"daily_loss_limit", -50000.0},
                            {"session_open_hhmm", 930},        {"after_market", false},
                            {"order_min_interval_ms", 500},    {"displace_enabled", true}};
        const AppConfig app = parse_config(document, "");
        CHECK(app.has_risk);
        CHECK(app.risk.max_quantity_per_ticker == 7);
        CHECK(app.risk.daily_loss_limit == -50000.0);
        CHECK(app.risk.session_open_min == 9 * 60 + 30);
        CHECK(app.risk.session_close_min == 15 * 60 + 30);
        CHECK(app.risk.after_open_min == 0 && app.risk.after_close_min == 0);
        CHECK(app.risk.displace_enabled);
        CHECK(app.order_min_interval_ms == 500);
        CHECK(app.order_max_retries == 3);

        document["replay_file"] = "capture.bin";
        const AppConfig replay = parse_config(document, "");
        CHECK(replay.risk.session_open_min == 0 && replay.risk.session_close_min == 0);
        CHECK(replay.replay_speed == 1.0);
    }

    // 6. regime_strategies — RISK_ON/RISK_OFF와 BULL/BEAR를 같은 국면으로 받고, 모르는 키는 버린다.
    {
        json document = minimal_document();
        document["regime_strategies"] = {{"RISK_ON", {"DevScale_*"}}, {"BEAR", {"ITB"}}, {"SIDEWAYS", {"x"}}};
        document["strategies"]        = json::array({{{"type", "DEVSCALE"}}});
        const AppConfig app = parse_config(document, "");
        CHECK(app.has_regime_strategies);
        CHECK(app.regime_strategies.size() == 2);
        CHECK(app.regime_strategies.at(Regime::BULL).at(0) == "DevScale_*");
        CHECK(app.regime_strategies.at(Regime::BEAR).at(0) == "ITB");
        CHECK(app.strategies.size() == 1);
    }

    std::cout << "test_app_config OK (" << g_checks << " checks)\n";
    return 0;
}
