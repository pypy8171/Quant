#pragma once
#include "api/IOrderExecutor.h"
#include "core/Types.h"
#include <chrono>
#include <cstdio>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

struct KisConfig
{
    std::string app_key;
    std::string app_secret;
    std::string account_no;
    std::string account_type; // "01"
    std::string hts_id;       // H0STCNI0/9 체결통보 구독 키(HTS ID). 비어 있으면 구독을 건너뛴다 — account_no로 대체하지 않는다
    bool is_paper = false;
    // 일봉 캐시 유효시간(초). 0이면 캐시 끔. 일봉을 매 사이클 다시 받아야 하는 전략
    //  (MA_CROSS·MOMENTUM처럼 on_data로 도는 것)을 쓸 때는 짧게 두거나 0으로 끈다.
    int daily_cache_ttl_sec = 600;
};

// HHMMSS 문자열에서 minutes분을 빼 같은 형식으로 돌려준다. 자릿수 산술이 아니라 초로 바꿔 뺀다
//  ("100000" - 1분 = "095900". 10진수 -100은 "099900"이라는 없는 시각을 만든다).
//  형식이 아니거나 결과가 00:00:00 아래로 내려가면 빈 문자열.
inline std::string kis_hhmmss_minus_minutes(const std::string& hhmmss, int minutes)
{
    if (hhmmss.size() != 6)
    {
        return "";
    }

    for (char c : hhmmss)
    {
        if (c < '0' || c > '9')
        {
            return "";
        }
    }

    const int hh = (hhmmss[0] - '0') * 10 + (hhmmss[1] - '0');
    const int mm = (hhmmss[2] - '0') * 10 + (hhmmss[3] - '0');
    const int ss = (hhmmss[4] - '0') * 10 + (hhmmss[5] - '0');

    if (hh > 23 || mm > 59 || ss > 59)
    {
        return "";
    }

    const long total = static_cast<long>(hh) * 3600 + mm * 60 + ss - static_cast<long>(minutes) * 60;

    if (total < 0)
    {
        return "";
    }

    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02ld%02ld%02ld", total / 3600, (total / 60) % 60, total % 60);
    return buf;
}

class KisClient : public IOrderExecutor
{
public:
    explicit KisClient(const KisConfig& cfg);
    ~KisClient() override;

    // 이 스레드의 조회를 재시도 없이 보낸다(재시도 없이 즉시 실패). 공유 전략 스레드처럼 한 번의 왕복이
    //  다른 종목 전체를 막는 자리에서 쓴다 — 3회 재시도 × 타임아웃이면 한 번의 조회가 스레드를
    //  수십 초 잡는다. 실패는 호출자가 보류(0)로 처리하고 다음 하트비트에 다시 온다.
    class FastFailScope
    {
    public:
        FastFailScope();
        ~FastFailScope();
    };

    bool authenticate();
    bool is_authenticated() const
    {
        std::lock_guard<std::mutex> lk(token_mtx_);
        return !access_token_.empty();
    }

    // 계좌번호 보유 여부(주문/잔고 계좌). 시세전용(quote) 클라이언트는 account_no가 비어
    // 잔고·주문가능 조회가 불가 — 호출측 가드용.
    bool has_account() const
    {
        return !cfg_.account_no.empty();
    }

    // 주문/잔고 계좌번호(CANO). 당일손익 기준선을 계좌별로 분리 저장할 때 쓴다
    //  (같은 거래일에 계좌를 바꾸면 옛 기준선 재사용으로 당일손익이 오염되는 것 방지).
    const std::string& account_no() const { return cfg_.account_no; }

