#include "core/AppConfig.h"
#include "utils/JsonNode.h"
#include "utils/Logger.h"

#include <fstream>
#include <stdexcept>

using json = nlohmann::json;

namespace
{

// 주문·잔고·기본 피드 키. app_key·app_secret·account_no·account_type·is_paper는 필수(없으면 json이 던진다).
KisConfig parse_kis(const json& node)
{
    KisConfig kis_config;
    kis_config.app_key      = node.at("app_key").get<std::string>();
    kis_config.app_secret   = node.at("app_secret").get<std::string>();
    kis_config.account_no   = node.at("account_no").get<std::string>();
    kis_config.account_type = node.at("account_type").get<std::string>();
    kis_config.hts_id       = node.value("hts_id", ""); // 비어 있으면 체결통보 구독을 건너뛴다
    kis_config.is_paper     = node.at("is_paper").get<bool>();
    // 주문 거래소·실시간 채널 — KRX(한국거래소만) / NXT(넥스트레이드만) / SOR(증권사 최선집행). [why D-096]
    kis_config.exchange     = node.value("exchange", "KRX");

    if (kis_config.exchange != "KRX" && kis_config.exchange != "NXT" && kis_config.exchange != "SOR")
    {
        throw std::runtime_error("kis.exchange 는 KRX/NXT/SOR 중 하나여야 한다: '" + kis_config.exchange + "'");
    }

    return kis_config;
}

// 시세 전용(실전 도메인) 키. 실시간 채널 선택은 주문 쪽(kis.exchange) 설정을 따른다. [why D-096]
KisConfig parse_quote_kis(const json& node, const KisConfig& order_kis)
{
    KisConfig kis_config;
    kis_config.app_key      = node.at("app_key").get<std::string>();
    kis_config.app_secret   = node.at("app_secret").get<std::string>();
    kis_config.account_no   = node.value("account_no", "");
    kis_config.account_type = node.value("account_type", "01");
    kis_config.hts_id       = node.value("hts_id", "");
    kis_config.is_paper     = false; // 시세는 실전 도메인
    kis_config.exchange     = order_kis.exchange;
    return kis_config;
}

// 추가 WS 세션 키(D-071 원칙 1). 계좌·모의 여부는 기본 키와 같고 app_key·app_secret만 다르다.
//  체결통보(hts_id)는 **한 세션만** 받는다 — KIS는 세션마다 같은 통보를 보내므로 둘이 받으면 원장이 두 번 센다.
//  기본값은 기본 키가 맡는 것이고, 추가 키에 "fill_notice": true 를 주면 맡는 자리를 그 키로 옮긴다
//  (체결을 듣는 세션을 주문 쪽에 붙이려는 것, D-114 단계 3). 둘 이상이 맡겠다고 하면 첫 번째만 맡는다.
//  맡지 않는 세션은 H0STCNI를 구독하지 않아 그 세션의 구독 슬롯(kMaxWsSubs)이 한 칸 남는다.
//  base는 맡는 자리를 넘길 수 있어야 해서 참조로 받는다. [why D-114]
std::vector<KisConfig> parse_feed_keys(const json& document, KisConfig& base)
{
    std::vector<KisConfig> keys;
    bool                   fill_notice_moved = false;

    for (const auto& key_entry : jsonx::array_or_empty(document, "feed_keys"))
    {
        KisConfig kis_config  = base;
        kis_config.app_key    = key_entry.at("app_key").get<std::string>();
        kis_config.app_secret = key_entry.at("app_secret").get<std::string>();

        if (!fill_notice_moved && key_entry.value("fill_notice", false))
        {
            fill_notice_moved = true; // base에서 복사한 hts_id를 그대로 둔다 — 이 키가 맡는다
        }
        else
        {
            kis_config.hts_id.clear();
        }

        keys.push_back(std::move(kis_config));
    }

    if (fill_notice_moved)
    {
        base.hts_id.clear();
    }

    return keys;
}

// G1: "regime_strategies": {"BULL":[id...], "NEUTRAL":[...], "BEAR":[...]}. id가 '*'로 끝나면 접두 매칭.
//  키는 regime.json 라벨(RISK_ON·NEUTRAL·RISK_OFF)이고, 09-14까지 쓰던 BULL·BEAR도 같은 뜻으로 받는다. [why D-084]
Regime regime_of_key(const std::string& key)
{
    if (key == "BULL" || key == "RISK_ON")
    {
        return Regime::BULL;
    }

    if (key == "BEAR" || key == "RISK_OFF")
    {
        return Regime::BEAR;
    }

    if (key == "NEUTRAL")
    {
        return Regime::NEUTRAL;
    }

    return Regime::UNKNOWN;
}

std::map<Regime, std::vector<std::string>> parse_regime_strategies(const json& node)
{
    std::map<Regime, std::vector<std::string>> regime_map;

    for (auto iterator = node.begin(); iterator != node.end(); ++iterator)
    {
        const Regime regime = regime_of_key(iterator.key());

        if (regime == Regime::UNKNOWN)
        {
            LOG_WARN("[Main] regime_strategies: 알 수 없는 국면 키 '" + iterator.key() + "' 무시");
            continue;
        }

        regime_map[regime] = iterator.value().get<std::vector<std::string>>();
    }

    return regime_map;
}

int hhmm_to_minute(int hhmm)
{
    return (hhmm / 100) * 60 + hhmm % 100;
}

// 위험 한도(risk). 지정된 키만 OrderGate 기본값에서 덮어쓴다 — 실제 돈 규율 튜닝을 재빌드 없이 하기 위함(S-1).
//  매매 세션 창은 risk 노드가 없어도 켠다 — 통합 피드가 08:00~20:00 틱을 줘도 주문은 매매 창 안에서만.
//  정규장 09:00~15:30 + KRX 애프터마켓 16:00~20:00(D-097, risk.after_market=false로 끈다).
//  모의투자(is_paper)는 KIS 모의 서버가 15:30 뒤 주문을 '모의투자 장종료'로 거부하므로(2026-09-18 실측)
//  애프터 창을 config 값과 무관하게 끈다 — KRX 강제와 같은 결. 캡처 리플레이는 밤에도 돌리므로 창을 끈 채(0/0) 둔다. [why D-096]
void parse_risk(const json& document, AppConfig& app)
{
    const json& risk_node   = jsonx::object_or_empty(document, "risk");
    // 부하시험도 밤에 돌리므로 리플레이와 같이 장 시간 창을 끈다 — 창이 닫혀 있으면 주문이 전부 거부된다.
    const bool  replaying    = !app.replay_file.empty() || app.load_test_enabled;
    const bool  after_market = risk_node.value("after_market", true) && !app.kis.is_paper;
    OrderGate::Config& risk  = app.risk;
    risk.session_open_min    = replaying ? 0 : hhmm_to_minute(risk_node.value("session_open_hhmm", 900));
    risk.session_close_min   = replaying ? 0 : hhmm_to_minute(risk_node.value("session_close_hhmm", 1530));
    risk.after_open_min      = (replaying || !after_market) ? 0 : hhmm_to_minute(risk_node.value("after_open_hhmm", 1600));
    risk.after_close_min     = (replaying || !after_market) ? 0 : hhmm_to_minute(risk_node.value("after_close_hhmm", 2000));
    app.session_end_grace_sec = risk_node.value("session_end_grace_sec", app.session_end_grace_sec);
    app.has_risk             = document.contains("risk");

    if (!app.has_risk)
    {
        return;
    }

    risk.max_quantity_per_ticker    = risk_node.value("max_qty_per_ticker", risk.max_quantity_per_ticker);
    risk.daily_loss_limit           = risk_node.value("daily_loss_limit", risk.daily_loss_limit);
    risk.max_orders_per_min         = risk_node.value("max_orders_per_min", risk.max_orders_per_min);
    risk.max_orders_per_sec         = risk_node.value("max_orders_per_sec", risk.max_orders_per_sec);
    risk.deduplicate_window_sec     = risk_node.value("dedup_window_sec", risk.deduplicate_window_sec);
    risk.max_quantity_per_order     = risk_node.value("max_qty_per_order", risk.max_quantity_per_order);
    risk.max_notional_per_order     = risk_node.value("max_notional_per_order", risk.max_notional_per_order);
    risk.max_notional_per_ticker    = risk_node.value("max_notional_per_ticker", risk.max_notional_per_ticker);
    risk.max_concurrent_positions   = risk_node.value("max_concurrent_positions", risk.max_concurrent_positions);
    risk.max_gross_exposure_percent = risk_node.value("max_gross_exposure_pct", risk.max_gross_exposure_percent);
    // 슬롯 경합 시 점수 상위부터 채운다(선착순 금지). 스캐너가 랭크를 게이트에 주입한다.
    risk.entry_priority_enabled     = risk_node.value("entry_priority_enabled", risk.entry_priority_enabled);
    // 슬롯이 꽉 찬 뒤에도 더 높은 점수가 오면 최약체를 비우고 자리를 넘긴다(교체 진입).
    risk.displace_enabled           = risk_node.value("displace_enabled", risk.displace_enabled);
    risk.displace_min_z_gap         = risk_node.value("displace_min_z_gap", risk.displace_min_z_gap);
    risk.displace_min_hold_sec      = risk_node.value("displace_min_hold_sec", risk.displace_min_hold_sec);
    risk.displace_cooldown_sec      = risk_node.value("displace_cooldown_sec", risk.displace_cooldown_sec);
    risk.displace_max_per_day       = risk_node.value("displace_max_per_day", risk.displace_max_per_day);
    risk.displace_slot_hold_sec     = risk_node.value("displace_slot_hold_sec", risk.displace_slot_hold_sec);
    // 오늘 스캔에 안 잡힌 이월 보유분에 매길 점수. 0이면 그런 보유분은 교체 후보에서 빠진다.
    risk.displace_unscored_z        = risk_node.value("displace_unscored_z", risk.displace_unscored_z);
    // 주문 호출 간격 조절(C-2/W-3) — 버스트 청산 EGW00201 회피 + 거부 SELL 재시도.
    app.order_min_interval_ms       = risk_node.value("order_min_interval_ms", app.order_min_interval_ms);
    app.order_max_retries           = risk_node.value("order_max_retries", app.order_max_retries);
}

} // namespace

