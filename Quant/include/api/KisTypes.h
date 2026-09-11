#pragma once
// KIS REST 응답을 옮겨 담는 값 타입 — 잔고(inquire-balance)와 선물 전광판(display-board-futures).
//  공개 API가 `nlohmann::json`을 돌려주지 않게 하려고 둔다. 필드명·부재 처리는 디코더
//  (`Quant/include/api/KisRestDecode.h`)가 소유하고, 호출자는 여기 필드만 본다. [why D-059]
#include <optional>
#include <string>
#include <vector>

// 잔고 output1 한 행 = 보유 종목 하나.
struct Holding
{
    std::string ticker;               // [wire] pdno
    std::string name;                 // [wire] prdt_name
    int         qty = 0;              // [wire] hldg_qty. 주
    double      avg_price = 0.0;      // [wire] pchs_avg_pric. 원
    double      eval_pnl = 0.0;       // [wire] evlu_pfls_amt. 평가손익, 원 — 표시 전용
    std::optional<int> sellable_qty;  // [wire] ord_psbl_qty. 필드가 없거나 숫자가 아니면 비어 있다("모름") —
                                      //  호출자는 보유수량을 대신 쓴다. 0은 "매도 가능 0주"라 비어 있음과 다르다
};

// 잔고 전체 — output1은 연속조회 전 페이지를 합친 것, 요약은 output2 첫 페이지.
// [inv] holdings가 비어 있는 것은 "빈 계좌"다. 조회 실패는 여기까지 오지 않고 KisResult가 fail로 든다.
struct AccountBalance
{
    std::vector<Holding> holdings;
    std::optional<double> total_eval_amt;        // [wire] tot_evlu_amt, 없으면 nass_amt(순자산). 원
    std::optional<double> available_cash;        // [wire] prvs_rcdl_excc_amt(가수도정산금), 없으면 dnca_tot_amt(예수금). 원
    std::optional<double> prev_day_total_asset;  // [wire] bfdy_tot_asst_evlu_amt(전일 총자산). 원
};

// 선물 전광판 한 행 = 거래 가능한 계약 하나. 만기 오름차순이라 첫 행이 최근월물.
struct FutureContract
{
    std::string iscd;  // [wire] futs_shrn_iscd — inquire-price의 FID_INPUT_ISCD로 넣는 코드
    std::string name;  // [wire] hts_kor_isnm
};
