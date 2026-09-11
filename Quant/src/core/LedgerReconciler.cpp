// 브로커 잔고 → 원장 대조기 구현. Engine.cpp의 bootstrap_ledger·reconcile_from_balance를 옮긴 것으로,
//  판정 규칙은 그대로고 브로커·라우터·종목명 의존만 std::function으로 바뀌었다. [why D-061]
#include "core/LedgerReconciler.h"

#include "utils/Logger.h"

#include <fstream>
#include <thread>
#include <vector>

LedgerReconciler::LedgerReconciler(OrderGate& gate, FetchBalance fetch)
    : gate_(gate), fetch_(std::move(fetch))
{
}

// ─── G5: 실계좌 보유분 원장 부트스트랩 ──────────────────────────────────────
//  계좌키는 account=""(전략 신호의 기본 account_id와 일치, C-1). 평단까지 시드해야
//  매도 실현손익·일일손실 한도가 실제와 정합(C-2). reset_daily가 positions_/avg_prices_를
//  보존하므로 장 시작 리셋 후에도 유지 — 프로세스 기동당 1회면 충분.
bool LedgerReconciler::bootstrap(int attempts, std::chrono::milliseconds retry_delay)
{
    try
    {
        KisResult<AccountBalance> bal = KisResult<AccountBalance>::fail("init", "");

        for (int attempt = 0; attempt < attempts; ++attempt)
        {
            bal = fetch_();

            if (bal)
            {
                break;
            }

            LOG_WARN("[Engine] 원장 부트스트랩: 잔고 실패(" + bal.error_text() + ") — 재시도 " +
                     std::to_string(attempt + 1) + "/" + std::to_string(attempts));

            if (retry_delay.count() > 0)
            {
                std::this_thread::sleep_for(retry_delay);
            }
        }

        if (!bal)
        {
            LOG_ERROR("[Engine] 원장 부트스트랩: 잔고를 끝내 못 읽음 — 기동 중단");
            return false;
        }

        int n = 0;

        for (const Holding& h : bal->holdings)
        {
            // 주문가능수량(ord_psbl_qty)을 게이트에 함께 시드한다. 보유수량과 다르면(직전
            //  세션이 남긴 미체결 매도·미결제분) 전량 청산이 40240000으로 통째 거부돼
            //  한 주도 못 빠져나온다(09-08 047050 254주·381주 연속 거부). 필드가 없거나
            //  파싱 실패면 -1을 넘겨 "모름"으로 두고 보유수량을 그대로 쓴다.
            const int psbl_q = h.sellable_qty.value_or(-1);
            gate_.seed_position(std::string(), h.ticker, h.qty, h.avg_price, psbl_q);

            if (name_sink_)
            {
                name_sink_(h.ticker, h.name); // 로그 라벨(초기 보유분 종목명)
            }

            LOG_INFO("[Engine]   시드 " + h.ticker + " " + h.name + " " + std::to_string(h.qty) + "주 @평단 " +
                     std::to_string(static_cast<long long>(h.avg_price)) + " 주문가능=" +
                     (h.sellable_qty ? std::to_string(*h.sellable_qty) : std::string("(field없음)")));
            ++n;
        }

        LOG_INFO("[Engine] 원장 부트스트랩 완료: " + std::to_string(n) + "종목 시드");
        return true;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("[Engine] 원장 부트스트랩 예외: " + std::string(e.what()));
        return false;
    }
}

