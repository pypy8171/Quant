#pragma once
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// ETF/ETN·파생상품 종목명 판별. 브랜드 접두사(전방일치)와 상품 토큰(부분일치)의 OR.
//  시총·거래대금 유니버스 피드에 섞이는 채권/액티브 ETF가 개별주 이격매매 전략으로
//  유입되는 것을 막는다. 접두사 목록 밖의 비브랜드 액티브(KIWOOM·신영 등)는 토큰이 잡는다.
// ─────────────────────────────────────────────────────────────────────────────
namespace etf_filter
{

// 이름이 ETF류인가: 브랜드 접두사(경계검사) 전방일치 OR 상품 토큰 부분일치.
inline bool is_etf_like(const std::string& name,
                        const std::vector<std::string>& prefixes,
                        const std::vector<std::string>& tokens)
{
    for (const auto& p : prefixes)
    {
        // 브랜드 전방일치 + 경계 검사: 접두사 뒤가 문자열 끝이거나 ASCII(공백·숫자·영문)여야
        //  ETF로 본다. KIS 종목명은 브랜드 뒤에 공백/숫자가 온다("KODEX 200","KIWOOM 단기채권…").
        //  경계 없이 전방일치만 하면 한글이 바로 붙는 보통주를 오드롭한다(예: "파워"→파워로직스 037030).
        //  한글은 UTF-8 선두바이트가 0x80 이상 — 접두사 직후가 한글이면 경계 불성립(보통주로 판정).
        if (!p.empty() && name.rfind(p, 0) == 0 &&
            (name.size() == p.size() || static_cast<unsigned char>(name[p.size()]) < 0x80))
            return true;
    }
    for (const auto& tk : tokens)
        if (!tk.empty() && name.find(tk) != std::string::npos) // 상품 토큰 부분일치(채권·액티브…)
            return true;
    return false;
}

// 상품 토큰 기본값 — 개별 보통주 이름엔 나타나지 않는 ETF/ETN 표지만(오탐 방지).
//  단기'채권'·ESG'액티브' 같은 비브랜드 액티브·채권 ETF를 접두사 없이 잡는다.
inline const std::vector<std::string>& default_tokens()
{
    static const std::vector<std::string> t = {"ETF", "ETN", "액티브", "레버리지",
                                               "인버스", "커버드콜", "채권"};
    return t;
}

// 브랜드 접두사 기본값 — etf_prefixes.json 미존재 시 폴백.
// 주의: KisClient.cpp의 ETF_PREFIXES_FALLBACK과 물리적으로 두 벌 복제라, 한쪽을 고치면 다른 쪽도 함께 고쳐야 한다(정합은 코드로 강제되지 않음).
inline const std::vector<std::string>& default_prefixes()
{
    static const std::vector<std::string> p = {
        "KODEX",    "TIGER", "KINDEX", "KOSEF",  "ARIRANG",  "ACE",       "SOL",  "HANARO",
        "FOCUS",    "TREX",  "WON",    "PLUS",   "KoAct",    "TIMEFOLIO", "KTOP", "BIG",
        "히어로즈", "KCGI",  "파워",   "KBSTAR", "마이다스", "RISE",      "TRUE", "MASTER"};
    return p;
}

// config JSON(top-level 문자열 배열) 로드. QUANT_CONFIG_DIR(없으면 "Quant/config") 기준.
//  파일 부재·형식오류·빈배열이면 fallback 반환(조용히 — 로깅은 호출측이 필요하면 담당).
//  주의: 폴백 경로 "Quant/config"는 현재 작업 디렉터리 기준 상대경로다. 프로세스를
//   레포 루트가 아닌 곳(예: build_win/)에서 띄우면 파일을 못 찾아 조용히 fallback으로
//   떨어진다 — 실행은 레포 루트에서 하거나 QUANT_CONFIG_DIR를 절대경로로 지정할 것.
inline std::vector<std::string> load_list(const std::string& filename,
                                          const std::vector<std::string>& fallback)
{
    const char* dir = std::getenv("QUANT_CONFIG_DIR");
    std::string base = (dir && *dir) ? std::string(dir) : std::string("Quant/config");
    std::ifstream f(base + "/" + filename);
    if (!f.is_open())
        return fallback;
    try
    {
        auto j = nlohmann::json::parse(f);
        if (!j.is_array())
            return fallback;
        std::vector<std::string> out;
        for (const auto& e : j)
            if (e.is_string())
                out.push_back(e.get<std::string>());
        return out.empty() ? fallback : out;
    }
    catch (...)
    {
        return fallback;
    }
}

} // namespace etf_filter
