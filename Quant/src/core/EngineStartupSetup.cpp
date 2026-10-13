// 기동 준비 — 브로커 인증, 모의 체결기, 주문 라우터, 잔고 대조기, REST 폴러, 장부 저널·잔고 시드.
//  Engine 클래스는 그대로다. Engine.cpp 가 길어 start()가 부르는 준비 단계만 따로 낸 것이다(몸통은 옮기기만 했다).
//  관련 결정: D-061·D-062·D-113·D-114.

#include "core/Engine.h"
#include "core/KstTime.h"
#include "core/LatencyTrace.h"
#include "core/ReconcilePlan.h"
#include "utils/Logger.h"
#include "utils/ThreadName.h"
#include "utils/Utf8.h"
#include <chrono>
#include <ctime>
#include <memory>
#include <thread>
#include <nlohmann/json.hpp>

using namespace std::chrono_literals;

bool Engine::authenticate_feed(bool offline)
{
    // 피드를 직접 받았거나 캡처 파일을 트는 것이면 브로커 없이 돈다 — feed_.kis는 비고, 아래 KIS를 보는 경로는 전부 null을
    //  "소스 없음"으로 다룬다. 리플레이의 종목은 config tickers(전략 구독)뿐이고 유니버스 스캔·REST 봉 시드는 없다. [why D-071]
    if (offline)
    {
        LOG_INFO(std::string("[Engine] ") + (feed_.feed_override ? "피드 주입" : "리플레이") +
                 " — KIS 없이 기동(주문·잔고는 모의 체결기)");
        return true;
    }

    feed_.kis = std::make_unique<KisClient>(kis_config_);

    if (!feed_.kis->authenticate())
    {
        LOG_ERROR("[Engine] KIS 인증 실패");
        return false;
    }

    // 시세 전용 클라이언트(실전 도메인) — 모의 시세 REST가 HTTP 500이므로 시세만 실전으로 조회.
    //  실패해도 feed_.kis(모의)로 폴백하되, 모의 시세는 500이라 사실상 틱이 안 나옴을 경고.
    //  WS 모드에서도 만들어 둔다: WS가 죽어 폴링으로 낮출 때 쓸 시세 소스가 그때 가서는 없으면
    //  폴백이 무의미해진다(모의 도메인으로 폴링하면 500만 쌓인다).
    //  근거: 실측 — Quant/logs/archive/quant_trader_2026-08-06.log 509행(openapivts 시세 HTTP 500), docs/DECISIONS.md D-097(2026-09-23).
    if (feed_.has_quote_kis)
    {
        feed_.quote_kis = std::make_unique<KisClient>(feed_.quote_kis_config);

        if (!feed_.quote_kis->authenticate())
        {
            LOG_ERROR("[Engine] 시세 클라이언트(실전) 인증 실패 — 모의 시세로 폴백(틱 없을 수 있음)");
            feed_.quote_kis.reset();
        }
        else
        {
            LOG_INFO("[Engine] 시세 클라이언트(실전 도메인) 인증 완료 — 현재가 폴링 소스");

            // 실계좌는 시세 키가 주문 키의 사본이다 — 초당 한도를 둘이 따로 세면 같은 키로 두 배가 나간다. [why D-138]
            if (feed_.quote_kis->share_rate_limit_with(*feed_.kis))
            {
                LOG_INFO("[Engine] 시세·주문 클라이언트가 같은 앱키 — 초당 호출 한도를 하나로 센다");
            }
        }
    }

    return true;
}

void Engine::setup_paper_executor(bool offline)
{
    // KIS가 없으면 주문·잔고는 모의 체결기가 받는다. [why D-071]
    if (offline)
    {
        feed_.paper = std::make_unique<feed::PaperExecutor>(feed_.replay_cash, symbols_.table);
        LOG_INFO("[Engine] 모의 체결기: 현금 " + std::to_string(static_cast<long long>(feed_.replay_cash)) + "원");
    }
}