// ─── 보유분 재동기 — 실체결 원장을 실제 잔고로 강제 일치 ───────────────────
void LedgerReconciler::resync_holdings(const AccountBalance& bal, bool resync_positions)
{
    // 대조 행은 덮어쓰기·정리 "전" 원장 값으로 남긴다 — 덮어쓴 뒤에 재면 항상 일치로 나온다. [why D-038]
    std::vector<reconcile::Held> ledger_before;

    for (const auto& hp : gate_.snapshot_positions())
    {
        ledger_before.push_back(reconcile::Held{hp.ticker, hp.qty, hp.avg_price});
    }

    std::vector<reconcile::Held> broker_now;

    // 잔고는 서버 확정 스냅샷 → 미체결 선점(reserved_)을 통째로 비우고 실보유만 신뢰.
    //  체결피드(H0STCNI0) 부재로 누적된 H-1 드리프트(과잉 선점 → 정상신호 과잉차단)를
    //  동기화 시점마다 해소한다(#1). reset은 seed 재기록 전에 1회.
    //  체결통보가 오는 WS 모드에서는 건너뛴다 — 그쪽은 원장이 체결로 이미 갱신되고,
    //  reserved_에는 아직 살아 있는 지정가 주문이 잡혀 있어 비우면 재발주를 부른다.
    if (resync_positions)
    {
        gate_.reset_reserved();
    }

    // 잔고에 있는 종목을 모으면서, 재동기 모드면 원장까지 덮어쓴다.
    std::vector<std::string> held;

    for (const Holding& h : bal.holdings)
    {
        const std::string& code = h.ticker;
        const int    q  = h.qty;
        const double av = h.avg_price;
        held.push_back(code);
        broker_now.push_back(reconcile::Held{code, q, av});

        if (resync_positions)
        {
            gate_.seed_position(std::string(), code, q, av);
        }

        // 매도가능수량은 재동기 모드와 무관하게 매번 맞춘다(기동 시드 0 고착 해소).
        if (h.sellable_qty)
        {
            gate_.refresh_sellable(std::string(), code, *h.sellable_qty);
        }

        // 체결통보 모드는 원장을 덮어쓰지 않는다. 대신 재연결 사이에 빠진 매도 체결만
        //  잔고로 되메운다 — 두 번 연속 같은 부족분이 보이고 미체결 매도 이내일 때.
        //  (09-11 11:00 WS 끊김 4초에 248170 매도 52주 통보 유실 → 18분 유령 보유)
        if (!resync_positions)
        {
            const int absorbed = gate_.absorb_missed_sell(std::string(), code, q);

            if (absorbed > 0)
            {
                LOG_WARN("[Engine] 잔고 대조: " + code + " 원장이 잔고보다 " + std::to_string(absorbed) +
                         "주 많아 놓친 매도 체결로 보고 " + std::to_string(q) + "주로 맞춘다");
            }
        }
    }

    // 잔고에 없는데 원장에 남은 종목을 걷어낸다. 이 유령이 슬롯을 물고 있으면
    //  실제 보유가 20인데 "동시 보유 종목 한도 초과 (25 >= 25)"가 난다(09-09 관측).
    //  체결통보가 오는 WS 모드에서도 통보를 놓치면 같은 자리에 남으므로 두 모드 다 돈다.
    //  갓 열린 포지션은 잔고 왕복이 체결보다 빨랐을 수 있어 남긴다.
    //
    // [inv] held가 비면 한 종목도 걷어내지 않는다. 초당 한도(EGW00201)·전송 실패는 D-059부터
    //  fail 봉투로 걸러져 여기 오지 않지만, rt_cd="0"에 빈 output1로 오는 일시 오류가 없다고 장담할
    //  수 없어 가드는 남긴다. 빈 응답을 정본으로 믿고 지우면 원장이 통째로 날아가고 엔진은 미보유로
    //  읽어 같은 종목을 다시 산다(09-09 14:04, 재기동 직후 한도 폭주 중에 25종목 전부 정리됨).
    //  진짜로 빈 계좌라면 걷어낼 것도 없으니 건너뛰어 잃는 것이 없다.
    const auto gone = held.empty() ? std::vector<std::string>{} : gate_.prune_positions(held, prune_age_sec_);

    if (held.empty())
    {
        LOG_WARN("[Engine] 잔고 대조: output1이 비어 유령 정리를 건너뛴다 (잔고 조회 실패로 본다 — 원장 유지)");
    }

    if (!gone.empty())
    {
        std::string list;

        for (const auto& t : gone)
        {
            list += (list.empty() ? "" : ",") + t;
        }

        LOG_WARN("[Engine] 잔고 대조: 잔고에 없는 원장 보유 " + std::to_string(gone.size()) + "종목 정리 (" + list + ")");
    }

    // 어긋난 종목만 RECONCILE 행으로 원장 CSV에 남긴다(일치는 행 없음). held가 비면 잔고를 못 받은
    //  것이라 위에서 정리를 건너뛰었듯 대조도 건너뛴다 — 전 종목 "브로커 0"으로 찍히면 오독한다.
    if (!held.empty() && reconcile_sink_)
    {
        const auto rows = reconcile::plan(ledger_before, broker_now, resync_positions, gone,
                                          resync_positions ? "mode=REST" : "mode=WS");

        for (const auto& r : rows)
        {
            reconcile_sink_(r);
        }

        if (!rows.empty())
        {
            LOG_WARN("[Engine] 잔고 대조: 원장≠잔고 " + std::to_string(rows.size()) + "종목 → trades CSV RECONCILE 행 (" +
                     (resync_positions ? "REST 모드, 잔고로 덮어씀" : "WS 모드, 원장 유지") + ")");
        }
    }
}

