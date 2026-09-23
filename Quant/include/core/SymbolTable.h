// 종목 문자열 ↔ 정수 id(SymbolId) 테이블. 수신 스레드가 틱·호가에 id를 찍고, hot path의 캐시·디스패치는
//  id 배열 인덱스로 간다. 등록은 기동·재스캔·처음 보는 종목에서만 일어나고 id는 재사용하지 않는다. [why D-071]
//  읽기는 락 없이 간다 — 고정 크기 배열(재할당 없음) + 열린 주소법 버킷의 원자 id를 release/acquire로 발행.
//  shared_mutex의 읽기 락도 카운터를 고치는 쓰기라 수신 스레드가 여럿이면 캐시라인을 주고받았다(D-106).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <string_view>

namespace symbol
{

using SymbolId = uint32_t;

// 0은 "아직 id를 안 받았다". 기본 초기화된 TradeData·OrderBook이 이 값이라 배선이 빠진 경로가 드러난다.
constexpr SymbolId kNone = 0;

// 국내 현물 종목코드 자릿수 — 거래소 규격이라 config가 아니라 상수다(6이 아니면 종목이 아니다).
constexpr size_t kKoreanTickerLength = 6;

// 표 하나가 드는 종목 수 기본값 — 코스콤 전 종목이 2,500여 개라 8,192면 재스캔 누적분까지 든다.
constexpr size_t kDefaultSymbolCapacity = 8192;

// 국내 현물 종목코드인가 — 숫자 6자리. 운영단말 수동주문·유니버스 스캔·거래소 종목 목록이 같은 판정을 쓴다.
bool is_korean_ticker(std::string_view ticker) noexcept;

// 틱·호가·봉 구조체가 드는 종목 코드 — std::string 대신 고정 배열이라 구조체가 trivially copyable이고 링 복사가
//  memcpy다. 문자열이 필요한 곳(로그·REST·캡처 파일·화면)은 view()·string()로 꺼낸다. 최대 15자, 넘치면 잘린다
//  (KIS 현물 6·선물 8·미국 티커). 암시적으로 string_view가 되지만 std::string은 되지 않는다 — hot path에서
//  할당이 생기면 컴파일이 막히게. [why D-071]
struct Ticker
{
    static constexpr size_t kMax = 15;

    char    data[kMax] = {};
    uint8_t length     = 0;

    constexpr Ticker() = default;

    // string_view 하나만 받는다(const char*·std::string은 그 뒤에 선다) — 두 갈래를 두면 `t == "005930"`이 모호해진다.
    Ticker(std::string_view text);

    // 바이트 루프다 — 길이가 실행 시간에 정해지는 memcpy·memset은 인라인되지 않아 호출 둘이 붙는데, 종목 코드는
    //  여섯 자리라 루프가 더 짧다(SymbolTable 조회 벤치 25.6 → 측정값은 D-106).
    void assign(std::string_view text);

    Ticker& operator=(std::string_view text);

    [[nodiscard]] std::string_view view() const
    {
        return {data, length};
    }

    [[nodiscard]] std::string string() const
    {
        return std::string(data, length);
    }

    [[nodiscard]] bool empty() const
    {
        return length == 0;
    }

    [[nodiscard]] size_t size() const
    {
        return length;
    }

    operator std::string_view() const
    {
        return view();
    }

    friend bool operator==(const Ticker& ticker_a, const Ticker& ticker_b)
    {
        return ticker_a.length == ticker_b.length && std::memcmp(ticker_a.data, ticker_b.data, ticker_a.length) == 0;
    }

    friend bool operator==(const Ticker& ticker, std::string_view begin)
    {
        return ticker.view() == begin;
    }

