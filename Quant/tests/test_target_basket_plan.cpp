// 목표 비중표 바스켓(strategy/TargetBasketPlan.h) 단위 테스트 — 엔진 없이 파일 파싱·검증과 "목표 − 보유 = 주문" 계산을 고정한다.
//  관련 결정: D-109(바스켓 슬리브 — 파일 계약·밴드·두 레그).
// 빌드: cmake --build <directory> --target test_target_basket_plan
//
// 테스트 항목:
//   1. 정상 파일 파싱 — 슬리브·행·소유 집합(DROP 포함)·key
//   2. 검증 실패 — schema, weight 합, count 불일치, reference_price 없음, 모르는 슬리브
//   3. 빈 원장 → 전 종목 매수, 목표 수량 = floor(순자산 × share × weight / 기준가)
//   4. 리밸 날 밴드 — 10% 안 차이는 유지, 밖은 매도/매수
//   5. 리밸 아닌 날 — 채우기만, 줄이지 않는다
//   6. DROP 행 — 매도가능 수량만큼 매도, 매도가능 0이면 notes
//   7. 두 슬리브가 같은 종목 — 목표 합산, strategy_id BASKET_VALUE+MOMENTUM
//   8. liquidate_all — 파일의 모든 보유를 판다
//   9. 순자산 — 시드 + 실현손익 + 보유 평가손익(기준가 − 평단)
//  10. 한 슬리브 DROP + 다른 슬리브 KEEP — DROP 슬리브의 리밸 날로 그날 줄인다
#include "strategy/TargetBasketPlan.h"

#include <cmath>
#include <iostream>
#include <map>

namespace
{
int g_checks = 0;

#define CHECK(condition)                                                                        \
    do                                                                                          \
    {                                                                                           \
        ++g_checks;                                                                             \
        if (!(condition))                                                                       \
        {                                                                                       \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #condition << "\n";     \
            return 1;                                                                           \
        }                                                                                       \
    } while (0)

using json = nlohmann::json;

json sample_document()
{
    json document;
    document["schema"]       = 1;
    document["generated_at"] = "2026-09-22T08:40:00+09:00";
    document["as_of"]        = "2026-09-22";
    document["count"]        = 4;
    document["sleeves"]      = {{"VALUE", {{"share", 0.5}, {"is_rebalance_day", true}}},
                                {"MOMENTUM", {{"share", 0.5}, {"is_rebalance_day", false}}}};
    document["targets"]      = json::array({
        {{"ticker", "005930"}, {"name", "삼성전자"}, {"sleeve", "VALUE"}, {"weight", 0.5}, {"reference_price", 70000.0}, {"action", "KEEP"}},
        {{"ticker", "000660"}, {"name", "SK하이닉스"}, {"sleeve", "VALUE"}, {"weight", 0.5}, {"reference_price", 200000.0}, {"action", "NEW"}},
        {{"ticker", "035420"}, {"name", "NAVER"}, {"sleeve", "MOMENTUM"}, {"weight", 1.0}, {"reference_price", 250000.0}, {"action", "KEEP"}},
        {{"ticker", "051910"}, {"name", "LG화학"}, {"sleeve", "MOMENTUM"}, {"weight", 0.0}, {"reference_price", 400000.0}, {"action", "DROP"},
         {"reason", "국면 꺼짐"}},
    });
    return document;
}

basket::PlanInput input_with(std::map<std::string, basket::Holding> holdings, double capital = 10'000'000.0)
{
    basket::PlanInput input;
    input.capital_krw = capital;
    input.band        = 0.10;
    input.holding     = [holdings = std::move(holdings)](const std::string& ticker)
    {
        auto iterator = holdings.find(ticker);
        return iterator == holdings.end() ? basket::Holding{} : iterator->second;
    };
    return input;
}

const basket::PlannedOrder* find_order(const std::vector<basket::PlannedOrder>& orders, const std::string& ticker)
{
    for (const auto& order : orders)
    {
        if (order.ticker == ticker)
        {
            return &order;
        }
    }

    return nullptr;
}
} // namespace

