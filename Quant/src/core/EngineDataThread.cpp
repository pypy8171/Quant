// 데이터 수집 스레드 — 장 시작 감지·잔고 대조·일봉·지수·수급 조회를 한 사이클씩 돈다.
//  Engine 클래스는 그대로다. Engine.cpp 가 4,700줄을 넘겨 열기 어려워 이 스레드 본체만 따로 낸 것이다
//  (헤더는 한 줄도 안 바뀐다 — 같은 Engine 의 멤버 함수 본체가 여기 있을 뿐이다).
//
//  ── 부르는 자리 ──────────────────────────────────────────────────────────
//  data_thread_fn() : spawn_threads() 가 data 스레드로 띄운다. 맡는 일감은 프로세스 역할(주문·전략·시세)로 갈린다.
//  daily_bars_needed() : data_thread_fn() 가 일봉 조회 전에

#include "core/Engine.h"
#include "core/LatencyTrace.h"
#include "utils/Logger.h"
#include "utils/ThreadName.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <nlohmann/json.hpp>

using namespace std::chrono_literals;

// ─── 데이터 수집 스레드 ───────────────────────────────────────────────────
void Engine::data_thread_fn(std::stop_token stop_token)
{
    thread_name::set_current("DataThread");
    LOG_INFO("[DataThread] 시작");

    // 맡는 일감은 역할로 갈린다. [inv] 역할은 start() 전에 정해지고 도는 동안 바뀌지 않는다. [why D-114]
    const bool strategy_side = runs_strategy_side();
    const bool order_side    = runs_order_side();
    const bool feed_side     = runs_feed_side();

    bool was_market_open = false;

    while (!stop_token.stop_requested())
    {
        bool market_now = is_any_market_open();

        // 장 시작 감지 → 일별 카운터 리셋 + 국면 판정
        //  "어느 시장이든 닫힘→열림" 전이라 KR 09:00과 US 22:30(KST) 두 번 발화한다. 22:30 리셋은
        //  그날 KR 손익 기록을 지우지만 그 시각 KR 주문은 나가지 않는다(W-9, 시장별 분리는 보류).
        if (market_now && !was_market_open)
        {
            // 하루치를 새로 여는 것은 주문 쪽 제 주기다 — 게이트·원장·라우터가 거기 있다. [why D-114]
            if (order_side)
            {
                request_reset_daily();
                LOG_INFO(std::string("[DataThread] 장 개장 전이(") + (is_kr_market_open() ? "KR" : "US") +
                         ") — OrderGate 일별 카운터 리셋");
            }

            // 기동 뒤 첫 개장이면 아직 라벨 전이가 없었을 수 있다. 마지막 선택을 강제 로그로 다시 적용해
            //  "오늘 무엇이 켜져 있나"가 하루 한 줄은 남게 한다.
            if (strategy_side && strategy_.last_selected_regime != Regime::UNKNOWN)
            {
                apply_regime_selection(strategy_.last_selected_regime, /*force_log=*/true);
            }
        }

        was_market_open = market_now;

        if (!market_now)
        {
            wake::sleep_unless_stopped(stop_token, 60s); // 정지 요청이면 바로 깬다 — 장 외 종료가 60초를 기다리지 않는다
            continue;
        }

        // 계측(문항 2): 사이클 본체가 재스캔 주기(슬라이스)보다 길면 재스캔은 그만큼 늦는다.
        //  본체 안에서 어느 단계가 먹는지(재스캔·잔고 대조·시세 보충) 같이 잰다.
        using cycle_clock = std::chrono::steady_clock;
        const auto cycle_start = cycle_clock::now();
        auto ms_between = [](cycle_clock::time_point from, cycle_clock::time_point to) -> long long
        { return std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count(); };
        long long rescan_ms = 0, reconcile_ms = 0, top_up_ms = 0;

        try
        {
            // 매크로 레짐 게이트: 보조 프로세스가 쓴 regime.json → OrderGate entry_halt 토글.
            //  재스캔/잔고 대조와 같은 "사이클 1회" 계층. rest·일봉 모드 공통 경로라 두 모드 다 커버.
            //  국면을 읽는 것은 전략 쪽이다 — 게이트를 고치는 일은 제어 요청으로 주문 쪽에 넘어간다. [why D-114]
            if (strategy_side)
            {
                poll_regime_file();
            }

            // 주기적 유니버스 재스캔(동적 등록) — 슬리브별 주기는 각 job이 자체 판단한다.
            if (strategy_side && !universe_rescan_.jobs.empty())
            {
                {
                    const auto rescan_start = cycle_clock::now();
                    maybe_rescan_universe();
                    rescan_ms = ms_between(rescan_start, cycle_clock::now());

                    // G1: 재스캔으로 새로 등록된 전략도 현재 국면 선택에 맞춰 즉시 게이팅
                    //  (기본 active_=true로 잘못된 국면에 진입하는 창을 닫는다). 국면 불변이라
                    //  force_log=false → 로그 노이즈 없음.
                    if (strategy_.last_selected_regime != Regime::UNKNOWN)
                    {
                        apply_regime_selection(strategy_.last_selected_regime, /*force_log=*/false);
                    }
                }
            }

            const bool rest_now = feed_.rest_feed_active.load(std::memory_order_relaxed);

            // 매 사이클 잔고 대조. 폴링 모드는 원장까지 덮어쓰고(체결콜백 부재 보완),
            //  WS 모드는 총평가금·일손익만 갱신한다. WS 모드에서 이걸 건너뛰면 equity가 0에
            //  머물러 총노출 게이트가 조용히 통과만 하고, 일간손실 한도의 기준값도 안 움직인다.
            //  잔고 조회 자체는 대조기가 뒤 스레드에서 돌리고 여기서는 짧게만 기다리므로(기본 500ms) 서버가
            //  늦어도 아래 재선점 정리·시세 보충은 제때 돈다. 늦은 응답은 다음 사이클이 집는다.
            //  원장과 라우터는 주문 프로세스 것이라 대조·선점 정리도 그쪽 제 주기로 돈다. [why D-114]
            if (order_side)
            {
                const auto reconcile_start = cycle_clock::now();
                ledger_->reconcile(/*resync_positions=*/rest_now, std::time(nullptr));
                reconcile_ms = ms_between(reconcile_start, cycle_clock::now());

                // 잔고가 실보유를 바로잡는 자리 옆에서, 라우터가 선점을 바로잡는다. 정본이 서로
                //  다르다 — 실보유는 브로커, 선점은 라우터 이력. 둘 다 슬롯을 세므로 같이 돈다.
                if (order_router_)
                {
                    order_router_->sweep_stale_reservations();
                }
            }

            // 틱이 끊긴 보유 종목은 REST 현재가로 보충한다 — 운영단말 현재가가 비어 있던 원인(09-11)은
            //  둘이었다: 유니버스 밖 보유(구독 자체가 없음)와 WS 구독 상한에 밀린 종목. 구독 여부를 따지지
            //  않고 "최근 틱이 없다"로만 고르면 둘 다 잡힌다. 전략이 볼 일은 없으니 td_queue_에는 넣지 않는다.
            //  모의 도메인 초당 한도가 낮아 300ms 간격. 시세를 채우는 일이라 전략 쪽이 한다. [why D-114]
            if (strategy_side)
            {
                std::vector<std::string> held;

                // 사본을 본다 — 단계 5에서 시세가 딴 프로세스로 가면 여기서 주문 쪽 장부를 못 부른다. [why D-114]
                std::vector<symbol::SymbolId> ledger_ids;
                std::vector<ipc::LedgerRow>   ledger_rows;
                ipc::collect_all_rows(*ledger_snapshot_, ledger_ids, ledger_rows);

                for (size_t index = 0; index < ledger_rows.size(); ++index)
                {
                    if (ledger_rows[index].position > 0)
                    {
                        held.push_back(symbols_.table.name(ledger_ids[index]).string());
                    }
                }

                std::vector<std::string> stale = poller::select_stale(
                    held,
                    [this](const std::string& ticker) -> std::optional<std::chrono::steady_clock::time_point>
                    {
                        const int64_t at_ns = last_price_at_ns(ticker);

                        if (at_ns == 0)
                        {
                            return std::nullopt;
                        }

                        return std::chrono::steady_clock::time_point(std::chrono::nanoseconds(at_ns));
                    },
                    std::chrono::steady_clock::now() - std::chrono::seconds(60));

                const auto top_up_start = cycle_clock::now();
                poller_->top_up(stale, [this](const std::string& ticker, double price) { set_last_price(ticker, price); });
                top_up_ms = ms_between(top_up_start, cycle_clock::now());
            }

            // 시세를 받아 흘리는 자리는 전부 전략 쪽이다 — 현재가 폴링과 일봉이 그렇다. 수급·섹터·매크로
            //  관측 적재는 흘리는 일이 아니라 REST로 떠서 로그로만 남기는 것인데, 이것도 전략 쪽에 둔다:
            //  게이트로 승격하면(D-014·D-044) 읽는 쪽이 전략이라 경계를 한 번 더 넘지 않아도 되고,
            //  주문 쪽에 두면 관측 REST가 주문 전송과 같은 프로세스에서 KIS 호출을 다툰다. 관측은 게이트가
            //  아니라 로그라 전략이 내려간 동안 비어도 매매에는 영향이 없고, 감시견이 셋을 같이 다시
            //  띄우므로 끊김은 재기동으로 메워진다. [why D-114]
            if (rest_now && strategy_side)
            {

                // ── 당일 외국인·기관 추정 순매수 "관측 적재"(게이트 아님) ──────────────
                //  data-sourcer 판정: FHPTJ04400000는 추정/가집계 → 부호·상대크기만 신뢰.
                //  게이트로 승격 전, 매 사이클 스냅샷을 로그로 남겨 장중추정 vs 장후확정을
                //  나중에 대조한다(지금 안 남기면 영구 소실). 레이트 절약 위해 5분마다만.
                //  ⚠️ 스키마 미확정 — 첫 성공 응답의 원문 로그로 필드명 확정할 것.
                {
                    static int est_flow_tick = 0;
                    // 수급추정(EstInvestorFlow) 로그 주기. 폴 간격(fetch_interval_sec_)이
                    //  30초일 때 10틱이면 약 5분마다다. 폴 간격을 바꾸면 실제 분 주기도 바뀐다.
                    constexpr int kEstFlowLogEveryNTicks = 10;
                    KisClient* equity_quote_client = feed_.quote_kis ? feed_.quote_kis.get() : feed_.kis.get();

                    if (equity_quote_client && (est_flow_tick % kEstFlowLogEveryNTicks) == 0)
                    {
                        // 우리 유니버스(watch) 티커 집합 — 교집합만 강조 로깅.
                        std::vector<bool> ours(symbols_.table.capacity(), false);

                        for (const auto& watch_specification : watch_specifications_)
                        {
                            if (watch_specification.market == Market::KR)
                            {
                                const symbol::SymbolId symbol = symbols_.table.lookup(watch_specification.ticker);

                                if (symbol != symbol::kNone)
                                {
                                    ours[symbol] = true;
                                }
                            }
                        }

                        auto buy_top  = equity_quote_client->fetch_est_investor_ranking("0000", "0", "0"); // 순매수 상위
                        std::this_thread::sleep_for(150ms);
                        auto sell_top = equity_quote_client->fetch_est_investor_ranking("0000", "1", "0"); // 순매도 상위

                        // 로그 축소: 전체시장 30행 덤프 대신 "우리 유니버스(★) 교집합만" 남긴다.
                        //  fetch(관측 적재)는 그대로 — 로그 볼륨만 스냅샷당 ~61줄→1~4줄로 줄인다.
                        //  전체 랭킹 아카이브가 필요하면 별도 보조 프로세스(logs/supply_*.csv)로 후속 분리.
                        auto dump = [&](const char* label,
                                        const std::vector<KisClient::EstInvestorFlow>& values) -> int
                        {
                            int shown = 0;

                            for (const auto& flow_f : values)
                            {
                                const symbol::SymbolId flow_symbol = symbols_.table.lookup(flow_f.ticker); // 응답 티커는 문자열

                                if (flow_symbol == symbol::kNone || !ours[flow_symbol])
                                {
                                    continue; // 우리 종목만 로깅
                                }

                                int rank = 0;

                                for (const auto& flow_g : values) { ++rank; if (flow_g.ticker == flow_f.ticker) break; }
                                LOG_INFO(std::string("[수급추정] ") + label + " ★" + flow_f.ticker + " " +
                                         flow_f.name + " (전체 " + std::to_string(rank) + "위)" +
                                         " 외인=" + std::to_string(flow_f.foreign_net_quantity) +
                                         " 기관=" + std::to_string(flow_f.institution_net_quantity) +
                                         " 외인금액=" + std::to_string(static_cast<int64_t>(flow_f.foreign_net_amount)));
                                ++shown;
                            }

                            return shown;
                        };
                        LOG_INFO("[수급추정] 스냅샷(관측) 매수상위 " + std::to_string(buy_top.size()) +
                                 "행·매도상위 " + std::to_string(sell_top.size()) + "행 수신");
                        int buy_top_count = dump("매수상위", buy_top);
                        int sell_top_count = dump("매도상위", sell_top);

                        if (buy_top_count + sell_top_count == 0)
                        {
                            LOG_INFO("[수급추정]   (우리 유니버스가 외인·기관 상위권 미포함)");
                        }
                    }

                    ++est_flow_tick;
                }

                // ── 섹터(업종) 강약 모니터(관측용) ─────────────────────────────────
                //  업종 지수 등락률을 강→약으로 로깅해 "오늘 어느 섹터가 주도하나"를 눈으로 본다.
                //  코드는 ThemeStrategy.h KOSPI_SECTORS와 동일(실전 시세키로 조회, 5분 주기).
                //  get_index_price(업종코드): inquire-index-price(FID_MRKT_DIV=U) → 등락률.
                {
                    static int sector_tick = 0;
                    KisClient* sqc = feed_.quote_kis ? feed_.quote_kis.get() : feed_.kis.get();

                    if (sqc && (sector_tick % 10) == 0) // 30s×10 ≈ 5분
                    {
                        // KRX 정본 업종코드. 2026-09-08 구성종목으로 확증했다 — 직전 표는 이름이
                        //  통째로 밀려 있어(0017을 "통신업"으로 불렀는데 실제 구성은 한국전력·
                        //  한국가스공사·지역난방공사, 즉 전기가스업) 관측 로그가 매일 거짓말을 했다.
                        //  0022(은행)·0023은 금융업 통합으로 폐지돼 지수가 0.00이라 뺐다.
                        //  0001~0004(종합·대형·중형·소형주)는 업종이 아니라 규모별 집계라 뺀다.
                        static const std::vector<std::pair<std::string, std::string>> kSectors = {
                            {"0005", "음식료품"},   {"0006", "섬유의복"}, {"0007", "종이목재"},
                            {"0008", "화학"},       {"0009", "의약품"},   {"0010", "비금속광물"},
                            {"0011", "철강금속"},   {"0012", "기계"},     {"0013", "전기전자"},
                            {"0014", "의료정밀"},   {"0015", "운수장비"}, {"0016", "유통업"},
                            {"0017", "전기가스업"}, {"0018", "건설업"},   {"0019", "운수창고"},
                            {"0020", "통신업"},     {"0021", "금융업"},   {"0024", "증권"},
                            {"0025", "보험"},       {"0026", "서비스업"}};

                        struct SecRate { std::string name; double rate; double price; };
                        std::vector<SecRate> sectors;

                        for (const auto& [code, name] : kSectors)
                        {
                            std::this_thread::sleep_for(100ms); // rate limit 여유(20업종×100ms=2초)
                            auto index_price = sqc->get_index_price(code);

                            if (index_price.price > 0.0)
                            {
                                sectors.push_back({name, index_price.change_rate, index_price.price});
                            }
                        }

                        std::ranges::sort(sectors, std::ranges::greater{}, &SecRate::rate);

                        // 폭(breadth) 한 줄. 지수 등락률 하나로는 "지수는 빠졌는데 업종 절반이
                        //  플러스"인 회복 초입과 전 업종이 같이 밀리는 진짜 위험회피를 구분할 수
                        //  없다. 지금은 관측 전용이다 — 어떤 판정에도 쓰지 않는다. 회복일과
                        //  데드캣을 사후에 갈라 볼 표본이 쌓이기 전에는 게이트로 승격하지 않는다.
                        //  [why D-033]
                        if (!sectors.empty())
                        {
                            int up = 0;

                            for (const auto& sector : sectors)
                            {
                                if (sector.rate > 0.0)
                                {
                                    ++up;
                                }
                            }

                            // 정렬이 끝난 뒤라 중앙값은 가운데 원소다(짝수면 두 값의 평균).
                            const size_t count = sectors.size();
                            double median = (count % 2 == 1)
                                             ? sectors[count / 2].rate
                                             : (sectors[count / 2 - 1].rate + sectors[count / 2].rate) / 2.0;
                            char head[128];
                            std::snprintf(head, sizeof(head),
                                          "[섹터] 폭 %d/%zu 플러스, 중앙값 %+.2f%%", up, count, median);
                            LOG_INFO(head);
                        }

                        LOG_INFO("[섹터] ── 업종 등락률(강→약, 관측용) ──");

                        for (const auto& sector : sectors)
                        {
                            char line[128];
                            std::snprintf(line, sizeof(line), "[섹터] %-8s %+6.2f%%  지수=%.2f",
                                          sector.name.c_str(), sector.rate, sector.price);
                            LOG_INFO(line);
                        }
                    }

                    ++sector_tick;
                }

                // ── 매크로 지표 모니터(관측용) ─────────────────────────────────────
                //  환율·미국지수·미국채10Y금리는 도메스틱 KIS 밖 → 보조 프로세스(macro_regime_feed.py)가
                //  FinanceDataReader로 계산해 regime.json에 쓴 components를 그대로 로깅한다.
                //  키 이름은 NQ_F·ES_F지만 실제 소스는 현물지수 일봉(IXIC·US500)이다. 이 환경에서
                //  yfinance가 전 심볼 실패해 2026-09-04에 FDR로 갈아탄 결과다. 그래서 5개 중 4개는
                //  KST 09:00~15:30 내내 값이 고정된다 — 이 줄들을 장중 신호로 읽지 않는다. [why D-033]
                //  regime.json 미존재(보조 프로세스 미실행) 시 조용히 스킵. entry_halt 게이트와 독립.
                {
                    static int macro_tick = 0;

                    if (!regime_file_.empty() && (macro_tick % 10) == 0) // 30s×10 ≈ 5분
                    {
                        std::error_code mec;

                        if (std::filesystem::exists(regime_file_, mec) && !mec)
                        {
                            try
                            {
                                std::ifstream mf(regime_file_);
                                nlohmann::json regime_json;

                                if (mf)
                                {
                                    mf >> regime_json;
                                }

                                if (regime_json.contains("components"))
                                {
                                    std::string reg = regime_json.value("regime", std::string("?"));
                                    int score = regime_json.value("risk_score", 0);
                                    bool valid = regime_json.value("valid", false);
                                    LOG_INFO("[매크로] ── regime=" + reg + " score=" +
                                             std::to_string(score) +
                                             (valid ? "" : " (valid=false)") + " ──");
                                    // 관심 순서: 환율·나스닥선물·미국채10Y·(참고)S&P선물·VIX
                                    static const std::vector<std::pair<std::string, std::string>> kMacro = {
                                        {"USDKRW", "환율(USD/KRW)"}, {"NQ_F", "나스닥선물"},
                                        {"TNX10", "미국채10Y금리"},  {"ES_F", "S&P500선물"},
                                        {"VIX", "VIX"}};
                                    auto& comps = regime_json["components"];

                                    for (const auto& [key, label] : kMacro)
                                    {
                                        if (!comps.contains(key))
                                        {
                                            continue;
                                        }

                                        auto& component = comps[key];

                                        if (component.contains("pct") && !component["pct"].is_null())
                                        {
                                            double percent = component["pct"].get<double>();
                                            double price = (component.contains("price") && !component["price"].is_null())
                                                               ? component["price"].get<double>() : 0.0;
                                            int vote = component.value("vote", 0);
                                            char line[160];
                                            std::snprintf(line, sizeof(line),
                                                          "[매크로] %s  %+.2f%%  price=%.2f  vote=%+d",
                                                          label.c_str(), percent, price, vote);
                                            LOG_INFO(line);
                                        }
                                        else
                                        {
                                            LOG_INFO("[매크로] " + label + " = NA(데이터 없음)");
                                        }
                                    }
                                }
                            }
                            catch (const std::exception&)
                            {
                                // 원자적 write라 정상은 완전한 json — 부분/손상은 다음 틱 재시도.
                            }
                        }
                        else if (macro_tick == 0)
                        {
                            LOG_INFO("[매크로] regime.json 없음 — 매크로 표시엔 보조 프로세스"
                                     "(macro_regime_feed.py) 실행 필요");
                        }
                    }

                    ++macro_tick;
                }

            }
            else if (strategy_side)
            {
                // 차트(일봉) TR은 모의 도메인에서 HTTP 500을 돌려준다. 주문 클라이언트로 부르면
                //  종목 수×사이클마다 500이 쌓여 로그가 그걸로 덮인다(3회 재시도까지 붙는다).
                //  위 rest 분기와 같이 시세 클라이언트로 부른다.
                KisClient* quote_client = feed_.quote_kis ? feed_.quote_kis.get() : feed_.kis.get();

                // 일봉을 받아 쓰는 전략이 하나도 없으면 폴링 자체를 건너뛴다. DevScale·ITB처럼
                //  호가·체결 이벤트로만 도는 구성에서는 이 루프가 종목 수만큼 차트 TR을 매 사이클
                //  때리고 결과는 아무도 안 본다. 그 호출량이 초당 한도를 밀어 다른 조회(3분봉·현재가)까지
                //  500으로 떨어뜨린다. 전략 집합은 국면 전환으로 바뀌므로 매 사이클 다시 확인한다.
                if (quote_client && daily_bars_needed())
                {
                    for (const auto& specification : watch_specifications_)
                    {
                        std::vector<MarketData> bars;

                        if (specification.market == Market::KR)
                        {
                            // 여기만 당일 봉이 목적이다(파이프라인에 오늘 시세를 흘린다).
                            //  지표·기준점 용도의 다른 호출자는 전부 기본값(전일까지)을 쓴다.
                            bars = quote_client->get_daily_ohlcv(specification.ticker, 1, /*include_today=*/true);
                        }
                        else
                        {
                            bars = quote_client->get_us_daily_ohlcv(specification.ticker, 1, specification.exchange);
                        }

                        if (bars.empty())
                        {
                            continue;
                        }

                        auto& market_data = bars[0];
                        market_data.bar_index = static_cast<int>(data_count_.load());
                        market_data.symbol_id       = lookup_symbol(market_data.ticker);

                        if (feed_.capture)
                        {
                            feed_.capture->on_bar(market_data, feed::kDailyBarSeconds); // 리플레이가 일봉 전략도 재현하도록 [why D-071]
                        }

                        shard::for_each_shard(pipeline_.routes.mask(market_data.symbol_id), pipeline_.bars_matrix.consumer_of(market_data.symbol_id),
                                              [&](uint32_t consumer)
                        {
                            while (!pipeline_.bars_matrix.push_to(0, consumer, market_data) && !stop_token.stop_requested())
                            {
                                std::this_thread::sleep_for(1ms);
                            }

                            pipeline_.shards[consumer]->wake().notify();
                        });
                        ++data_count_;
                    }
                }

            }

            // 폴링 두 가지는 시세 소켓을 쥔 쪽 일감이다 — 넘침 목록이 그 소켓에만 있고, WS가 죽어 REST로
            //  낮출지를 아는 것도 그쪽뿐이다. 한 프로세스로 돌면 셋이 다 참이라 예전과 같은 자리에서 돈다.
            //  [why D-114 단계 5]
            if (feed_side && poller_)
            {
                if (rest_now)
                {
                    // REST 현재가 폴링 → TradeData(WS on_trade 경로 대체). 깨진 일봉(G1/G2) 대신 살아있는
                    //  get_current_price를 쓰고, ITB는 이 틱으로 1분 버킷 채널을 구성/스탑 평가한다.
                    //  종전엔 WS와 같은 큐에 넣었는데, WS 폴백 중 WS가 되살아나면 생산자가 둘이 됐다 — 폴러의
                    //  싱크는 행렬의 데이터 스레드 행이라 그 경우가 없다. [why D-062]
                    data_count_ += poller_->poll_universe(watch_specifications_, std::time(nullptr));
                }
                else if (feed_.websocket)
                {
                    // WS 상한에 밀린 종목은 REST로 받는다. 칸 복귀는 rebalance_websocket_slots()가 우선순위로 정한다 — 여기서
                    //  먼저 잡으면 넘침 목록에 먼저 선 종목이 보유 종목보다 칸을 먼저 가져간다. [why D-132]
                    //  REST 폴백이 도는 사이클에는 부르지 않는다(그쪽이 이미 전 종목을 폴링한다).
                    data_count_ += poller_->poll_overflow(feed_.websocket->take_overflow_specifications(), {}, std::time(nullptr));
                }
            }
        }
        catch (const std::exception& exception)
        {
            LOG_ERROR("[DataThread] 예외: " + std::string(exception.what()));
        }

        {
            const long long body_ms = ms_between(cycle_start, cycle_clock::now());
            const std::string line = "[DataThread] 사이클 계측: 본체=" + std::to_string(body_ms) + "ms (재스캔=" +
                                     std::to_string(rescan_ms) + "ms 잔고대조=" + std::to_string(reconcile_ms) +
                                     "ms 시세보충=" + std::to_string(top_up_ms) + "ms)";

            // 이 값을 넘긴 사이클만 INFO — 재스캔 주기(20초)를 갉아먹기 시작하는 값이다. 나머지는 DEBUG.
            constexpr long long kSlowCycleLogMs = 2000;

            if (body_ms >= kSlowCycleLogMs)
            {
                LOG_INFO(line);
            }
            else
            {
                LOG_DEBUG(line);
            }
        }

        // 사이클 tail 대기 — 재스캔 주기가 사이클보다 짧으면 그 간격으로 잘게 깨어난다.
        //  maybe_rescan_universe()는 이 루프 안에서만 불리므로, 그냥 두면 재스캔 주기를
        //  아무리 줄여도 fetch_interval_sec_ 단위로 반올림된다(30초 사이클 + 20초 재스캔 = 30초).
        //  잔고 대조·REST 폴백 폴링은 KIS REST를 쓰므로 사이클 주기 그대로 두고,
        //  일봉 캐시와 시세 파일만 보는 재스캔만 앞당긴다.
        {
            const int cycle = fetch_interval_sec_ > 0 ? fetch_interval_sec_ : 1;
            int slice = cycle;

            for (const auto& job : universe_rescan_.jobs)
            {
                if (job.interval_sec > 0 && job.interval_sec < slice)
                {
                    slice = job.interval_sec;
                }
            }

            if (slice < 1)
            {
                slice = 1;
            }

            int slept = 0;

            while (slept < cycle && !stop_token.stop_requested())
            {
                const int step = (slice < cycle - slept) ? slice : (cycle - slept);

                if (!wake::sleep_unless_stopped(stop_token, std::chrono::seconds(step)))
                {
                    break;
                }

                slept += step;

                if (strategy_side && slept < cycle)
                {
                    maybe_rescan_universe();   // 사이클 시작의 호출과 합쳐 재스캔 주기를 지킨다
                }
            }
        }
#ifdef HAS_ZMQ
        if (zmq_bridge_)
        {
            ZmqBridge::HealthSnapshot snapshot;
            snapshot.data_count   = data_count_.load();
            snapshot.signal_count = signal_count_.load();
            snapshot.order_count  = order_count_.load();

            for (const auto& shard : pipeline_.shards)   // 샤드마다 셀 하나 — 가장 높았던 값만 싣는다
            {
                snapshot.shard_high_water = std::max<uint64_t>(snapshot.shard_high_water, shard->high_water());
            }

            snapshot.shard_capacity         = ShardPipeline::kTickCellCapacity;
            snapshot.shard_out_size         = pipeline_.shard_out.size();
            snapshot.shard_out_capacity     = pipeline_.shard_out.capacity();
            snapshot.order_queue_high_water = pipeline_.order_high_water.load(std::memory_order_relaxed);
            snapshot.order_queue_capacity   = pipeline_.requests->capacity();
            snapshot.fill_queue_high_water  = pipeline_.fill_queue.high_water();
            snapshot.fill_queue_capacity    = pipeline_.fill_queue.capacity();
            snapshot.shard_dropped          = pipeline_.shard_dropped.load(std::memory_order_relaxed);
            snapshot.order_dropped          = pipeline_.order_dropped.load(std::memory_order_relaxed);
            snapshot.order_stale            = pipeline_.order_stale.load(std::memory_order_relaxed);
            snapshot.fill_dropped           = pipeline_.fill_dropped.load(std::memory_order_relaxed);
            snapshot.latency_samples        = pipeline_latency_.total.count();
            snapshot.tick_to_signal_p50_us  = pipeline_latency_.tick_to_signal.percentile(0.50);
            snapshot.tick_to_signal_p99_us  = pipeline_latency_.tick_to_signal.percentile(0.99);
            snapshot.signal_to_pop_p50_us   = pipeline_latency_.signal_to_pop.percentile(0.50);
            snapshot.signal_to_pop_p99_us   = pipeline_latency_.signal_to_pop.percentile(0.99);
            snapshot.pop_to_done_p50_us     = pipeline_latency_.pop_to_done.percentile(0.50);
            snapshot.pop_to_done_p99_us     = pipeline_latency_.pop_to_done.percentile(0.99);
            snapshot.total_p50_us           = pipeline_latency_.total.percentile(0.50);
            snapshot.total_p99_us           = pipeline_latency_.total.percentile(0.99);

            // 직전 사본과 빼 이번 구간만의 분포를 낸다 — 분위수끼리는 뺄 수 없어 버킷을 통째로 떠서 뺀다.
            //  사본은 이 스레드만 들고 있다(데이터 스레드 지역 상태). [inv] [why D-071]
            trace::PipelineSnapshot current;
            current.capture(pipeline_latency_);
            const auto names = trace::PipelineLatency::segment_names();

            for (int index = 0; index < trace::PipelineLatency::kSegmentCount; ++index)
            {
                snapshot.interval_segments[index] = {names[index],
                                                     trace::percentile_of_difference(
                                                         previous_latency_snapshot_.segments[index],
                                                         current.segments[index], 0.50),
                                                     trace::percentile_of_difference(
                                                         previous_latency_snapshot_.segments[index],
                                                         current.segments[index], 0.99)};
            }

            snapshot.interval_samples = current.segments.back().count -
                                        std::min(previous_latency_snapshot_.segments.back().count,
                                                 current.segments.back().count);
            previous_latency_snapshot_ = current;
            zmq_bridge_->publish_health(snapshot);
        }
#endif
    }

    LOG_INFO("[DataThread] 종료");
}

// 일봉을 원하는 전략이 하나라도 켜져 있는가 — 없으면 data_thread_fn 이 일봉 조회를 건너뛴다.
bool Engine::daily_bars_needed()
{
    std::lock_guard<std::mutex> lock(strategy_.mutex);

    for (const auto& strategy : strategy_.list)
    {
        if (strategy && strategy->is_active() && strategy->wants_daily_bars())
        {
            return true;
        }
    }

    return false;
}
