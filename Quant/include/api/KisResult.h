#pragma once
// KIS REST 응답 봉투. "값이 비었다"와 "요청이 실패했다"를 한 타입에서 구분한다.
//  잔고가 빈 배열로 오는 경우는 진짜 빈 계좌보다 초당 한도(EGW00201)·서버 오류가 압도적으로 많고, 그걸
//  보유 0으로 읽으면 원장을 비운 채 매매한다(09-09 14:04 25종목 정리, 09-11 09:17 빈 원장 3분). 호출자가
//  `contains("output1")` 같은 응답 모양으로 실패를 추측하지 않게 봉투가 실패를 든다. [why D-059]
// 스레드: 값 타입이라 소유권만 넘긴다. 어느 스레드에서든 만들고 읽는다.
#include <string>
#include <utility>

// [wire] code는 KIS msg_cd(EGW00201=초당 한도 등)이고, 전송·파싱 실패는 각각 "transport"·"parse"로 둔다.
struct KisError
{
    std::string code;
    std::string msg;
};

// 성공이면 value(), 실패면 error()만 의미가 있다. 성공 여부를 안 보고 값을 쓰는 실수를 [[nodiscard]]와
//  explicit bool로 막는다 — `if (auto r = kis.get_balance()) { r->holdings ... }` 꼴로 쓴다.
template <class T> class [[nodiscard]] KisResult
{
public:
    static KisResult ok(T v)
    {
        KisResult r;
        r.ok_    = true;
        r.value_ = std::move(v);
        return r;
    }

    static KisResult fail(std::string code, std::string msg)
    {
        KisResult r;
        r.err_ = KisError{std::move(code), std::move(msg)};
        return r;
    }

    explicit operator bool() const noexcept { return ok_; }
    bool ok() const noexcept { return ok_; }

    // ok()일 때만. 실패 봉투의 value_는 기본 생성값이라 읽어도 터지지는 않지만 의미가 없다.
    const T& value() const& noexcept { return value_; }
    T& value() & noexcept { return value_; }
    T&& value() && noexcept { return std::move(value_); }
    const T& operator*() const noexcept { return value_; }
    const T* operator->() const noexcept { return &value_; }

    const KisError& error() const noexcept { return err_; }

    // 로그 한 줄용 "code msg". 성공이면 빈 문자열.
    std::string error_text() const { return ok_ ? std::string() : err_.code + " " + err_.msg; }

private:
    bool     ok_ = false;
    T        value_{};
    KisError err_;
};
