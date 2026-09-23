// tools/value_rank_check.cpp
// 거래대금 상위 랭킹 점검 — KisClient::fetch_value_ranking이 실제로 몇 종목을, 무엇을 돌려주는지 본다.
//
//   보는 것 셋:
//     ① 행수 — volume-rank는 한 번에 30행이 상한이라 그 위는 가격 구간을 갈라 두 번 부른다.
//        count를 30 위로 주고도 30에서 멈추면 합치는 쪽이 망가진 것이다.
//     ② ETF 섞임 — FID_TRGT_EXLS_CLS_CODE 7·8번째 자리로 API단에서 빼고 있어 0이어야 한다.
//        0이 아니면 마스크가 안 먹는 것이고(KIS가 스펙을 바꾼 것일 수 있다), 이름 필터만 남는다.
//     ③ 거래대금 내림차순 — 두 페이지를 합친 뒤 다시 줄 세우는 것이 맞는지.
//
//   시세 REST는 모의 도메인이 미지원(HTTP500)이라 실전 시세키가 필요하다.
//   config에 quote_kis 블록이 있으면 그걸, 없으면 kis 블록을 쓴다(future_quote_check와 동일).
//
//   사용법:
//     value_rank_check <config> [count=40] [blng_cls=3]
//   예)
//     value_rank_check config/config_dev_paper.json
//     value_rank_check config/config_dev_paper.json 30 1

#include "api/KisClient.h"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

using json = nlohmann::json;

int main(int argc, char** argv)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    if (argc < 2)
    {
        std::cout << "사용법: value_rank_check <config> [count=40] [blng_cls=3]\n"
                     "  blng_cls 정렬축: 0=거래량 1=거래증가율 3=거래금액(기본)\n"
                     "예) value_rank_check config/config_dev_paper.json 40\n";
        return 1;
    }

    const std::string config_path = argv[1];
    const int requested_count = (argc > 2) ? std::stoi(argv[2]) : 40;
    const std::string belong_class = (argc > 3) ? argv[3] : "3";

    std::ifstream file(config_path);

    if (!file)
    {
        std::cerr << "[중단] config 못 엶: " << config_path << "\n";
        return 1;
    }

    json config = json::parse(file);

    // 시세키 선택: quote_kis(실전 시세) 우선, 없으면 kis.
    const char* block = config.contains("quote_kis") ? "quote_kis" : "kis";
    const json& kis_block = config[block];

    KisConfig kis_config;
    kis_config.app_key    = kis_block.value("app_key", "");
    kis_config.app_secret = kis_block.value("app_secret", "");
    kis_config.is_paper   = kis_block.value("is_paper", false);
    // 시세 조회는 계좌 불필요(quote 전용). account_no/type는 비워둔다.

    std::cout << "=== 거래대금 랭킹 점검 ===\n";
    std::cout << "config=" << config_path << "  키블록=" << block
              << "  is_paper=" << (kis_config.is_paper ? "true" : "false") << "\n";
    std::cout << "요청 count=" << requested_count << "  정렬축=" << belong_class << "\n";

    if (kis_config.is_paper)
    {
        std::cout << "[경고] is_paper=true 시세키 — 모의 도메인은 시세 REST 미지원이라 HTTP500이 예상됩니다.\n"
                     "       config에 실전 quote_kis 블록을 두거나 실전 config를 쓰세요.\n";
    }

    KisClient kis(kis_config);

    if (!kis.authenticate())
    {
        std::cerr << "[중단] 인증 실패 (앱키/시크릿 확인)\n";
        return 3;
    }

    std::vector<KisClient::RankingStock> ranked = kis.fetch_value_ranking(requested_count, "J", belong_class);

    if (ranked.empty())
    {
        std::cerr << "[중단] 0종목 — 응답이 비었거나 전부 걸러졌다. 로그의 진단 줄을 본다.\n";
        return 4;
    }

    std::cout << "\n순위  종목코드  거래대금(억)  종목명\n";

    for (const auto& stock : ranked)
    {
        std::cout << std::setw(4) << stock.rank << "  " << std::setw(8) << stock.ticker << "  "
                  << std::setw(12) << std::fixed << std::setprecision(0) << stock.trade_value / 100000000.0 << "  "
                  << stock.name << "\n";
    }

    // 판정 셋 — 사람이 표를 읽지 않아도 되게 여기서 끝낸다.
    const bool count_met = static_cast<int>(ranked.size()) >= requested_count;
    bool descending = true;

    for (std::size_t index = 1; index < ranked.size(); ++index)
    {
        if (ranked[index].trade_value > ranked[index - 1].trade_value)
        {
            descending = false;
            break;
        }
    }

    std::cout << "\n[판정] 행수 " << ranked.size() << "/" << requested_count << " — "
              << (count_met ? "채움" : "모자람") << "\n";
    std::cout << "[판정] 거래대금 내림차순 — " << (descending ? "맞음" : "어긋남(합치는 쪽 확인)") << "\n";
    std::cout << "[판정] ETF 섞임 — 위 표에 KODEX·TIGER 같은 상품명이 보이면 제외 마스크가 안 먹는 것이다\n";

    if (belong_class != "3")
    {
        std::cout << "       (정렬축 " << belong_class << "은 거래대금이 비어 있어 위 두 판정은 뜻이 없다)\n";
    }

    return (count_met && descending) ? 0 : 5;
}
