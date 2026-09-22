#include "core/ReconcilePlan.h"

namespace reconcile
{
std::vector<Row> plan(const std::vector<Held>& ledger, const std::vector<Held>& broker, bool resync,
                      const std::vector<symbol::SymbolId>& pruned, const std::string& note)
{
    // 종목 id 인덱스 표 세 개 — 크기는 세 목록의 가장 큰 id+1.
    symbol::SymbolId top = 0;

    for (const auto& entry : ledger)
    {
        top = (std::max)(top, entry.symbol);
    }

    for (const auto& entry : broker)
    {
        top = (std::max)(top, entry.symbol);
    }

    for (const symbol::SymbolId symbol : pruned)
    {
        top = (std::max)(top, symbol);
    }

    std::vector<const Held*> by_symbol(static_cast<size_t>(top) + 1, nullptr);
    std::vector<bool> seen(by_symbol.size(), false);
    std::vector<bool> gone(by_symbol.size(), false);
    std::vector<Row> rows;

    for (const auto& ledger_entry : ledger)
    {
        by_symbol[ledger_entry.symbol] = &ledger_entry;
    }

    for (const symbol::SymbolId symbol : pruned)
    {
        gone[symbol] = true;
    }

    for (const auto& broker_entry : broker)
    {
        if (broker_entry.ticker.empty() || broker_entry.quantity <= 0)
        {
            continue;
        }

        seen[broker_entry.symbol] = true;
        const Held* ledger_entry = by_symbol[broker_entry.symbol];
        const int ledger_quantity = ledger_entry ? ledger_entry->quantity : 0;
        const auto ledger_average = ledger_entry ? ledger_entry->average : 0.0;

        if (ledger_quantity == broker_entry.quantity && !average_differs(ledger_average, broker_entry.average))
        {
            continue;
        }

        rows.push_back(Row{broker_entry.ticker, ledger_quantity, broker_entry.quantity, ledger_average,
                           broker_entry.average, resync ? "OVERWRITE" : "KEEP", note});
    }

    for (const auto& ledger_entry : ledger)
    {
        if (ledger_entry.quantity <= 0 || seen[ledger_entry.symbol])
        {
            continue;
        }

        rows.push_back(Row{ledger_entry.ticker, ledger_entry.quantity, 0, ledger_entry.average, 0.0,
                           gone[ledger_entry.symbol] ? "PRUNE" : "KEEP", note});
    }

    return rows;
}

} // namespace reconcile
