// 국면 판정 피드 파일 사이의 공용 표 — 표결·장초·참고 지표 목록과 json 자식 찾기 선언. src/regime 안에서만 읽는다.
//  나눈 파일: RegimeFeedParse.cpp(응답 읽기)·RegimeFeedScore.cpp(점수·regime.json 문서)·RegimeFeed.cpp(스레드·HTTP·파일).
//  상태 없음. [why D-147]
#pragma once

#include <array>
#include <nlohmann/json.hpp>
#include <string_view>

namespace regime_feed::detail
{

// 표결 지표 8개. vote_direction은 지표가 오를 때 위험선호(+1)인지 위험회피(−1)인지.
//  코스피·코스닥은 네이버 실시간 지수가 먼저다(Yahoo는 개장 직후 몇 분 전날 값에 멈춘다, 09-15).
//  나스닥·S&P는 선물 현재가에 현물 직전 세션 등락을 더한다 — 선물만 보면 미국 정규장 하루치가 빠진다(09-18).
struct VotingSymbol
{
    std::string_view key;
    std::string_view yahoo;
    std::string_view label;
    int              vote_direction;
    double           warn;
    double           strong;
    std::string_view naver_code;     // 비면 네이버를 안 쓴다
    std::string_view cash_reference; // 비면 현물 보정 없음
};

constexpr std::array<VotingSymbol, 8> kVotingSymbols{{
    {"KOSPI", "^KS11", "코스피", +1, 0.7, 1.5, "KOSPI", ""},
    {"KOSDAQ", "^KQ11", "코스닥", +1, 0.8, 1.8, "KOSDAQ", ""},
    {"NQ_F", "NQ=F", "나스닥 (마감+선물)", +1, 0.4, 0.9, "", "^IXIC"},
    {"ES_F", "ES=F", "S&P500 (마감+선물)", +1, 0.4, 0.9, "", "^GSPC"},
    {"TNX10", "^TNX", "10Y 미국채금리", -1, 1.5, 3.0, "", ""},
    {"VIX", "^VIX", "VIX", -1, 4.0, 9.0, "", ""},
    {"USDKRW", "KRW=X", "USD/KRW", -1, 0.4, 0.9, "", ""},
    {"WTI", "CL=F", "WTI 유가", -1, 2.0, 4.0, "", ""},
}};

// 장초 대비 방향표 — 전일 대비 표는 개장 때 이미 빨간 날 장중에 돌아서는 것을 못 본다. warn은 기준점 대비 %.
struct IntraSymbol
{
    std::string_view key;
    double           warn;
    int              vote_direction;
};

constexpr std::array<IntraSymbol, 2> kIntraSymbols{{{"NQ_F", 0.3, +1}, {"TNX10", 0.6, -1}}};

// 참고 지표 — 표 없음, valid 개수에도 안 들어간다. FRED 일별 시리즈라 1~2일 늦다.
struct InfoSymbol
{
    std::string_view key;
    std::string_view fred_id;
    std::string_view label;
};

constexpr std::array<InfoSymbol, 3> kInfoSymbols{{
    {"TYX30", "DGS30", "30Y 미국채금리 (FRED, 1~2일 지연)"},
    {"DGS2", "DGS2", "2Y 미국채금리 (FRED, 1~2일 지연)"},
    {"HY", "BAMLH0A0HYM2", "미국 하이일드 스프레드 (FRED, 1~2일 지연)"},
}};

// node[key]. 객체가 아니거나 키가 없으면 nullptr.
const nlohmann::json* child(const nlohmann::json& node, const char* key);

} // namespace regime_feed::detail