// ─── 당일 총평가금 기준선 ───────────────────────────────────────────────────
//  손실컷 재시작 리셋 구멍 방지: 기준선을 거래일(KST)별 파일로 영속화. 같은 날 재시작 → 저장된
//  기준선 재사용(손실 한도 유지), 새 거래일 → 신규 캡처+저장. 장 시작(09:00)부터 연속 구동 시
//  파일이 그날 시가 기준선을 담아 당일손익이 정확.
void LedgerReconciler::capture_baseline(double tot_eval, std::time_t now_utc)
{
    const std::string ymd = ledger::kst_ymd(now_utc);
    std::filesystem::path bpath;

    if (!baseline_dir_.empty())
    {
        bpath = baseline_dir_ / ledger::baseline_file_name(ymd, account_no_);
    }

    double file_base = 0.0;
    bool   from_file = false;

    if (!bpath.empty())
    {
        std::ifstream bf(bpath);

        if (bf.is_open() && (bf >> file_base) && file_base > 0.0)
        {
            from_file = true;
        }
    }

    if (from_file)
    {
        baseline_ = file_base;
        LOG_INFO("[Engine] 기준선 파일 재사용(" + ymd + "): " + std::to_string(static_cast<long long>(baseline_)) +
                 "원 — 재시작해도 당일 손실컷 유지");
    }
    else
    {
        baseline_ = tot_eval;

        if (!bpath.empty())
        {
            std::error_code ec;
            std::filesystem::create_directories(baseline_dir_, ec); // 종전 Logger::path_for가 하던 일
            std::ofstream of(bpath, std::ios::trunc);

            if (of.is_open())
            {
                of << static_cast<long long>(baseline_) << "\n";
            }
        }

        LOG_INFO("[Engine] 기준선 신규 캡처+저장(" + ymd + "): 총평가금 " +
                 std::to_string(static_cast<long long>(tot_eval)) + "원");
    }

    have_baseline_ = true;
}

