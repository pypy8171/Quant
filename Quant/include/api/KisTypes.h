#pragma once
// KIS REST 응답을 옮겨 담는 값 타입 — 잔고(inquire-balance)·선물 전광판(display-board-futures)·선물 현재가·
//  순위(market-cap·volume-rank)·장중 추정 수급·투자자별 매매동향.
//  공개 API가 `nlohmann::json`을 돌려주지 않게 하려고 둔다. 필드명·부재 처리는 디코더
//  (`Quant/include/api/KisRestDecode.h`)가 소유하고, 호출자는 여기 필드만 본다. [why D-059]
//  선물 현재가·순위·수급 네 타입은 KisClient 안에 중첩돼 있던 것을 옮겼다 — KisClient::RankingStock 같은 이름도 그대로 통한다.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// 잔고 output1 한 행 = 보유 종목 하나.
// [wire] 출처: 아래 필드 이름은 KIS 공식 샘플 inquire_balance 응답 컬럼(pdno·prdt_name·hldg_qty·pchs_avg_pric·
//  evlu_pfls_amt·prpr·ord_psbl_qty, output2의 tot_evlu_amt·nass_amt·prvs_rcdl_excc_amt 가수도정산금액·dnca_tot_amt
//  예수금총금액·bfdy_tot_asst_evlu_amt), 2026-09-27 MCP 확인.
struct Holding
{
    std::string ticker;               // [wire] pdno
    std::string name;                 // [wire] prdt_name
    int         quantity = 0;              // [wire] hldg_qty. 주
    double      average_price = 0.0;      // [wire] pchs_avg_pric. 원
    double      evaluation_pnl = 0.0;       // [wire] evlu_pfls_amt. 평가손익, 원 — 표시 전용
    double      current_price = 0.0;        // [wire] prpr. 현재가, 원 — 총노출 한도(§3d)의 시가. 0이면 모름
    std::optional<int> sellable_quantity;  // [wire] ord_psbl_qty. 필드가 없거나 숫자가 아니면 비어 있다("모름") —
                                      //  호출자는 보유수량을 대신 쓴다. 0은 "매도 가능 0주"라 비어 있음과 다르다
};

// 잔고 전체 — output1은 연속조회 전 페이지를 합친 것, 요약은 output2 첫 페이지.
// [inv] holdings가 비어 있는 것은 "빈 계좌"다. 조회 실패는 여기까지 오지 않고 KisResult가 fail로 든다.
struct AccountBalance
{
    std::vector<Holding> holdings;
    std::optional<double> total_evaluation_amount;        // [wire] tot_evlu_amt, 없으면 nass_amt(순자산). 원
    std::optional<double> available_cash;        // [wire] prvs_rcdl_excc_amt(가수도정산금), 없으면 dnca_tot_amt(예수금). 원
    std::optional<double> previous_day_total_asset;  // [wire] bfdy_tot_asst_evlu_amt(전일 총자산). 원
};

// 선물 전광판 한 행 = 거래 가능한 계약 하나. 만기 오름차순이라 첫 행이 최근월물.
// [wire] 출처: KIS 공식 샘플 display_board_futures(futs_shrn_iscd 선물 단축 종목코드·hts_kor_isnm), 2026-09-27 MCP 확인.
//  "만기 오름차순"은 샘플에 없다 — 근거 없음.
struct FutureContract
{
    std::string issue_code;  // [wire] futs_shrn_iscd — inquire-price의 FID_INPUT_ISCD로 넣는 코드
    std::string name;  // [wire] hts_kor_isnm
};

// 국내 선물/옵션 현재가 한 건 — inquire-price(FHMIF10000000) output1. 필드 출처는 KisClient::get_future_price 주석.
struct FuturePrice
{
    std::string issue_code;
    double price = 0.0;
    double change = 0.0;       // 전일 대비
    double change_rate = 0.0;  // 전일 대비율(%)
    int sign = 3;              // 1=상한 2=상승 3=보합 4=하한 5=하락
    // 근거: api/IMarketDataSource.h IndexPrice::sign 주석 참고(값 1~5의 공식 출처를 찾지 못했다).
    double open = 0.0;
    double high = 0.0;
    double low = 0.0;
    int64_t volume = 0;        // 누적 거래량
    int64_t open_interest = 0; // 미결제약정(open interest)
    bool ok = false;           // 가격 파싱 성공 여부
};

// 순위 한 행 — 시가총액(market-cap)·거래대금(volume-rank) 순위가 같이 쓴다. 현재가·등락률·시가총액 포함.
struct RankingStock
{
    int rank = 0;
    std::string ticker;
    std::string name;
    double price = 0.0;
    double change = 0.0;      // 전일 대비
    double change_rate = 0.0; // 등락률(%)
    int64_t volume = 0;       // 누적 거래량
    double market_cap = 0.0;  // 시가총액(억원) — market-cap 경로에서만 채워짐
    double trade_value = 0.0; // 누적 거래대금(원) — volume-rank 경로에서만 채워짐
};

// 장중 외국인·기관 추정 순매수 한 행 — foreign-institution-total(FHPTJ04400000). 출처는 KisClient::fetch_est_investor_ranking 주석.
struct EstInvestorFlow
{
    std::string ticker;             // mksc_shrn_iscd
    std::string name;               // hts_kor_isnm
    int64_t foreign_net_quantity = 0;    // frgn_ntby_qty (외국인 추정 순매수 수량, +담기/-던지기)
    int64_t institution_net_quantity    = 0;    // orgn_ntby_qty (기관 추정)
    double  foreign_net_amount = 0.0;  // frgn_ntby_tr_pbmn (금액, 원)
};

// 투자자별 매매동향 — 외국인·기관 순매수 확인 (단일 최신값)
struct InvestorTrend
{
    std::string ticker;
    int64_t foreign_net = 0; // 외국인 순매수 수량 (양수=순매수)
    int64_t institution_net    = 0; // 기관 순매수 수량
};
