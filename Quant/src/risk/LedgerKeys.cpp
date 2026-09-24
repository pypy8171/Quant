#include "risk/LedgerKeys.h"

// 살아 있는 종목 문자열(브로커 잔고·라우터 이력)을 종목 id 비트로 한 번만 바꾼다 — 원장을 돌며 항목마다
//  문자열 집합을 묻던 것을 비트 인덱스로 바꿨다. 테이블이 모르는 종목은 원장에도 없으니 빠뜨려도 같다.
std::vector<bool> LedgerKeys::live_symbols(const std::vector<std::string>& live_tickers) const
{
    std::vector<bool> live(symbols_->capacity(), false);

    for (const std::string& ticker : live_tickers)
    {
        const symbol::SymbolId symbol = symbols_->lookup(ticker);

        if (symbol != symbol::kNone && symbol < live.size())
        {
            live[symbol] = true;
        }
    }

    return live;
}
