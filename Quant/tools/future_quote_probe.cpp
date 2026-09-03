// tools/future_quote_probe.cpp
// 국내 선물 시세 조회 프로브 — 실전 시세키로 KIS 선물옵션 inquire-price(FHMIF10000000)를
// 1콜 찍어 파싱 결과 + raw output(로그)을 확인한다.
//
//   목적: KisClient::get_future_price의 output 필드명은 KIS 공개 스키마가 없어 잠정값이다.
//         실키 1콜의 raw output을 보고 필드명을 확정한 뒤 게터를 잠근다.
//
//   시세 REST는 모의(openapivts:29443)가 미지원(HTTP500)이라 실전 도메인이 필요하다.
//   config에 quote_kis(실전 시세키) 블록이 있으면 그걸 쓰고, 없으면 kis 블록을 쓴다.
//   quote_kis/kis 중 실제로 쓰는 키가 is_paper=true면 경고만 하고 진행(500 예상).
//
//   사용법:
//     future_quote_probe <config> <iscd> [market_div=F]
//   예)
//     future_quote_probe config/config_dev_paper.json 101W09
//         (KOSPI200 선물 최근월물 코드는 만기마다 바뀐다 — KRX 또는 선물 전광판에서 확인)

#include "api/KisClient.h"

#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#ifdef _WIN32
#include <windows.h>
#endif

using json = nlohmann::json;

int main(int argc, char** argv)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    if (argc < 3)
    {
        std::cout << "사용법: future_quote_probe <config> <iscd> [market_div=F]\n"
                     "예) future_quote_probe config/config_dev_paper.json 101W09\n"
                     "    (선물 최근월물 코드는 만기마다 바뀜 — KRX/선물 전광판에서 확인)\n";
        return 1;
    }

    const std::string config_path = argv[1];
    const std::string iscd = argv[2];
    const std::string mrkt = (argc > 3) ? argv[3] : "F";

    std::ifstream f(config_path);
    if (!f)
    {
        std::cerr << "[중단] config 못 엶: " << config_path << "\n";
        return 1;
    }
    json cfg = json::parse(f);

    // 시세키 선택: quote_kis(실전 시세) 우선, 없으면 kis.
    const char* block = cfg.contains("quote_kis") ? "quote_kis" : "kis";
    const json& kb = cfg[block];

    KisConfig kc;
    kc.app_key    = kb.value("app_key", "");
    kc.app_secret = kb.value("app_secret", "");
    kc.is_paper   = kb.value("is_paper", false);
    // 시세 조회는 계좌 불필요(quote 전용). account_no/type는 비워둔다.

    std::cout << "=== 선물 시세 프로브 ===\n";
    std::cout << "config=" << config_path << "  키블록=" << block
              << "  is_paper=" << (kc.is_paper ? "true" : "false") << "\n";
    std::cout << "iscd=" << iscd << "  market_div=" << mrkt << "\n";
    if (kc.is_paper)
        std::cout << "[경고] is_paper=true 시세키 — 모의 도메인은 시세 REST 미지원이라 HTTP500이 예상됩니다.\n"
                     "       config에 실전 quote_kis 블록을 두거나 실전 config를 쓰세요.\n";

    KisClient kis(kc);
    if (!kis.authenticate())
    {
        std::cerr << "[중단] 인증 실패 (앱키/시크릿 확인)\n";
        return 3;
    }
    std::cout << "[1] 인증 완료\n";

    // iscd=="list" → 선물 전광판 조회(현재 거래가능 계약 목록·코드). 최근월물 코드 확보용.
    if (iscd == "list")
    {
        std::string cls = (mrkt == "F") ? "MKI" : mrkt; // 3번째 인자를 market_cls로 재사용 가능
        json board = kis.get_future_board(cls);
        std::cout << "[2] 선물 전광판 (market_cls=" << cls << ")\n";
        std::cout << board.dump(2) << "\n";
        std::cout << "=== 완료 (output 배열에서 계약코드 필드를 찾아 그 값을 iscd로 재실행) ===\n";
        return 0;
    }

    KisClient::FuturePrice fp = kis.get_future_price(iscd, mrkt);
    std::cout << "[2] 조회 결과 (ok=" << (fp.ok ? "true" : "false") << ")\n";
    std::cout << "    현재가        = " << fp.price << "\n";
    std::cout << "    전일대비      = " << fp.change << " (" << fp.change_rate << "%)  sign=" << fp.sign << "\n";
    std::cout << "    시/고/저      = " << fp.open << " / " << fp.high << " / " << fp.low << "\n";
    std::cout << "    누적거래량    = " << fp.volume << "\n";
    std::cout << "    미결제약정    = " << fp.open_interest << "\n";
    std::cout << "\n※ 필드명 확정: 로그(logs/quant_trader.log)의 '[KIS] get_future_price RAW ...' 한 줄을\n"
                 "  보고 실제 output 키와 대조하세요. ok=false거나 값이 0이면 필드명/ISCD/market_div를 조정합니다.\n";
    std::cout << "=== 완료 ===\n";
    return 0;
}
