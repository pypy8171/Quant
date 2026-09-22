#include "utils/EtfFilter.h"

namespace etf_filter
{
bool is_etf_like(const std::string& name, const std::vector<std::string>& prefixes,
                 const std::vector<std::string>& tokens)
{
    for (const auto& prefix : prefixes)
    {
        // 브랜드 전방일치 + 경계 검사: 접두사 뒤가 문자열 끝이거나 ASCII(공백·숫자·영문)여야
        //  ETF로 본다. KIS 종목명은 브랜드 뒤에 공백/숫자가 온다("KODEX 200","KIWOOM 단기채권…").
        //  경계 없이 전방일치만 하면 한글이 바로 붙는 보통주를 오드롭한다(예: "파워"→파워로직스 037030).
        //  한글은 UTF-8 선두바이트가 0x80 이상 — 접두사 직후가 한글이면 경계 불성립(보통주로 판정).
        if (!prefix.empty() && name.starts_with(prefix) &&
            (name.size() == prefix.size() || static_cast<unsigned char>(name[prefix.size()]) < 0x80))
        {
            return true;
        }
    }

    for (const auto& ticker : tokens)
    {
        if (!ticker.empty() && name.find(ticker) != std::string::npos) // 상품 토큰 부분일치(채권·액티브…)
        {
            return true;
        }
    }

    return false;
}

bool is_reit_like(const std::string& name, const std::vector<std::string>& suffixes,
                  const std::vector<std::string>& exacts)
{
    for (const auto& suffix : suffixes)
    {
        if (!suffix.empty() && name.size() >= suffix.size() &&
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
        {
            return true;
        }
    }

    for (const auto& exact : exacts)
    {
        if (!exact.empty() && name == exact)
        {
            return true;
        }
    }

    return false;
}

const std::vector<std::string>& default_reit_suffixes()
{
    static const std::vector<std::string> suffixes = {"리츠"};
    return suffixes;
}

const std::vector<std::string>& default_reit_exacts()
{
    static const std::vector<std::string> exacts = {"이리츠코크렙"};
    return exacts;
}

const std::vector<std::string>& default_tokens()
{
    static const std::vector<std::string> tokens = {"ETF", "ETN", "액티브", "레버리지", "인버스", "커버드콜", "채권"};
    return tokens;
}

const std::vector<std::string>& default_prefixes()
{
    static const std::vector<std::string> prefixes = {"KODEX", "TIGER",     "KINDEX",   "KOSEF", "ARIRANG",  "ACE",
                                                      "SOL",   "HANARO",    "FOCUS",    "TREX",  "WON",      "PLUS",
                                                      "KoAct", "TIMEFOLIO", "KTOP",     "BIG",   "히어로즈", "KCGI",
                                                      "파워",  "KBSTAR",    "마이다스", "RISE",  "TRUE",     "MASTER"};
    return prefixes;
}

std::vector<std::string> load_list(const std::string& filename, const std::vector<std::string>& fallback)
{
    const char* directory = std::getenv("QUANT_CONFIG_DIR");
    std::string base = (directory && *directory) ? std::string(directory) : std::string("Quant/config");
    std::ifstream file(base + "/" + filename);

    if (!file.is_open())
    {
        return fallback;
    }

    try
    {
        auto document = nlohmann::json::parse(file);

        if (!document.is_array())
        {
            return fallback;
        }

        std::vector<std::string> out;

        for (const auto& element : document)
        {
            if (element.is_string())
            {
                out.push_back(element.get<std::string>());
            }
        }

        if (out.empty())
        {
            return fallback;
        }

        return out; // 지역 변수라 복사 없이 옮겨진다(삼항식은 fallback 형에 맞춰 한 번 더 베꼈다)
    }
    catch (...)
    {
        return fallback;
    }
}

} // namespace etf_filter