    // ── 국내 (KR) ──────────────────────────────────────────────────────────
    // 일봉 조회. 기본값은 당일 봉을 뺀 '전일까지'다(docs/DECISIONS.md D-005).
    //   KIS 응답의 output2[0]은 장중이면 오늘 진행 중 봉이고 그 종가가 실시간 현재가다.
    //   이동평균에 넣으면 오늘 가격이 1/n 가중으로 섞여 지표가 스스로를 참조한다(SMA5는 1/5).
    //   스냅샷을 하루 1회만 갱신하는 호출자에게는 그 값이 페치 시각에 동결되기까지 한다.
    //   지표·앵커·게이트 용도는 전부 기본값(false)을 쓴다. 오늘 시세 자체가 필요한
    //   경우(파이프라인에 당일 봉을 흘리는 데이터 스레드)에만 include_today=true.
    std::vector<MarketData> get_daily_ohlcv(const std::string& ticker, int count,
                                            bool include_today = false);
    // 당일 분봉 → interval_min 집계봉(기본 3분봉). FHKST03010200 역페이지네이션 후 1분봉 집계.
    //   반환: 최신→과거(result[0]=최신), 최대 count봉. interval_min=1이면 1분봉 그대로.
    std::vector<MarketData> get_minute_ohlcv(const std::string& ticker, int count, int interval_min = 3);
    // 지정 날짜(과거일 포함)의 분봉 → interval_min 집계봉. TR FHKST03010230.
    //   당일 분봉 TR은 날짜 인자가 없어 오늘에 갇힌다. 이쪽은 1콜에 1분봉 120개(=130분)를 준다.
    //   end_hhmmss에서 과거로 역페이징. 반환: 최신→과거(result[0]=최신), 최대 count봉.
    //   라이브 신호용이 아니라 과거 분봉 캐시·오프라인 백테스트 입력용이다(docs/DECISIONS.md D-004).
    std::vector<MarketData> get_daily_minute_ohlcv(const std::string& ticker,
                                                   const std::string& yyyymmdd,
                                                   int count, int interval_min = 3,
                                                   const std::string& end_hhmmss = "153000");
    double get_current_price(const std::string& ticker);
    Fundamentals get_fundamentals(const std::string& ticker);
    bool send_order(const OrderSignal& signal);
    // FEP: 주문 제출 — KIS 접수번호(ODNO) 반환, 실패 시 빈 문자열
    std::string submit_order(const OrderSignal& signal) override;
    // MM-1: 신규 주문 + KRX 조직번호(정정/취소용) 캡처
    OrderAck submit_order_ack(const OrderSignal& signal) override;
    // MM-1: 국내 미체결 취소 (order-rvsecncl). 성공 시 취소접수 ODNO, 실패 시 ""
    std::string cancel_order(const std::string& ticker, const std::string& orig_odno,
                             const std::string& krx_orgno, int qty, bool all_remaining) override;
    // MM-1: 국내 정정 (order-rvsecncl). 성공 시 새 ODNO(정정접수번호), 실패 시 ""
    std::string revise_order(const std::string& ticker, const std::string& orig_odno,
                             const std::string& krx_orgno, int new_qty, double new_price) override;
    // 직전 주문/취소/정정의 KIS 오류코드(msg_cd). 성공 시 "". EGW00201(초당한도) 적응재시도 판별용.
    //  주문 3메서드는 단일 order_thread에서만 호출되므로 락 없이 안전(단일 기록자·판독자).
    std::string last_order_error_code() const override { return last_order_msg_cd_; }
    bool is_paper() const override { return cfg_.is_paper; }
    // 미체결(정정취소 가능) 예약주문 조회 — inquire-psbl-rvsecncl (모의 VTTC0084R / 실전 TTTC0084R)
    std::vector<OpenOrder> get_open_orders() override;
    nlohmann::json get_balance();

    // 지수 현재값 (코스피 "0001", 코스닥 "1001", KOSPI200 "2001")
    struct IndexPrice
    {
        std::string ticker;
        double price = 0.0;
        double change = 0.0;
        double change_rate = 0.0;
        int sign = 3; // 1=상한 2=상승 3=보합 4=하한 5=하락
    };
    IndexPrice get_index_price(const std::string& ticker);

    // ── 파생 (선물·옵션) ────────────────────────────────────────────────────
    // 국내 선물/옵션 현재가 — inquire-price (거래ID tr_id=FHMIF10000000).
    //   market_div = FID_COND_MRKT_DIV_CODE("F"=지수선물 등), iscd = 종목코드(예 KOSPI200
    //   최근월물). 시세 REST이므로 실전 도메인 전용 — 모의(openapivts:29443)는 시세 미지원이라
    //   HTTP500이 뜬다. 시세전용(quote) KisClient(is_paper=false)로 호출해야 한다.
    //   output 스키마는 실키 1콜(future_quote_probe, 2026-09-03 확정): output1=계약 시세
    //   (futs_prpr 현재가, futs_prdy_vrss/ctrt, prdy_vrss_sign, futs_oprc/hgpr/lwpr, acml_vol,
    //   hts_otst_stpl_qty 미결제 + delta/gama/theta/vega/rho 그릭스·basis·futs_last_tr_date 만기),
    //   output2/3=기초지수(종합·KOSPI200). 첫 호출 1회 raw를 로그로 남긴다(스키마 변동 대비).
    struct FuturePrice
    {
        std::string iscd;
        double price = 0.0;
        double change = 0.0;       // 전일 대비
        double change_rate = 0.0;  // 전일 대비율(%)
        int sign = 3;              // 1=상한 2=상승 3=보합 4=하한 5=하락
        double open = 0.0;
        double high = 0.0;
        double low = 0.0;
        int64_t volume = 0;        // 누적 거래량
        int64_t open_interest = 0; // 미결제약정(open interest)
        bool ok = false;           // 가격 파싱 성공 여부
    };
    FuturePrice get_future_price(const std::string& iscd, const std::string& market_div = "F");