json load_config_file(const std::string& path)
{
    std::ifstream file(path);

    if (!file.is_open())
    {
        throw std::runtime_error("설정 파일 없음: " + path);
    }

    return json::parse(file);
}

AppConfig parse_config(const json& document)
{
    // 실행 모드는 TRADE 하나만 남았다. 옛 설정에 "mode": "FEED" 가 남아 있으면 주문 없이 시세만 보려던 설정이
    //  실계좌 매매로 뜬다 — 그래서 조용히 무시하지 않고 멈춘다. [why D-130]
    if (const auto mode_node = document.find("mode"); mode_node != document.end() && *mode_node != "TRADE")
    {
        throw std::runtime_error("mode \"" + mode_node->get<std::string>() +
                                 "\" 는 지웠다 — TRADE만 남았다(D-130). 설정에서 mode 줄을 빼거나 TRADE로 둔다");
    }

    AppConfig app;
    app.debug_log = document.value("log_level", std::string("INFO")) == "DEBUG";
    app.kis       = parse_kis(document.at("kis"));

    app.fetch_interval_sec            = document.value("fetch_interval_sec", app.fetch_interval_sec);
    app.bootstrap_ledger_from_balance = document.value("bootstrap_ledger_from_balance", false);
    app.rest_price_feed               = document.value("rest_price_feed", false);
    app.capture_directory             = document.value("capture_dir", std::string());
    app.capture_tickers               = document.value("capture_tickers", std::vector<std::string>{});
    app.websocket_pin_tickers         = document.value("websocket_pin_tickers", std::vector<std::string>{});
    app.ledger_journal_directory      = document.value("ledger_journal_dir", std::string());
    app.ledger_journal_fsync          = document.value("ledger_journal_fsync", false);
    app.strategy_shards               = document.value("strategy_shards", 1u);
    app.feed_keys                     = parse_feed_keys(document, app.kis);
    app.replay_file                   = document.value("replay_file", std::string());
    app.replay_speed                  = document.value("replay_speed", app.replay_speed);
    app.replay_cash                   = document.value("replay_cash", app.replay_cash);

    const json& load_test_node        = jsonx::object_or_empty(document, "load_test");
    app.load_test_enabled             = load_test_node.value("enabled", false);
    app.load_test_lanes               = load_test_node.value("lanes", app.load_test_lanes);
    app.load_test_base_port           = load_test_node.value("base_port", app.load_test_base_port);
    app.load_test_bind_address        = load_test_node.value("bind_addr", app.load_test_bind_address);
    app.load_test_session_hhmmss      = load_test_node.value("session_start_hhmmss", 0);
    app.load_test_universe_out        = load_test_node.value("universe_out", std::string());

    app.regime_file                   = document.value("regime_file", std::string());
    app.regime_stale_sec              = document.value("regime_stale_sec", app.regime_stale_sec);
    app.regime_halt_expire_min        = document.value("regime_halt_expire_min", app.regime_halt_expire_min);
    app.zmq_bind_address              = document.value("zmq_bind_addr", std::string());
    app.zmq_control_token             = document.value("zmq_control_token", std::string());
    app.protective_orders             = document.value("protective_orders", app.protective_orders);
    app.protective_orders_interval_ms = document.value("protective_orders_interval_ms", app.protective_orders_interval_ms);
    app.protective_orders_retry_ms    = document.value("protective_orders_retry_ms", app.protective_orders_retry_ms);
    app.zmq_pub_port                  = document.value("zmq_pub_port", app.zmq_pub_port);
    app.zmq_rep_port                  = document.value("zmq_rep_port", app.zmq_rep_port);
    app.zmq_feed_pub_port             = document.value("zmq_feed_pub_port", app.zmq_feed_pub_port);
    app.zmq_strategy_pub_port         = document.value("zmq_strategy_pub_port", app.zmq_strategy_pub_port);

    // 안 적었으면 주문 포트에서 끌어온다 — 설정을 안 고쳐도 갈라 뜨고, 계좌를 둘 돌려도 겹치지 않는다.
    if (app.zmq_feed_pub_port <= 0)
    {
        app.zmq_feed_pub_port = app.zmq_pub_port + 2;
    }

    if (app.zmq_strategy_pub_port <= 0)
    {
        app.zmq_strategy_pub_port = app.zmq_pub_port + 3;
    }

    app.ops_bind_address              = document.value("ops_bind_addr", std::string());
    app.ops_port                      = document.value("ops_port", 0);
    app.ops_token                     = document.value("ops_token", std::string());
    app.instance                      = document.value("instance", std::string());

    if (document.contains("regime_strategies"))
    {
        app.has_regime_strategies = true;
        app.regime_strategies     = parse_regime_strategies(document["regime_strategies"]);

        // 선택 입력은 regime.json 라벨이라 파일이 없으면 맵이 한 번도 적용되지 않는다(전 전략 기본 활성).
        if (app.regime_file.empty())
        {
            LOG_WARN("[Main] regime_strategies가 있는데 regime_file이 비어 있다 — 국면별 전략 선택이 동작하지 않는다");
        }
    }

    if (document.contains("quote_kis"))
    {
        app.quote_kis = parse_quote_kis(document["quote_kis"], app.kis);
    }
    else if (!app.kis.is_paper)
    {
        // 실계좌는 주문 키가 곰 실전 키다 — 시세용을 따로 적을 이유가 없다. 모의계좌만 시세가 막혀
        //  있어 quote_kis 로 실전 키를 하나 더 받는다. 이 갈래가 없던 탓에 2026-09-23 실계좌 첫날
        //  유니버스 스캔이 "quote_kis(실전 시세 키) 미설정"으로 통째 건너뛰었고, 그 탓에 구독 종목이
        //  0개가 돼 WS 자체가 안 열려 체결통보까지 못 받았다. [why D-097]
        app.quote_kis = app.kis;
    }

    parse_risk(document, app);

    if (document.contains("strategies"))
    {
        app.strategies = document["strategies"];
    }

    return app;
}