void Engine::initialize_order_router()
{
    IOrderExecutor& executor = feed_.paper ? static_cast<IOrderExecutor&>(*feed_.paper) : *feed_.kis;

    // FEP OrderRouter 초기화
#ifdef HAS_ZMQ
    order_router_ = std::make_unique<OrderRouter>(order_gate_, executor, zmq_bridge_.get());
#else
    order_router_ = std::make_unique<OrderRouter>(order_gate_, executor);
#endif
    LOG_INFO("[Engine] OrderRouter (FEP) 초기화 완료");
    start_ops_server();

    // 이전 세션이 남긴 미체결 주문을 취소한다. 건당 왕복이 3~5초라 여기서 기다리면
    //  기동이 몇 분씩 멈춘다 — 목록을 읽고 부속 파일을 비우는 것만 여기서 하고 취소는
    //  별도 스레드가 이어서 낸다. 잔고 시드가 이를 기다릴 필요는 없다: 미체결 취소는
    //  보유수량을 바꾸지 않고 주문가능현금·매도가능수량만 푸는데 둘 다 주기 잔고 대조가
    //  다시 읽는다.
    order_router_->cancel_stale_orders_async();
}

void Engine::initialize_ledger_reconciler()
{
    // 잔고 → 장부 대조기. 브로커·라우터·종목명은 함수로 넘겨 대조기가 KisClient·OrderRouter를 모르게 한다. [why D-061]
    ledger_ = std::make_unique<LedgerReconciler>(order_gate_,
                                                 [this]
                                                 {
                                                     return feed_.paper ? feed_.paper->balance() : feed_.kis->get_balance_with_orderable_cash();
                                                 });
    ledger_->set_account_no(feed_.kis ? feed_.kis->account_no() : std::string("PAPER"));
    ledger_->set_baseline_directory(Logger::instance().base_directory()); // 실행 위치와 무관하게 로그 폴더와 같은 곳
    ledger_->set_name_sink([this](const std::string& ticker, const std::string& name)
    {
        register_ticker_name(ticker, name);
    });
    ledger_->set_reconcile_sink([this](const reconcile::Row& row)
    {
        order_router_->record_reconcile(row);
    });
}

void Engine::initialize_data_poller()
{
    // REST 현재가 폴러. 시세는 시세 전용 클라이언트가 있으면 그쪽(실전 도메인 초당 한도가 높다). [why D-062]
    //  [lock-order] 폴러 스레드는 pipeline_.trade_matrix의 WS 수신 스레드 행에 넣지 않는다 — 폴러의 틱은 자기 행(pipeline_.data_row())으로 간다.
    poller_ = std::make_unique<DataPoller>(
        [this](const std::string& ticker)
        {
            KisClient* quote_client = feed_.quote_kis ? feed_.quote_kis.get() : feed_.kis.get();
            return quote_client ? quote_client->get_current_price(ticker) : 0.0;
        },
        [this](TradeData trade)
        {
            trade.symbol_id       = lookup_symbol(trade.ticker);

            // 갈라 띄우면 샤드가 저쪽에 있다 — WS 수신 스레드와 같은 길로 통로에 넣고, 꺼내 가르는 일은
            //  전략 쪽 줄 스레드가 한다. 줄 번호는 행렬의 폴러 행과 같은 자리다(폴러 몫 한 줄).
            //  [inv] 이 줄에 넣는 스레드는 폴러의 조회 스레드 하나다 — SPSC가 그 위에 서 있다. [why D-114] [why D-138]
            //  단계 5부터 소켓과 넘침 폴러를 쥔 쪽은 시세 프로세스다 — 여기 넣는 쪽도 그쪽 하나다.
            if (role_ == ProcessRole::Feed)
            {
                push_feed_trade(pipeline_.data_row(), trade);
                return;
            }

            shard::for_each_shard(pipeline_.routes.mask(trade.symbol_id), pipeline_.trade_matrix.consumer_of(trade.symbol_id),
                                  [&](uint32_t consumer)
            {
                while (!pipeline_.trade_matrix.push_to(pipeline_.data_row(), consumer, trade) && running_.load(std::memory_order_acquire))
                {
                    std::this_thread::sleep_for(1ms);
                }

                pipeline_.shards[consumer]->wake().notify();
            });
        });
    poller_->set_keep_going([this]
    {
        return running_.load(std::memory_order_acquire);
    });
    // 넘침·폴백 조회는 30종목씩 멀티종목 시세 한 건으로 받는다. 실패한 묶음은 폴러가 한 종목씩 다시 받는다.
    //  첫 실패만 KIS 오류 코드를 남긴다(모의 도메인 지원 여부를 공식 자료로 확인하지 못했다). [why D-150]
    poller_->set_batch_quote(
        [this, failure_logged = false](const std::vector<std::string>& tickers) mutable -> std::optional<std::vector<double>>
        {
            KisClient* quote_client = feed_.quote_kis ? feed_.quote_kis.get() : feed_.kis.get();

            if (!quote_client)
            {
                return std::nullopt;
            }

            auto prices = quote_client->get_current_prices(tickers);

            if (!prices)
            {
                if (!failure_logged)
                {
                    failure_logged = true;
                    LOG_WARN("[Engine] 멀티종목 시세 조회 실패 - error(" + error_text(prices) + ") paper(" +
                             std::string(quote_client->is_paper() ? "true" : "false") + ")");
                }

                return std::nullopt;
            }

            return std::move(*prices);
        },
        KisClient::kMultiPriceMax);
}

