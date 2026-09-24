// 전 종목 시세 표 — 네이버 벌크 시세 파일을 종목 id 칸의 표로 읽는다. 뒤 단계(후보 수집·정배열 판정)가 현재가와
//  거래대금을 이 표에서 꺼내 KIS 초당 한도를 쓰지 않는다. 스캔 스레드 전용. [why D-112]

#include "detail/Pipeline.h"
#include "utils/JsonNode.h"
#include "universe/MaAlign.h"
#include "core/KstTime.h"
#include "core/Types.h"
#include "utils/EtfFilter.h"
#include "utils/Logger.h"
#include <algorithm>
#include <functional>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace universe
{
namespace
{
// 시세 파일이 이보다 오래되면 경고한다(판정은 계속 그 값으로 한다).
constexpr int kPricesStaleWarnSec = 600;

// 시세 파일을 비워 둔 표에 붓는다. 파일에 있는 종목 칸만 채운다 — 비우기는 load_quote_table이 한다.
void load_quote_file(const std::string& prices_file, QuoteTable& quotes, symbol::SymbolTable& symbols)
{
    if (prices_file.empty())
    {
        return;
    }

    std::ifstream pf(prices_file);

    if (!pf)
    {
        LOG_WARN("[Main] 전 종목 시세 파일 없음(" + prices_file + ") — 랭킹 축 가격만 쓴다");
        return;
    }

    try
    {
        nlohmann::json parsed_json;
        pf >> parsed_json;
        // 필드 타입이 기대와 다르면 nlohmann은 예외를 던진다. 그대로 두면 바깥 catch로 빠져
        //  시세 파일 전체가 버려지는데, 결과가 "price가 전일 종가로 회귀"라 로그만 보면 파일 없음과
        //  구분되지 않는다. 항목 단위로 막아 어긋난 종목만 버린다.
        auto number = [](const nlohmann::json& document, const char* key) -> double
        {
            const auto found = document.find(key);
            return (found != document.end() && found->is_number()) ? found->get<double>() : 0.0;
        };
        const nlohmann::json& pm = jsonx::object_or_empty(parsed_json, "prices");
        int bad    = 0;
        int loaded = 0;

        for (auto iterator = pm.begin(); iterator != pm.end(); ++iterator)
        {
            if (!iterator.value().is_object())
            {
                ++bad;
                continue;
            }

            const double price = number(iterator.value(), "px");

            if (price <= 0.0)
            {
                continue;
            }

            const symbol::SymbolId symbol = symbols.intern(iterator.key()); // 파일의 문자열 티커 — 여기서 id가 된다

            if (symbol == symbol::kNone)
            {
                continue;
            }

            MarketQuote& market_quote = quotes[symbol];
            market_quote.price  = price;
            ++loaded;
            market_quote.value = number(iterator.value(), "val");
            market_quote.volume = number(iterator.value(), "vol");
            const auto name_node = iterator.value().find("nm");

            if (name_node != iterator.value().end() && name_node->is_string())
            {
                const std::string& name = name_node->get_ref<const std::string&>();

                if (market_quote.name != name)
                {
                    market_quote.name = name; // 이름이 바뀐 때만 복사한다
                }
            }
            else
            {
                market_quote.name.clear();
            }
        }

        const auto timestamp_iterator = parsed_json.is_object() ? parsed_json.find("ts") : parsed_json.end();
        const std::time_t timestamp =
            (timestamp_iterator != parsed_json.end() && timestamp_iterator->is_number()) ? static_cast<std::time_t>(timestamp_iterator->get<long long>()) : 0;
        const std::time_t age = std::time(nullptr) - timestamp;
        LOG_INFO("[Main] 전 종목 시세: " + std::to_string(loaded) +
                 "종목 (" + std::to_string(static_cast<long long>(age)) + "초 전 갱신)");

        if (bad > 0)
        {
            LOG_WARN("[Main] 전 종목 시세 항목 " + std::to_string(bad) +
                     "건이 형식에 맞지 않아 건너뛴다");
        }

        // 이 파일이 멈추면 px_live가 전일 종가로 돌아가 정배열·이격 판정이 장 마감까지
        //  얼어붙는다. 재스캔은 돌지만 결과가 같아 구분이 안 된다.
        if (timestamp <= 0)
        {
            LOG_WARN("[Main] 전 종목 시세 파일에 갱신 시각(ts)이 없다 — 최신 여부 확인 불가");
        }
        else if (age > kPricesStaleWarnSec)
        {
            LOG_WARN("[Main] 전 종목 시세가 " + std::to_string(static_cast<long long>(age)) +
                     "초 지났다 — 보조 프로세스 확인 필요. 정배열 판정이 전일 종가로 고정된다");
        }
    }
    catch (const std::exception& exception)
    {
        LOG_WARN(std::string("[Main] 전 종목 시세 파일 파싱 실패: ") + exception.what());
    }
}
} // namespace

// 전 종목 장중 시세 파일. 네이버 벌크를 묶어오므로 KIS 초당 한도를 쓰지 않고 후보 전체의
//  현재가를 얻는다. 이게 있어야 정배열·이격을 매 재스캔마다 다시 판정한다. 표에는 시장 전체가 들어가고
//  (전 종목 확장 축이 이 표를 후보 원천으로 쓴다), 뒤이어 랭킹 축 스냅샷가가 덮인다.
//  실패는 경고만 내고 표를 비운 채 돌아간다 — 그러면 랭킹 축 스냅샷가만 쓰게 된다.
void load_quote_table(const std::string& prices_file, QuoteTable& quotes, symbol::SymbolTable& symbols)
{
    // 표는 재스캔 사이에 이어 쓴다. 먼저 전 칸의 숫자를 비워 이번 파일에 없는 종목이 옛 값을 들고 있지 않게 하고,
    //  이름은 끝에서 가격이 없는 칸만 비운다(문자열 버퍼는 남겨 다음 복사 때 다시 잡지 않는다).
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

    load_quote_file(prices_file, quotes, symbols);

    for (MarketQuote& market_quote : quotes)
    {
        if (market_quote.price <= 0.0 && !market_quote.name.empty())
        {
            market_quote.name.clear();
        }
    }
}
} // namespace universe
