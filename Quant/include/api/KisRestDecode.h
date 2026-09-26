#pragma once
// KIS REST 응답(JSON) → 값 타입. 분봉 → MarketData 집계봉, 잔고 → AccountBalance, 선물 전광판 → FutureContract.
// 순수 함수. 선언은 여기, 구현은 Quant/src/api/KisRestDecode.cpp. [why D-118]
// 로그·HTTP·인증 의존이 없어 테스트(`Quant/tests/test_kis_decode.cpp`)가 KisClient를
// 링크하지 않고 직접 부른다. 호출 스레드: 데이터 스레드(KisClient::get_minute_ohlcv*·get_balance)와 테스트.
// 관련 결정: D-051(분봉), D-059(잔고·전광판).
//
// [wire] 당일 분봉(FHKST03010200)과 과거일 분봉(FHKST03010230)은 output2에 같은 필드명을 쓴다:
//  stck_bsop_date(YYYYMMDD)·stck_cntg_hour(HHMMSS)·stck_oprc·stck_hgpr·stck_lwpr·stck_prpr·cntg_vol.
//  숫자는 문자열이고, 빈 값·형식 오류는 0으로 둔다 — 봉 하나를 버리는 것보다 0 거래량이 낫다는 판단은 아니고,
//  둘을 구분할 신호가 응답에 없어서다.

