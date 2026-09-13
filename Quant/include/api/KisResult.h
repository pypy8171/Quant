#pragma once
// KIS REST 응답 봉투. "값이 비었다"와 "요청이 실패했다"를 한 타입에서 구분한다.
//  잔고가 빈 배열로 오는 경우는 진짜 빈 계좌보다 초당 한도(EGW00201)·서버 오류가 압도적으로 많고, 그걸
//  보유 0으로 읽으면 원장을 비운 채 매매한다(09-09 14:04 25종목 정리, 09-11 09:17 빈 원장 3분). 호출자가
//  `contains("output1")` 같은 응답 모양으로 실패를 추측하지 않게 봉투가 실패를 든다. [why D-059]
// 스레드: 값 타입이라 소유권만 넘긴다. 어느 스레드에서든 만들고 읽는다.
#include <expected>
#include <string>
#include <utility>

// [wire] code는 KIS msg_cd(EGW00201=초당 한도 등)이고, 전송·파싱 실패는 각각 "transport"·"parse"로 둔다.
struct KisError
{
    std::string code;
    std::string msg;
};

// 성공이면 `*r`·`r->`·`r.value()`, 실패면 `r.error()`만 의미가 있다. 성공 여부를 안 보고 값을 쓰는 실수는
//  반환 함수의 [[nodiscard]]와 explicit bool로 막는다 — `if (auto r = kis.get_balance()) { r->holdings ... }` 꼴로 쓴다.
//  실패 반환은 `return kis_fail(code, msg);`, 성공 반환은 값을 그대로 `return v;`(암묵 변환).
//  실패 봉투에는 값이 없다 — 예전 손 봉투와 달리 실패 상태에서 `*r`·`r->`는 정의되지 않는다. [why D-070]
template <class T> using KisResult = std::expected<T, KisError>;

inline std::unexpected<KisError> kis_fail(std::string code, std::string msg)
{
    return std::unexpected(KisError{std::move(code), std::move(msg)});
}

// 로그 한 줄용 "code msg". 성공이면 빈 문자열.
template <class T> std::string error_text(const KisResult<T>& r)
{
    return r ? std::string() : r.error().code + " " + r.error().msg;
}