bool Engine::try_open_ledger_journal()
{
    auto& ledger = order_gate_.ledger();

    if (ledger_journal_directory_.empty())
    {
        return true;
    }

    // 단일 계좌 키("")로 적히는 레코드에 실을 계좌 번호 — DB fills·orders·positions가 계좌를 가르는 칸이다.
    ledger.set_journal_account(feed_.kis ? feed_.kis->account_no() : std::string("PAPER"));

    // 오늘 파일을 열고 처음부터 다시 적용한다 — 재기동 전 선점·체결·대조가 장부에 되살아난다. 못 열면 장부 없이
    //  주문이 나가는 셈이라 기동을 거부한다(감시견이 다시 띄운다). [why D-113]
    if (!ledger.set_journal(utf8::path_from_utf8(ledger_journal_directory_), kst::date_yyyymmdd(std::time(nullptr)),
                                 ledger_journal_fsync_))
    {
        LOG_ERROR("[Engine] 장부 저널을 못 열어 기동하지 않는다: " + ledger_journal_directory_);
        return false;
    }

    const auto& replay = ledger.journal_replay();
    LOG_INFO("[Engine] 장부 저널 리플레이: " + std::to_string(replay.applied) + "건 (마지막 seq " +
             std::to_string(replay.last_sequence) + (replay.truncated_tail ? ", 꼬리 잘림" : "") + ")");
    return true;
}

// 저널 리플레이가 남긴 미결 주문 — 엔진이 죽은 순간 호가창에 살아 있었을 주문이다. 잔고 시드가 끝난 뒤에 부른다:
//  체결로 닫힌 주문은 시드가 이미 보유에 반영했고, 여기서는 주문 쪽(이력·선점)만 맞춘다. [why D-113]
void Engine::resolve_open_intents()
{
    auto& ledger = order_gate_.ledger();
    order_router_->continue_order_numbers(ledger.highest_order_id());

    const auto intents = ledger.open_intents();

    if (intents.empty())
    {
        LOG_INFO("[Engine] 장부 미결 주문 대조: 되살림 0건 · 선점해제 0건 · 저널기록실패 " +
                 std::to_string(ledger.journal_failures()) + "건");
        return;
    }

    const auto adopted = order_router_->adopt_open_intents(intents);
    LOG_INFO("[Engine] 장부 미결 주문 대조: 되살림 " + std::to_string(adopted.restored) + "건 · 선점해제 " +
             std::to_string(adopted.released) + "건 · 저널기록실패 " + std::to_string(ledger.journal_failures()) + "건");
}

bool Engine::try_bootstrap_ledger()
{
    // G5: 실계좌 보유분을 장부에 시드 (스레드 시작 전, 단일스레드 구간)
    if (!bootstrap_ledger_)
    {
        return true;
    }

    if (ledger_->bootstrap())
    {
        return true;
    }

    // running_이 아직 false라 main 루프가 바로 빠지고, 감시자가 5초 뒤 다시 띄운다.
    LOG_ERROR("[Engine] 장부 없이 기동하지 않는다 — 프로세스 종료");
    return false;
}
