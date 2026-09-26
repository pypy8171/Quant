// 전 종목 시세 표 — 시세판(MarketBoard)의 판을 종목 id 칸의 표로 옮긴다. 뒤 단계(후보 수집·정배열 판정)가 현재가와
//  거래대금을 이 표에서 꺼내 KIS 초당 한도를 쓰지 않는다. 스캔 스레드 전용. [why D-112]

#include "detail/Pipeline.h"
#include "universe/MarketBoard.h"
#include "core/Types.h"
#include "utils/Logger.h"
#include <ctime>
#include <string>
#include <vector>

namespace universe
{
namespace
{
// 판이 이보다 오래되면 경고한다(판정은 계속 그 값으로 한다).
constexpr int kPricesStaleWarnSec = 600;

// 표를 재스캔 사이에 이어 쓴다. 먼저 전 칸의 숫자를 비워 이번 판에 없는 종목이 옛 값을 들고 있지 않게 하고,
//  이름은 finish_table이 가격이 없는 칸만 비운다(문자열 버퍼는 남겨 다음 복사 때 다시 잡지 않는다).
void clear_numbers(QuoteTable& quotes, const symbol::SymbolTable& symbols)
{
    if (quotes.size() < symbols.capacity())
    {
        quotes.resize(symbols.capacity());
    }

    for (MarketQuote& market_quote : quotes)
    {
        market_quote.price  = 0.0;
        market_quote.value  = 0.0;
        market_quote.volume = 0.0;
    }
}

void finish_table(QuoteTable& quotes)
{
    for (MarketQuote& market_quote : quotes)
    {
        if (market_quote.price <= 0.0 && !market_quote.name.empty())
        {
            market_quote.name.clear();
        }
    }
}
} // namespace

// 시세판이 꺼져 있거나 아직 첫 판을 못 받았을 때(장 전 기동 직후) 표를 비운다. 그러면 뒤 단계는 KIS 랭킹 축
//  스냅샷가만 쓴다. 전날 값으로 판정하는 것보다 이 편이 낫다 — 옛 시세 파일 폴백은 아무도 갱신하지 않는 파일을
//  읽어 정배열·이격 판정을 전일 종가로 묶었다. [why D-147]
void clear_quote_table(QuoteTable& quotes, const symbol::SymbolTable& symbols)
{
    clear_numbers(quotes, symbols);
    finish_table(quotes);
}

// 시세판(MarketBoard)의 판을 표에 붓는다. 표에는 시장 전체가 들어가고(전 종목 확장 축이 이 표를 후보 원천으로
//  쓴다), 뒤이어 랭킹 축 스냅샷가가 덮인다. 낡음 경고 문구는 scripts/check_runtime_health.py가 센다. [why D-147]
void load_quote_table(const BoardSnapshot& board, QuoteTable& quotes, symbol::SymbolTable& symbols)
{
    clear_numbers(quotes, symbols);
    int loaded = 0;

    for (const BoardQuote& quote : board.quotes)
    {
        const symbol::SymbolId symbol = symbols.intern(quote.code); // 판의 문자열 코드 — 여기서 id가 된다

        if (symbol == symbol::kNone)
        {
            continue;
        }

        if (quotes.size() <= symbol)
        {
            quotes.resize(symbols.capacity());
        }

        MarketQuote& market_quote = quotes[symbol];
        market_quote.price  = quote.price;
        market_quote.value  = quote.value;
        market_quote.volume = quote.volume;

        if (market_quote.name != quote.name)
        {
            market_quote.name = quote.name; // 이름이 바뀐 때만 복사한다
        }

        ++loaded;
    }

    finish_table(quotes);
    const std::time_t age = std::time(nullptr) - board.received_at;
    LOG_INFO("[Main] 전 종목 시세: " + std::to_string(loaded) + "종목 (시세판, " +
             std::to_string(static_cast<long long>(age)) + "초 전 갱신)");

    if (age > kPricesStaleWarnSec)
    {
        LOG_WARN("[Main] 전 종목 시세가 " + std::to_string(static_cast<long long>(age)) +
                 "초 지났다 — 시세판 스레드 확인 필요([MarketBoard] 경고). 정배열 판정이 전일 종가로 고정된다");
    }
}
} // namespace universe