// ─── 주기 대조 ──────────────────────────────────────────────────────────────
//  rest_price_feed_ 모드는 체결콜백(OrderRouter::on_fill)이 미등록이라 positions_/daily_pnl_이
//  갱신되지 않는다(치명). 매 사이클 잔고를 재조회해 1) output1 보유분으로 positions_/avg_prices_
//  재동기, 2) output2 총평가금(tot_evlu_amt)의 당일 기준선 대비 델타를 daily_pnl_로 세팅한다.
//  절대 평가손익(evlu_pfls)이 아니라 "당일 기준선 델타"를 쓴다 — 이미 -30% 물린 미실현손실을
//  daily_pnl로 넣으면 개장 즉시 모든 신규매수가 막힌다.
void LedgerReconciler::reconcile(bool resync_positions, std::time_t now_utc)
{
    // 서킷브레이커: 잔고조회가 연속 실패 중이면 이번 사이클은 조회를 건너뛴다.
    if (breaker_.take_skip())
    {
        return;
    }

    bool responded = false; // 잔고 응답을 실제 파싱했는가(서킷브레이커 판정용)

    try
    {
        const KisResult<AccountBalance> bal = fetch_();

        if (!bal)
        {
            LOG_WARN("[Engine] 잔고 대조: 조회 실패(" + bal.error_text() + ")");
        }
        else
        {
            responded = true;
            resync_holdings(*bal, resync_positions);

            // 주문가능현금 — 매수 클램프의 진짜 상한. 가수도정산금액(실질 주문가능)을 우선 쓰고
            //  없으면 예수금총금액으로 떨어진다. 평가금과 달리 미체결 지정가와 미결제 매수로
            //  묶인 몫이 빠져 있어야 40250000 도배를 막을 수 있다.
            if (bal->available_cash && *bal->available_cash >= 0.0)
            {
                gate_.set_available_cash(*bal->available_cash);
            }

            // 2) 당일 총평가금 델타 → daily_pnl_. 요약 필드의 부재는 optional이 든다.
            if (bal->total_eval_amt)
            {
                const double tot_eval = *bal->total_eval_amt;

                if (!have_baseline_)
                {
                    capture_baseline(tot_eval, now_utc);
                }

                const double delta = tot_eval - baseline_;
                gate_.set_daily_pnl(delta); // 손실컷용(세션 앵커) — 리스크게이트 동작 유지
                gate_.set_equity(tot_eval); // 총노출 게이트(§3d) 분모 — 총평가금 스냅샷 갱신
                LOG_INFO("[Engine] 잔고 대조: 당일손익 " + std::to_string(static_cast<long long>(delta)) + "원 (총평가 " +
                         std::to_string(static_cast<long long>(tot_eval)) + ")");

                // 표시 전용: 전일 총자산(bfdy_tot_asst_evlu_amt) 대비 오늘 손익 — launch 시점과 무관하게
                //  "전일종가 대비 당일손익"을 찍는다. 손실컷 기준선(세션 앵커)과는 분리(리스크 동작 불변).
                if (bal->prev_day_total_asset && *bal->prev_day_total_asset > 0.0)
                {
                    const double day_delta = tot_eval - *bal->prev_day_total_asset;
                    LOG_INFO("[Engine] 당일손익(전일대비): " + std::to_string(static_cast<long long>(day_delta)) +
                             "원 (전일총자산 " + std::to_string(static_cast<long long>(*bal->prev_day_total_asset)) + ")");
                }
            }
        }
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("[Engine] 잔고 대조 예외: " + std::string(e.what()));
    }

    // 서킷브레이커 갱신 — 성공 시 즉시 복귀, 실패 시 지수 백오프. 지속 정체 동안 daily_pnl_은 낡아
    //  §4 손실컷이 트립하지 못하므로 그 창에서 BUY NEW를 보수 정지하고(SELL 청산은 통과) 복구 즉시 푼다.
    const ledger::BreakerOutcome out = breaker_.on_result(responded);

    if (out.log_recovered)
    {
        LOG_INFO("[Engine] 잔고조회 복구 — 잔고 대조 정상화");
    }

    if (out.log_stale_off)
    {
        LOG_INFO("[Engine] daily_pnl 갱신 복구 — 신규 진입 정지 해제(B2)");
    }

    if (out.log_backoff)
    {
        LOG_WARN("[Engine] 잔고조회 실패(streak=" + std::to_string(breaker_.fail_streak()) +
                 ", 12002 타임아웃 등) — 잔고 대조 " + std::to_string(out.skip_cycles) + "사이클 백오프(핫루프 보호)");
    }

    if (out.log_stale_on)
    {
        LOG_WARN("[Engine] daily_pnl 갱신 끊김(streak≥" + std::to_string(ledger::kPnlStaleStreak) +
                 ") — BUY NEW 보수 정지(B2), 손실컷 신뢰불가 창 방어");
    }

    if (out.pnl_stale)
    {
        gate_.set_pnl_stale(*out.pnl_stale);
    }
}
