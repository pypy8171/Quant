// universe/MarketBoard.h — 전 종목 시세판. 엔진 안의 스레드 하나가 네이버에서 종목 목록(하루 한 번)과 전 종목
//  시세(몇 초마다)를 받아 들고 있고, 1분마다 시장별 시총 상위 ∪ 거래대금 상위로 유니버스를 다시 뽑는다.
//  왜 있나: 이 일을 하던 파이썬 보조 프로세스 둘(scripts/live_prices_feed.py·PYQuant/tools/universe_feed.py)이
//  파일로 값을 넘겼다. 엔진 밖 프로세스가 죽으면 판정이 조용히 전일 값으로 얼어붙었고, 엔진은 같은 값을 파일
//  파싱으로 다시 읽었다. 여기서 받으면 파일도 파싱 왕복도 없다. [why D-147]
//
//  [inv] 받기·재랭킹은 시세판 스레드만 한다. 읽는 쪽(스캔 스레드)은 snapshot()·ranked()로 불변 사본의
//        포인터를 받는다 — 판을 바꿀 때는 새 사본을 만들어 포인터만 바꿔 끼운다.
//  [inv] 종목 테이블(SymbolTable)은 건드리지 않는다. 문자열 코드를 id로 바꾸는 일은 읽는 쪽이 한다.
#pragma once

#include <atomic>
#include <cstdint>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace universe
{

// 상장 종목 한 줄 — 네이버 시총순 목록에서 온다. 개별주(stockEndType "stock")만 담는다.
struct ListedStock
{
    std::string code;   // 여섯 자리(0096B0처럼 영문이 섞인 새 코드 포함)
    std::string name;
    std::string market; // "KOSPI" / "KOSDAQ"
};

// 시세 한 줄. 값은 정규장(KRX) 누적치다 — 대체거래소(NXT) 합산치는 쓰지 않는다(파이썬 피드와 같은 기준).
struct BoardQuote
{
    std::string code;
    std::string name;
    double      price        = 0.0; // 원, 현재가
    double      volume       = 0.0; // 주, 누적 거래량
    double      value        = 0.0; // 원, 누적 거래대금
    double      market_value = 0.0; // 원, 시가총액
};

struct BoardSnapshot
{
    std::time_t             received_at = 0; // 이 판을 다 받은 시각
    std::vector<BoardQuote> quotes;
};

// 재랭킹 결과 한 줄. universe_scan.json의 universe 배열 한 칸과 같은 모양이다.
struct RankedStock
{
    std::string code;
    std::string name;
    double      close = 0.0;
    std::string market;
};

struct RankedUniverse
{
    std::time_t              ranked_at = 0;
    std::vector<RankedStock> stocks;  // 시장별로 시총순 먼저, 이어 거래대금순(중복 제거)
    std::vector<ListedStock> listing; // 전 종목 목록 — 코드→시장·이름 사전으로 쓴다
};

// 순수 함수 — 테스트가 부를 수 있게 공개한다.

// 네이버 시총순 목록 한 쪽. 개별주만 돌려주고, 목록 전체 종목 수를 total_count에 적는다(실패면 -1).
std::vector<ListedStock> parse_listing_page(std::string_view body, const std::string& market, int& total_count);

// 네이버 시세 폴링 응답. 가격이 0 이하인 종목은 뺀다. 표시용 필드("5조 5,043억")가 아니라 Raw 필드를 읽는다.
std::vector<BoardQuote> parse_polling(std::string_view body);

// 시장별로 거래대금 ≥ min_turnover 이고 시총 > 0 인 종목 중 시총 상위 n_market_value ∪ 거래대금 상위 n_turnover.
//  시총순을 먼저 싣고 중복은 뺀다. 판에 없는 종목은 후보가 아니다.
std::vector<RankedStock> rank_universe(const std::vector<ListedStock>& listing, const BoardSnapshot& board,
                                       int n_market_value, int n_turnover, double min_turnover);

// 판의 종목 중 거래대금이 잡힌 종목이 절반을 넘는가. 장이 막 열려 누적치가 비어 가는 동안에는 재랭킹을 미룬다.
bool turnover_filled(const BoardSnapshot& board);

// universe_scan.json 문서(파이썬 피드와 같은 스키마 — 알림·대시보드·백필 스크립트가 읽는다).
std::string universe_file_text(const RankedUniverse& ranked, const std::string& source_label);

class MarketBoard
{
public:
    struct Config
    {
        int         period_sec       = 5;    // 초, 시세 한 바퀴 주기
        int         rerank_sec       = 60;   // 초, 재랭킹 주기
        size_t      codes_per_call   = 900;  // 한 요청에 묶는 종목 수(1,000까지 받고 1,500부터 400, 09-26 실측)
        int         n_market_value   = 0;    // 시장별 시총 상위 N. 0=시총 축 끔(대형주가 거래 없이 자리를 먹어 뺐다, D-146)
        int         n_turnover       = 100;  // 시장별 거래대금 상위 N
        double      min_turnover     = 1e9;  // 원, 재랭킹 후보의 거래대금 하한
        std::string universe_out;            // 비어 있지 않으면 재랭킹마다 이 경로에 universe_scan.json을 쓴다
    };

    // 프로세스에 하나. 슬리브 여럿이 켜도 같은 판을 본다.
    static MarketBoard& instance();

    // 처음 부른 설정으로 한 번만 띄운다. 이미 돌고 있으면 아무것도 안 한다.
    void start(const Config& config);
    void stop(); // 되돌아올 때 스레드는 멈춰 있다
    ~MarketBoard();

    bool running() const { return running_.load(std::memory_order_acquire); }

    // 아직 한 바퀴도 못 받았으면 nullptr.
    std::shared_ptr<const BoardSnapshot> snapshot() const;

    // 아직 한 번도 못 뽑았으면 nullptr.
    std::shared_ptr<const RankedUniverse> ranked() const;

    // 오늘 받은 종목 목록. 아직 못 받았으면 nullptr. 장 전 일봉 캐시 데우기가 대상 종목을 여기서 고른다.
    std::shared_ptr<const std::vector<ListedStock>> listing() const;

private:
    MarketBoard() = default;

    void run();
    bool refresh_listing();
    bool sweep();
    void rerank();

    Config                                config_;
    std::atomic<bool>                     running_{false};
    std::thread                           worker_;
    std::vector<ListedStock>              listing_;      // 시세판 스레드 전용
    std::string                           listing_date_; // 목록을 받은 KST 날짜
    std::vector<std::string>              request_urls_; // 목록이 바뀔 때만 다시 만든다
    mutable std::mutex                              mutex_;        // 아래 세 포인터만 지킨다
    std::shared_ptr<const BoardSnapshot>            snapshot_;
    std::shared_ptr<const RankedUniverse>           ranked_;
    std::shared_ptr<const std::vector<ListedStock>> published_listing_;
};

} // namespace universe