#include "api/IOrderExecutor.h"
#include "api/KisResult.h"
#include "api/KisTypes.h"
#include "core/KstTime.h"
#include "core/Types.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace kis_rest
{

// 문자열 숫자 필드 → double. 키 없음·빈 값·파싱 실패·문자열이 아닌 값은 0.
//  키는 리터럴이라 const char* — json이 투명 비교자를 써서 std::string을 만들지 않는다.
double number(const nlohmann::json& node, const char* key);

// YYYYMMDD + HHMMSS → time_t. 자리값을 UTC로 읽는다 — KST 오프셋은 호출자가 뺀다. 서버 TZ와 무관하다.
//  형식이 아니거나 달력에 없는 날짜(13월·2월 30일)면 0.
time_t parse_dt(const std::string& data, const std::string& ticker);

// 1분봉 원본 한 행.
struct RawMinute
{
    std::string date, hour;
    double open = 0, high = 0, low = 0, close = 0;
    int64_t volume = 0;
};

// 1분봉 원본 → interval_min 집계봉. 반환은 최신→과거(result[0]=최신, bar_index 0=최신), 최대 count봉.
//  raws는 정렬을 위해 제자리에서 바뀐다. 두 분봉 TR이 같은 집계를 쓰므로 한 곳에 둔다.
//  버킷은 시계 정렬((hour*60+minute)/interval_min)이라 09:00 기준 3분봉은 09:00·09:03·… 으로 떨어진다.
//  timestamp는 버킷 마지막 1분의 진짜 UTC다(KST 라벨 − 9h). 일봉(now)·체결 틱과 같은 축이어야 집계기 시드와
//  resample이 KST 분을 바로 읽는다 — 라벨을 UTC처럼 두면 시드 자리가 9시간 밀려 틱이 전부 버려진다. [why D-072]
std::vector<MarketData> aggregate_minutes(std::vector<RawMinute>& raw_minutes, const std::string& ticker,
                                                 int interval_min, int count);

// output2(최신→과거) 한 페이지 → raws에 누적. seen(날짜·시각을 한 정수로)으로 페이지 경계 중복을 걸러내고,
//  이 페이지에서 가장 이른 HHMMSS를 돌려준다(역페이징 커서 — 날짜 필터·중복과 무관하게 모든 행을 본다).
//  date_filter가 비어 있지 않으면 그 날짜 행만 취한다. added_out은 이번 호출로 raws에 더한 행 수.
constexpr uint64_t kTimeDigitsSpan = 1'000'000; // HHMMSS 여섯 자리 — 날짜를 그 위 자리로 올린다

std::string parse_minute_page(const nlohmann::json& array, std::vector<RawMinute>& raw_minutes,
                                     std::unordered_set<uint64_t>& seen, const std::string& date_filter,
                                     int& added_out);

// 문자열 숫자 필드 → optional<double>. 키 없음·빈 값·숫자 아님은 비어 있음 — number()의 0과 달리 "없다"를 남긴다.
//  잔고 요약처럼 0원과 필드 부재를 구분해야 하는 곳에 쓴다.
std::optional<double> option_number(const nlohmann::json& node, const char* key);

// 잔고 output1 한 행 → Holding. ord_psbl_qty는 숫자(공백 허용)일 때만 채운다 — 09-08에 이 필드를 보유수량으로
//  대신 썼다가 전량 청산이 40240000으로 통째 거부된 적이 있어, 못 읽은 것은 못 읽었다고 남긴다.
Holding decode_holding(const nlohmann::json& node);

// 잔고 응답 한 페이지 → out에 누적. output1 행은 pdno가 비거나 수량 0 이하면 버린다(잔고는 매도 완료 종목을
//  0주로 며칠 남긴다). 요약(output2)은 first_page일 때만 읽는다 — 배열로도 객체로도 온다.
void decode_balance_page(const nlohmann::json& document, AccountBalance& out, bool first_page);

// 선물 전광판 응답 → 계약 목록. 행 배열은 output1·output2·output 중 처음 비어 있지 않은 것이다
//  (실키 응답이 어느 키로 오는지 문서가 못 박지 않아 셋을 본다). 코드가 빈 행은 버린다.
std::vector<FutureContract> decode_future_board(const nlohmann::json& document);

// 미체결 조회 한 쪽. rows는 종목이 있고 잔여가 0보다 큰 행만 담는다(모의는 취소된 행도 뺀다).
struct OpenOrderPage
{
    std::vector<OpenOrder> rows;
    std::string            forward_key; // ctx_area_fk100, 끝 공백을 뗀 값
    std::string            next_key;    // ctx_area_nk100, 끝 공백을 뗀 값. 비면 마지막 쪽이다
};

// 미체결 조회 응답 본문 한 쪽 → OpenOrderPage. 본문이 비면 "transport", JSON이 아니면 "parse", rt_cd가 "0"이
//  아니면 msg_cd·msg1을 실패로 돌려준다 — 초당 한도(EGW00201) 응답은 rt_cd "1"에 빈 output1로 오고, 그것을
//  "미체결 없음"으로 읽으면 재기동 대조가 살아 있는 주문의 선점을 푼다. [why 전수조사 B1-2]
//  [wire] 모의(VTTC0081R)는 output1·rmn_qty(잔여)·cncl_yn, 실거래(TTTC0084R)는 output·psbl_qty(취소가능).
KisResult<OpenOrderPage> decode_open_order_page(std::string_view response, bool paper);

// 일별주문체결조회 한 쪽. rows는 주문번호가 있고 누적 체결이 0보다 큰 행만 담는다(취소 행은 체결 0이라 빠진다).
struct DailyFillPage
{
    std::vector<DailyOrderFill> rows;
    std::string                 forward_key; // ctx_area_fk100, 끝 공백을 뗀 값
    std::string                 next_key;    // ctx_area_nk100, 끝 공백을 뗀 값. 비면 마지막 쪽이다
};

// 일별주문체결조회 응답 본문 한 쪽 → DailyFillPage. 실패 구분은 decode_open_order_page와 같다 — 한도 초과
//  응답을 "체결 없음"으로 읽으면 놓친 체결을 못 되찾는다.
//  [wire] 모의(VTTC0081R)·실거래(TTTC0081R) 모두 output1 배열에 odno·orgn_odno·pdno·sll_buy_dvsn_cd·ord_qty·
//  tot_ccld_qty·tot_ccld_amt. 실계좌 응답으로 필드 이름 확인(2026-09-27).
KisResult<DailyFillPage> decode_daily_fill_page(std::string_view response);

} // namespace kis_rest
