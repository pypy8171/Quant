// 잔고 대조 차이 계산(core/ReconcilePlan.h) 단위 테스트. 원장≠잔고 네 갈래(수량 어긋남 REST/WS, 평단만 어긋남,
// 원장에만 있는 종목의 PRUNE/KEEP)와 "일치는 행 없음"을 고정한다. 헤더 전용이라 라우터·KIS 링크 없이 돈다.
// 관련 결정: D-038.
#include "core/ReconcilePlan.h"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

namespace
{

using reconcile::Held;
using reconcile::Row;

int g_pass = 0;

#define CHECK(cond)                                                                                                  \
    do                                                                                                               \
    {                                                                                                                \
        if (!(cond))                                                                                                 \
        {                                                                                                            \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #cond << "\n";                               \
            return 1;                                                                                                \
        }                                                                                                            \
        ++g_pass;                                                                                                    \
    } while (0)

// 행 목록에서 종목 하나를 찾는다. 순서가 결정적이지 않은 검사에 쓴다.
const Row* find_row(const std::vector<Row>& rows, const std::string& t)
{
    for (const auto& r : rows)
    {
        if (r.ticker == t)
        {
            return &r;
        }
    }

    return nullptr;
}

} // namespace

int main()
{
    // 1) 전부 일치 → 행 없음. 소수점 아래 평단 차이(0.4원)는 일치로 본다.
    {
        std::vector<Held> ledger{{"005930", 10, 71000.0}, {"000660", 3, 120000.0}};
        std::vector<Held> broker{{"005930", 10, 71000.4}, {"000660", 3, 120000.0}};
        auto rows = reconcile::plan(ledger, broker, /*resync=*/true, {}, "mode=REST");
        CHECK(rows.empty());
    }

    // 2) 수량 어긋남 — REST(resync)면 OVERWRITE, WS면 KEEP. 값은 덮어쓰기 전 원장 값 그대로.
    {
        std::vector<Held> ledger{{"005930", 10, 71000.0}};
        std::vector<Held> broker{{"005930", 12, 71100.0}};
        auto rest = reconcile::plan(ledger, broker, true, {}, "mode=REST");
        CHECK(rest.size() == 1);
        CHECK(rest[0].ticker == "005930");
        CHECK(rest[0].ledger_qty == 10 && rest[0].broker_qty == 12);
        CHECK(rest[0].ledger_avg == 71000.0 && rest[0].broker_avg == 71100.0);
        CHECK(rest[0].action == "OVERWRITE");
        CHECK(rest[0].note == "mode=REST");

        auto ws = reconcile::plan(ledger, broker, false, {}, "mode=WS");
        CHECK(ws.size() == 1);
        CHECK(ws[0].action == "KEEP");
    }

    // 3) 수량은 같고 평단만 1원 이상 어긋남 → 행이 난다(체결 하나가 다른 가격으로 들어갔다는 신호).
    {
        std::vector<Held> ledger{{"005930", 10, 71000.0}};
        std::vector<Held> broker{{"005930", 10, 71001.0}};
        auto rows = reconcile::plan(ledger, broker, true, {}, "");
        CHECK(rows.size() == 1);
        CHECK(rows[0].action == "OVERWRITE");
        CHECK(rows[0].note.empty());
    }

    // 4) 원장에만 있는 종목 — 엔진이 걷어낸(pruned) 것은 PRUNE, 유예된 것은 KEEP. 브로커 쪽은 0.
    //    브로커에만 있는 종목은 원장 0으로 OVERWRITE. 순서는 브로커 목록 → 원장 목록.
    {
        std::vector<Held> ledger{{"005930", 10, 71000.0}, {"035420", 5, 200000.0}, {"000660", 3, 120000.0}};
        std::vector<Held> broker{{"005930", 10, 71000.0}, {"068270", 2, 150000.0}};
        auto rows = reconcile::plan(ledger, broker, true, {"035420"}, "mode=REST");
        CHECK(rows.size() == 3);
        CHECK(rows[0].ticker == "068270" && rows[0].action == "OVERWRITE");
        CHECK(rows[0].ledger_qty == 0 && rows[0].broker_qty == 2 && rows[0].ledger_avg == 0.0);

        const Row* pr = find_row(rows, "035420");
        CHECK(pr && pr->action == "PRUNE");
        CHECK(pr->ledger_qty == 5 && pr->broker_qty == 0 && pr->broker_avg == 0.0);

        const Row* kp = find_row(rows, "000660");
        CHECK(kp && kp->action == "KEEP");
        CHECK(kp->ledger_qty == 3 && kp->broker_qty == 0);
        CHECK(find_row(rows, "005930") == nullptr);
    }

    // 5) 브로커 qty<=0·빈 티커는 보유가 아니다 — 원장에 없어도 행이 나지 않고, 원장에 있으면 원장 쪽 KEEP으로 잡힌다.
    {
        std::vector<Held> ledger{{"005930", 10, 71000.0}};
        std::vector<Held> broker{{"005930", 0, 0.0}, {"", 4, 100.0}};
        auto rows = reconcile::plan(ledger, broker, false, {}, "");
        CHECK(rows.size() == 1);
        CHECK(rows[0].ticker == "005930" && rows[0].action == "KEEP" && rows[0].broker_qty == 0);
    }

    // 6) 원장 qty<=0 항목은 대조 대상이 아니다.
    {
        std::vector<Held> ledger{{"005930", 0, 0.0}};
        std::vector<Held> broker{};
        CHECK(reconcile::plan(ledger, broker, true, {}, "").empty());
    }

    std::cout << "test_reconcile_plan: " << g_pass << " checks passed\n";
    return 0;
}
