// Engine::configure — AppConfig의 typed 값을 Engine 세터에 옮긴다. 값 해석(json→typed·검증)은 core/AppConfig.cpp가
//  이미 끝냈고 여기서는 배선만 한다. 단계마다 이름 있는 함수 하나, configure()는 그 호출 목록이다
//  (정본 docs/guides/MAINTENANCE_AUTOMATION.md 4절 "초기화 위치"). start() 전에만 부른다.
#include "core/AppConfig.h"
#include "core/Engine.h"
#include "utils/Logger.h"

#include <algorithm>

namespace
{

// 피드·리플레이·매크로 레짐·ZMQ·운영단말.
void configure_channels(Engine& engine, const AppConfig& app)
{
    engine.set_bootstrap_ledger(app.bootstrap_ledger_from_balance);
    engine.set_rest_price_feed(app.rest_price_feed);
    engine.set_capture_directory(app.capture_directory);
    engine.set_strategy_shards(app.strategy_shards);

    for (const auto& feed_key : app.feed_keys)
    {
        engine.add_feed_config(feed_key);
    }

    // 캡처 파일 리플레이(D-071 원칙 8). WS 대신 파일을 틀어 같은 파이프라인을 돌린다. Engine이 KisClient를 만들지 않으므로
    //  app_key·계좌가 없어도 되고 실주문 경로도 없다 — 종목은 config tickers, 주문은 모의 체결기.
    if (!app.replay_file.empty())
    {
        engine.set_replay(app.replay_file, app.replay_speed, app.replay_cash);
    }

    engine.set_regime_file(app.regime_file, app.regime_stale_sec);
    engine.set_regime_halt_expire_min(app.regime_halt_expire_min);
    engine.set_zmq_control(app.zmq_bind_address, app.zmq_control_token);
    engine.set_ops_control(app.ops_bind_address, app.ops_port, app.ops_token);
}

// G1: 국면→전략 자동선택 맵. 미지정이면 기존 per-strategy active_regimes 방식 유지(하위호환).
void configure_regime_strategies(Engine& engine, const AppConfig& app)
{
    if (!app.has_regime_strategies)
    {
        return;
    }

    engine.set_regime_strategies(app.regime_strategies);
    LOG_INFO("[Engine] 국면→전략 자동선택 맵 " + std::to_string(app.regime_strategies.size()) + "개 국면 적용");
}

// 기동 스모크 테스트 — 서버 실행 직후 지정 종목을 시장가 1주 매수해, 주문 경로 전체가 살아있는지 최소 점검한다.
//  config "startup_check": {"ticker":"005930","qty":1}. 없으면 미가동.
void configure_startup_check(Engine& engine, const AppConfig& app)
{
    if (!app.has_startup_check)
    {
        return;
    }

    engine.set_startup_check(app.startup_check_ticker, app.startup_check_quantity);

    if (!app.startup_check_ticker.empty() && app.startup_check_quantity > 0)
    {
        LOG_INFO("[Engine] 기동 점검 설정: " + app.startup_check_ticker + " 시장가 " +
                 std::to_string(app.startup_check_quantity) + "주 (모의계좌 주문경로 검증)");
    }
}

// 시세 전용(실전 도메인) 키: 모의(openapivts)는 시세 REST가 HTTP 500이므로 시세만 실전으로 조회.
//  스캔 유니버스 분기(universe_from_scan)도 이 실전 키로 거래대금 랭킹/지수를 조회한다(StrategyLoadCtx).
void configure_quote_kis(Engine& engine, const AppConfig& app)
{
    if (!app.quote_kis)
    {
        return;
    }

    engine.set_quote_kis_config(*app.quote_kis);
    LOG_INFO("[Engine] 시세 전용 클라이언트(실전 도메인) 설정됨");
}

// 위험 한도 + 주문 호출 간격. risk 노드가 없으면 매매 창만 넣고 간격 조절은 Engine 기본값.
void configure_risk(Engine& engine, const AppConfig& app)
{
    const OrderGate::Config& risk = app.risk;
    engine.set_risk_config(risk);
    // 마감 자기 종료 — 마지막 창(애프터마켓이 있으면 20:00, 없으면 15:30)이 기준. 창이 0/0이면 판정 없음. [why D-098]
    engine.set_session_end(std::max(risk.session_close_min, risk.after_close_min), app.session_end_grace_sec);

    if (!app.has_risk)
    {
        return;
    }

    LOG_INFO("[Engine] risk 한도: 종목당 " + std::to_string(risk.max_quantity_per_ticker) + "주, 일손실 " +
             std::to_string(static_cast<long long>(risk.daily_loss_limit)) + "원, " +
             std::to_string(risk.max_orders_per_sec) + "/s·" +
             std::to_string(risk.max_orders_per_min) + "/min");

    if (risk.max_notional_per_ticker > 0.0 || risk.max_concurrent_positions > 0)
    {
        LOG_INFO("[Engine] 사이징 백스톱: 종목당 명목 " +
                 std::to_string(static_cast<long long>(risk.max_notional_per_ticker)) + "원, 동시보유 " +
                 std::to_string(risk.max_concurrent_positions) + "종목");
    }

    if (risk.max_gross_exposure_percent > 0.0)
    {
        LOG_INFO("[Engine] 총노출 상한: 자본의 " +
                 std::to_string(risk.max_gross_exposure_percent) + " (잔고 대조 총평가금 기준)");
    }

    // 주문 호출 간격 조절(C-2/W-3) — 버스트 청산 EGW00201 회피 + 거부 SELL 재시도.
    engine.set_order_pacing(app.order_min_interval_ms, app.order_max_retries);
    LOG_INFO("[Engine] 주문 호출 간격 조절: " + std::to_string(app.order_min_interval_ms) + "ms 간격, 청산 SELL 재시도 " +
             std::to_string(app.order_max_retries) + "회");
}

} // namespace

void Engine::configure(const AppConfig& app)
{
    configure_channels(*this, app);
    configure_regime_strategies(*this, app);
    configure_startup_check(*this, app);
    configure_quote_kis(*this, app);
    configure_risk(*this, app);
}