    friend std::ostream& operator<<(std::ostream& os, const Ticker& ticker)
    {
        return os << ticker.view();
    }
};

static_assert(sizeof(Ticker) == 16);

// 표의 알맹이 — 버킷 배열, 이름 배열, 다음에 줄 번호 한 칸. 이 셋이면 표가 선다.
//  힙에 두면 SymbolTable, 공유 쪽지에 두면 ipc::SharedSymbolDictionary다. 프로세스를 가르면 양쪽이 같은
//  종목에 같은 번호를 써야 하고(주문 요청이 종목을 번호로 나른다, D-071 원칙 6), 그러려면 배열이 공유
//  쪽지에 놓여야 한다. 알맹이만 떼어 두면 해시·탐사·발행 순서는 한 벌로 남는다 — 두 벌이 되면 한쪽만
//  고쳐지고 그 순간 양쪽 번호가 갈린다. [why D-114]
//  [inv] 배열 둘은 표가 사는 동안 자리를 옮기지 않는다. 재할당이 없어야 읽기가 락 없이 간다.
struct TableSlots
{
    std::atomic<SymbolId>* buckets     = nullptr; // 0 = 빈 칸. 삭제 없음, 번호는 한 번 놓이면 안 바뀐다
    Ticker*                names       = nullptr; // [inv] names[id] == 그 번호를 받은 티커(15자 넘으면 잘린 채)
    std::atomic<SymbolId>* count       = nullptr; // 다음에 줄 번호 = 채워진 names 수(0번 자리 포함)
    size_t                 capacity    = 0;       // 번호 상한(0 제외)
    size_t                 bucket_mask = 0;       // 버킷 수 − 1

    [[nodiscard]] bool empty() const noexcept
    {
        return buckets == nullptr;
    }
};

// 버킷 수 — capacity의 2배 이상인 2의 거듭제곱. 절반 넘게 차지 않아 선형 탐사가 반드시 빈 칸에서 끝난다.
[[nodiscard]] size_t bucket_count_for(size_t capacity);

// 있으면 그 번호, 없으면 kNone. 락도 원자 카운터 갱신도 없다 — 버킷 하나 acquire 읽기와 16바이트 비교.
[[nodiscard]] SymbolId table_lookup(const TableSlots& slots, std::string_view ticker);

// 있으면 그 번호, 없으면 새 번호를 놓는다. 가득 차면 kNone.
//  [inv] 쓰기 직렬화는 부르는 쪽 몫이다 — 이 함수는 자물쇠를 잡지 않는다. 읽는 쪽은 자물쇠 없이 들어오므로,
//  읽기와의 약속은 발행 순서(이름 → count → 버킷)로 지킨다.
[[nodiscard]] SymbolId table_insert(const TableSlots& slots, std::string_view ticker);

// 모르는 번호면 빈 Ticker. 값으로 돌려준다(16바이트, 할당 없음).
[[nodiscard]] Ticker table_name(const TableSlots& slots, SymbolId id);

class SymbolTable
{
public:
    // capacity는 id 상한(0 제외). 기본값은 kDefaultSymbolCapacity다.
    explicit SymbolTable(size_t capacity = kDefaultSymbolCapacity);

    // 있으면 그 id, 없으면 새 id. 가득 차면 kNone — 호출 쪽은 문자열 경로로 돌아간다.
    //  읽기(수신 스레드가 틱마다 한 번)는 락도 원자 카운터 갱신도 없다. 삽입만 write_mutex_로 직렬화한다. [why D-071]
    SymbolId intern(std::string_view ticker);

    [[nodiscard]] SymbolId lookup(std::string_view ticker) const
    {
        return table_lookup(slots_, ticker);
    }

    // 모르는 id면 빈 Ticker. 배열이 고정이라 참조도 안전하지만, 호출 쪽이 수명을 생각할 일이 없게 값이다.
    [[nodiscard]] Ticker name(SymbolId id) const
    {
        return table_name(slots_, id);
    }

    // 등록된 종목 수(id 0 제외).
    [[nodiscard]] size_t size() const
    {
        return count_.load(std::memory_order_acquire) - 1;
    }

    [[nodiscard]] size_t capacity() const noexcept
    {
        return slots_.capacity;
    }

private:
    std::unique_ptr<std::atomic<SymbolId>[]> buckets_;
    std::unique_ptr<Ticker[]>                names_;
    std::atomic<SymbolId>                    count_{1};
    TableSlots                               slots_;
    std::mutex                               write_mutex_; // 삽입만 잡는다. 읽기 경로는 잡지 않는다
};

} // namespace symbol