int main()
{
    // 1. 정상 파싱
    std::string error;
    auto        targets = basket::parse_targets(sample_document(), error);
    CHECK(targets.has_value());
    CHECK(targets->as_of == "20260922");
    CHECK(targets->sleeves.size() == 2);
    CHECK(targets->rows.size() == 4);
    CHECK(targets->tickers().size() == 4); // DROP 행도 소유 집합에 든다
    CHECK(targets->key() == "2026-09-22T08:40:00+09:00/4");
    CHECK(targets->rows[3].weight == 0.0);

    // 2. 검증 실패
    {
        json bad = sample_document();
        bad["schema"] = 2;
        CHECK(!basket::parse_targets(bad, error));

        bad = sample_document();
        bad["targets"][0]["weight"] = 0.6; // VALUE 합 1.1
        CHECK(!basket::parse_targets(bad, error));
        CHECK(error.find("weight") != std::string::npos);

        bad = sample_document();
        bad["count"] = 3;
        CHECK(!basket::parse_targets(bad, error));

        bad = sample_document();
        bad["targets"][0]["reference_price"] = 0.0;
        CHECK(!basket::parse_targets(bad, error));

        bad = sample_document();
        bad["targets"][0]["sleeve"] = "QUALITY";
        CHECK(!basket::parse_targets(bad, error));

        bad = sample_document();
        bad["targets"][0]["weight"] = 0.503; // 합 1.003 — 허용
        CHECK(basket::parse_targets(bad, error).has_value());
    }

    // 3. 빈 원장 → 전부 매수
    {
        auto plan = basket::make_plan(*targets, input_with({}));
        CHECK(std::abs(plan.nav - 10'000'000.0) < 1e-6);
        CHECK(plan.sells.empty());
        CHECK(plan.buys.size() == 3);
        const auto* samsung = find_order(plan.buys, "005930");
        CHECK(samsung != nullptr);
        CHECK(samsung->quantity == static_cast<int>(std::floor(10'000'000.0 * 0.5 * 0.5 / 70000.0))); // 35
        CHECK(samsung->strategy_id == "BASKET_VALUE");
        CHECK(samsung->reference_price == 70000.0);
        const auto* naver = find_order(plan.buys, "035420");
        CHECK(naver != nullptr);
        CHECK(naver->quantity == 20); // 5,000,000 / 250,000
        CHECK(naver->strategy_id == "BASKET_MOMENTUM");
        CHECK(find_order(plan.buys, "051910") == nullptr); // DROP인데 보유 0 → 아무것도 없음
    }

    // 4. 리밸 날 밴드(VALUE) — 목표 35주. 33주 보유(차이 2주×7만=14만 < 밴드 25만) 유지, 20주(차이 105만) 매수, 50주 매도
    {
        auto plan = basket::make_plan(*targets, input_with({{"005930", {33, 70000.0, 33}}}));
        CHECK(find_order(plan.buys, "005930") == nullptr);
        CHECK(!plan.notes.empty());

        plan = basket::make_plan(*targets, input_with({{"005930", {20, 70000.0, 20}}}));
        const auto* top_up = find_order(plan.buys, "005930");
        CHECK(top_up != nullptr && top_up->quantity == 15);

        plan = basket::make_plan(*targets, input_with({{"005930", {50, 70000.0, 50}}}));
        const auto* trim = find_order(plan.sells, "005930");
        CHECK(trim != nullptr && trim->quantity == 15);
    }

    // 5. 리밸 아닌 날(MOMENTUM) — 목표 20주. 30주 보유는 줄이지 않고, 15주(< 18)는 채운다, 19주는 유지
    {
        auto plan = basket::make_plan(*targets, input_with({{"035420", {30, 250000.0, 30}}}));
        CHECK(find_order(plan.sells, "035420") == nullptr);

        plan = basket::make_plan(*targets, input_with({{"035420", {15, 250000.0, 15}}}));
        const auto* fill = find_order(plan.buys, "035420");
        CHECK(fill != nullptr && fill->quantity == 5);

        plan = basket::make_plan(*targets, input_with({{"035420", {19, 250000.0, 19}}}));
        CHECK(find_order(plan.buys, "035420") == nullptr);
    }

    // 6. DROP — 매도가능만큼, 0이면 notes
    {
        auto plan = basket::make_plan(*targets, input_with({{"051910", {10, 400000.0, 7}}}));
        const auto* drop = find_order(plan.sells, "051910");
        CHECK(drop != nullptr && drop->quantity == 7);
        CHECK(drop->strategy_id == "BASKET_DROP");
        CHECK(drop->reason.find("국면 꺼짐") != std::string::npos);

        plan = basket::make_plan(*targets, input_with({{"051910", {10, 400000.0, 0}}}));
        CHECK(find_order(plan.sells, "051910") == nullptr);
        bool noted = false;

        for (const auto& note : plan.notes)
        {
            noted = noted || note.find("051910") != std::string::npos;
        }

        CHECK(noted);
    }

    // 7. 두 슬리브 같은 종목 — 목표 합산
    {
        json both = sample_document();
        both["targets"][2]["ticker"] = "005930"; // MOMENTUM 1.0도 삼성전자
        both["targets"][2]["reference_price"] = 70000.0;
        auto merged = basket::parse_targets(both, error);
        CHECK(merged.has_value());
        auto plan = basket::make_plan(*merged, input_with({}));
        const auto* samsung = find_order(plan.buys, "005930");
        CHECK(samsung != nullptr);
        CHECK(samsung->quantity == static_cast<int>(std::floor((2'500'000.0 + 5'000'000.0) / 70000.0))); // 107
        CHECK(samsung->strategy_id == "BASKET_VALUE+MOMENTUM");
    }

    // 8. liquidate_all
    {
        json all = sample_document();
        all["liquidate_all"] = true;
        auto liquidate = basket::parse_targets(all, error);
        CHECK(liquidate.has_value());
        auto plan = basket::make_plan(*liquidate, input_with({{"005930", {35, 70000.0, 35}}, {"035420", {20, 250000.0, 20}}}));
        CHECK(plan.buys.empty());
        CHECK(plan.sells.size() == 2);
    }

    // 9. 순자산 = 시드 + 실현손익 + 평가손익
    {
        auto input = input_with({{"005930", {35, 60000.0, 35}}}); // 평단 6만, 기준가 7만 → +35만
        input.realized_pnl_krw = 150'000.0;
        auto plan = basket::make_plan(*targets, input);
        CHECK(std::abs(plan.nav - (10'000'000.0 + 150'000.0 + 350'000.0)) < 1e-6);
    }

    // 10. 한 슬리브가 DROP, 다른 슬리브는 KEEP(리밸 날 아님) — DROP 슬리브의 리밸 날로 그날 줄인다
    {
        json split = sample_document();
        split["sleeves"]["VALUE"]["is_rebalance_day"]    = false;
        split["sleeves"]["MOMENTUM"]["is_rebalance_day"] = true;
        split["targets"][3]["ticker"] = "005930"; // MOMENTUM이 삼성전자를 뺀다, VALUE 0.5는 KEEP
        split["targets"][3]["reference_price"] = 70000.0;
        auto parsed = basket::parse_targets(split, error);
        CHECK(parsed.has_value());
        auto plan = basket::make_plan(*parsed, input_with({{"005930", {200, 70000.0, 200}}}));
        const auto* reduce = find_order(plan.sells, "005930");
        CHECK(reduce != nullptr && reduce->quantity == 200 - 35); // 목표 1,000만×0.5×0.5/7만 = 35주
        CHECK(reduce->strategy_id == "BASKET_VALUE");
    }

    std::cout << "test_target_basket_plan: " << g_checks << " checks passed\n";
    return 0;
}