    // 선물 전광판 — display-board-futures (tr_id FHPIF05030200). 현재 거래가능 선물 계약 목록.
    //   market_cls = FID_COND_MRKT_CLS_CODE("MKI"=KOSPI200 지수선물 등). raw json 반환.
    //   inquire-price에 넣을 최근월물 코드(FID_INPUT_ISCD) 확보용. 실전 도메인 전용.
    nlohmann::json get_future_board(const std::string& market_cls = "MKI",
                                    const std::string& market_div = "F");

    // 시가총액 순위 — 현재가·등락률 포함 전체 데이터
    struct RankingStock
    {
        int rank = 0;
        std::string ticker;
        std::string name;
        double price = 0.0;
        double change = 0.0;      // 전일 대비
        double change_rate = 0.0; // 등락률(%)
        int64_t volume = 0;       // 누적 거래량
        double pbr = 0.0;
        double per = 0.0;
        double trade_value = 0.0; // 누적 거래대금(원) — volume-rank 경로에서만 채워짐
    };
    std::vector<RankingStock> fetch_kr_ranking(int count = 200, const std::string& market_div = "J");

    // 거래대금 상위 순위 — volume-rank API (tr_id FHPST01710000). 상위 ~30행 고정(연속조회 불가).
    //  blng_cls = FID_BLNG_CLS_CODE 정렬축: "0"=거래량 "1"=거래증가율 "3"=거래금액(기본). 축마다
    //  다른 30행이 오므로 여러 축을 union하면 유니버스를 넓힐 수 있다(페이지네이션 대체).
    //  거래대금축("3")일 때만 acml_tr_pbmn 내림차순 재정렬, 그 외엔 API 순위 순서 유지.
    std::vector<RankingStock> fetch_value_ranking(int count = 30, const std::string& market_div = "J",
                                                  const std::string& blng_cls = "3");

    // 전체 시장 PBR 기반 Universe 조회 (ticker만 반환)
    std::vector<std::string> fetch_universe_by_pbr(double max_pbr, const std::string& market_div = "J");

    // 업종 지수 일봉 (sector_code: 코스피 업종 "0001"~"0026" 등)
    std::vector<MarketData> get_index_daily_ohlcv(const std::string& sector_code, int count = 6);

    // 업종별 등락률 순위 — 업종 내 상승 종목 스캔
    std::vector<RankingStock> fetch_sector_ranking(const std::string& sector_code, int count = 30);

    // 당일 장중 외국인·기관 "추정(가집계)" 순매수 — 시장 랭킹 배치 1콜.
    //  endpoint: /uapi/domestic-stock/v1/quotations/foreign-institution-total, tr_id FHPTJ04400000.
    //  per-ticker가 아니라 "지금 담는/던지는 상위 종목" 리스트 → top-30과 교집합해 lookup.
    //  ⚠️ 추정치(확정 아님) — 부호·상대크기만 신뢰. 실전 도메인 전용(모의 HTTP500 추정).
    //  ⚠️ FID 파라미터/필드명은 실전키로 1콜 찍어 확정 필요(스키마 변동 잦음).
    struct EstInvestorFlow
    {
        std::string ticker;             // mksc_shrn_iscd
        std::string name;               // hts_kor_isnm
        int64_t foreign_net_qty = 0;    // frgn_ntby_qty (외국인 추정 순매수 수량, +담기/-던지기)
        int64_t inst_net_qty    = 0;    // orgn_ntby_qty (기관 추정)
        double  foreign_net_amt = 0.0;  // frgn_ntby_tr_pbmn (금액, 원)
        double  inst_net_amt    = 0.0;  // orgn_ntby_tr_pbmn
    };
    // market: "0000"=전체 "0001"=코스피 "1001"=코스닥. sort: "0"=순매수상위 "1"=순매도상위.
    // etc_cls: "0"=전체 "1"=외국인 "2"=기관계 (필드가 한 행에 동거 안 하면 분리 조회).
    std::vector<EstInvestorFlow> fetch_est_investor_ranking(const std::string& market = "0000",
                                                            const std::string& sort = "0",
                                                            const std::string& etc_cls = "0");

    // 투자자별 매매동향 — 외국인·기관 순매수 확인 (단일 최신값)
    struct InvestorTrend
    {
        std::string ticker;
        int64_t foreign_net = 0; // 외국인 순매수 수량 (양수=순매수)
        int64_t inst_net    = 0; // 기관 순매수 수량
    };
    InvestorTrend get_investor_trend(const std::string& ticker);

    // 투자자별 매매동향 일자별 시계열 (수급 전략용 — 최대 30거래일)
    // flows[0] = 가장 최근 거래일, look-ahead 방지는 호출측 책임
    std::vector<InvestorFlow> get_investor_flow(const std::string& ticker,
                                                const std::string& market_div = "J");

    // ── 해외 (US) ──────────────────────────────────────────────────────────
    // exchange: "NAS"(NASDAQ), "NYS"(NYSE)
    std::vector<MarketData> get_us_daily_ohlcv(const std::string& ticker, int count,
                                               const std::string& exchange = "NAS");
    Fundamentals get_us_fundamentals(const std::string& ticker, const std::string& exchange = "NAS");
    bool send_us_order(const OrderSignal& signal);

    // S&P 500 주요 종목 내장 리스트 — PBR 필터 적용 후 반환
    std::vector<std::string> fetch_us_universe_by_pbr(double max_pbr, const std::string& exchange = "NAS");

private:
    std::string http_get(const std::string& url, const std::vector<std::string>& headers);
    std::string http_post(const std::string& url, const std::vector<std::string>& headers, const std::string& body);

    // 토큰 만료 5분 전이면 자동 재발급 (token_mtx_ 하에서 authenticate_locked 호출)
    void ensure_authenticated();
    // 실제 발급/캐시로직 — token_mtx_를 이미 쥔 상태에서만 호출(락 없음). 데드락 방지 분리.
    bool authenticate_locked();
    // 현재 토큰의 락-보호 스냅샷 복사본. 헤더 조립 시 access_token_ 직접 참조 대신 사용
    // — authenticate가 std::string을 재기록하는 순간 다른 스레드가 복사하다 힙 손상되던 레이스 차단.
    std::string token() const
    {
        std::lock_guard<std::mutex> lk(token_mtx_);
        return access_token_;
    }

    std::string base_url() const
    {
        return cfg_.is_paper ? "https://openapivts.koreainvestment.com:29443"
                             : "https://openapi.koreainvestment.com:9443";
    }

    KisConfig cfg_;
    // 토큰 상태(access_token_/token_expires_at_)는 전략·데이터 스레드가 같은 인스턴스를
    // 공유하며 재발급 시 동시 읽기/쓰기가 발생 → token_mtx_로 직렬화(비재귀). 진입점은
    // authenticate()/ensure_authenticated()/token()/is_authenticated() 넷 모두 각자 독립 획득.
    mutable std::mutex token_mtx_;

    // 일봉 캐시 — 같은 종목의 일봉을 유니버스 스캐너·데이터 스레드·전략 프리페치가 겹쳐 부른다.
    //  120봉 추세 판정은 몇 분 사이에 결론이 달라지지 않으므로 TTL 안에서는 받아둔 것을 다시 쓴다.
    //  KIS는 최신봉부터 주므로 더 짧은 요청은 앞부분을 잘라 답하고, 더 긴 요청은 캐시를 못 쓴다.
    //  하루 캐시로 두지 않는 것은 장중 마지막 봉이 당일 미완성 봉이라 계속 갱신되기 때문이다.
    struct DailyCacheEntry
    {
        std::chrono::steady_clock::time_point at;
        int requested = 0; // 이 항목을 만들 때 요청한 봉 수(짧은 이력 종목이 매번 미스 나는 것 방지)
        std::vector<MarketData> bars;
    };
    mutable std::mutex daily_cache_mtx_;
    std::unordered_map<std::string, DailyCacheEntry> daily_cache_;

    // 초당 호출 한도 토큰버킷 — 인스턴스(=app_key)당 하나. 한도는 app_key 단위라 시세 클라이언트와
    //  주문 클라이언트가 각각 자기 예산을 쓴다. 모든 호출이 http_get/http_post를 지나므로
    //  여기서 재우면 우회하는 호출 경로가 없다. 주문·잔고 경로는 예약분을 따로 둬, 시세 조회가
    //  버킷을 비워도 주문이 그 뒤에 줄서지 않게 한다.
    mutable std::mutex rate_mtx_;
    double rate_tokens_ = 0.0;
    std::chrono::steady_clock::time_point rate_last_;
    void rate_limit_acquire(const std::string& url);
    // 서버가 한도 초과를 알려오면 버킷을 비워 다음 호출들을 스스로 늦춘다. 리필률 추정이
    //  실제 한도보다 높았을 때 되돌리는 유일한 경로다(응답이 알려주는 값을 그대로 쓴다).
    void note_rate_limited();

    std::string access_token_;
    std::chrono::system_clock::time_point token_expires_at_;
    std::string last_order_msg_cd_; // 직전 주문/취소/정정 KIS 오류코드(msg_cd), 성공 시 "" — order_thread 전용
};
