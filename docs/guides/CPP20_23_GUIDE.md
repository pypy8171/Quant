# C++20 · C++23 문법 안내 — 이 저장소에 쓰는 것 기준

`Quant/CMakeLists.txt`의 표준을 17에서 23으로 올리면서, 새로 쓰게 되는 문법을 저장소 코드의 전·후로 적어 둔다.
각 항목은 (1) 무엇인지 (2) 문법 (3) 이 저장소의 어느 자리에 쓰는지 순서다. 표준 승격 자체의 결정과 적용 순서는
`docs/DECISIONS.md` D-070를 본다.

컴파일러 지원 요약. Windows는 MSVC 14.44(VS 2022 17.14)가 아래 전부를 지원한다. Linux는 `Quant/Dockerfile`이
Ubuntu 24.04 + g++-14다.

| 기능 | 표준 | GCC 최소 |
|---|---|---|
| `std::format`, `<chrono>` 달력·시간대 | 20 | 13 |
| `std::jthread`, `std::span`, `atomic::wait`, ranges, `starts_with`, `<=>`, 지정 초기화, concepts | 20 | 11 |
| `std::expected`, `string::contains`, `std::to_underlying`, `optional::transform` | 23 | 12 |
| `ranges::to`, `std::print`, `std::generator` | 23 | 14 |

---

## 1. `std::format` — 문자열 조립

`printf` 서식의 안전판. 타입을 인자에서 추론하고 서식은 `{}` 안에 쓴다. 자리 표시자와 인자 수가 안 맞으면
컴파일 오류다(서식 문자열이 상수일 때).

```cpp
#include <format>
std::string s = std::format("{} ({} > {})", name, qty, limit);      // 순서대로
std::string t = std::format("{:.2f}", price);                       // 소수 2자리
std::string u = std::format("{:06}", n);                            // 0 채움 6자리 → 000042
std::string v = std::format("{:<8}|{:>6}|{:+.1f}%", a, b, pct);     // 왼쪽·오른쪽 정렬, 부호 강제
std::format_to(std::back_inserter(buf), "{},{}\n", x, y);           // 기존 문자열 뒤에 이어 붙임(할당 1회)
```

서식 지정자는 `{[인덱스]:[채움][정렬][부호][너비][.정밀도][타입]}`. 정렬은 `<` `>` `^`, 타입은 `d x f e s` 등.

이 저장소의 전·후.

```cpp
// 전 — Quant/src/risk/OrderGate.cpp 거부 사유. ostringstream 생성이 check() 경로마다 든다.
std::ostringstream ss;
ss << "1주문 수량 한도 초과 (" << sig.quantity << " > " << cfg_.max_qty_per_order << ")";
return ss.str();

// 후
return std::format("1주문 수량 한도 초과 ({} > {})", sig.quantity, cfg_.max_qty_per_order);
```

```cpp
// 전 — Quant/src/ipc/OrderRouter.cpp 원장 CSV 행. setprecision·fixed 상태가 스트림에 남는다.
f << ev << ',' << mo.order_id << ',' << std::fixed << std::setprecision(2) << sig.price << ...;

// 후 — 소수 자리를 자리마다 명시한다.
std::format("{},{},{:.2f},...", ev, mo.order_id, sig.price, ...);
```

`"..." + std::to_string(x) + "..."` 사슬(로그 문장 약 200곳)도 같은 방식이다. 로그 매크로는 완성된 문자열을
받으므로 `LOG_INFO(std::format("[gate] {} 거부 {}", ticker, reason))`처럼 감싼다.

C++23 `std::print("{}\n", x)`는 `printf`를 대신하는 출력 함수다. 콘솔 출력 도구(`Quant/src/modes/Monitors.cpp`)에
쓸 수 있고, 엔진 로그는 로거 경로를 유지한다.

---

## 2. `<chrono>` 달력과 시간대

C++17 `<chrono>`는 시각(time_point)과 기간(duration)만 있었다. C++20은 날짜·시각 분해·시간대가 들어왔다.
`gmtime_s/gmtime_r`·`strftime`·`_mkgmtime/timegm`의 플랫폼 `#ifdef` 쌍이 없어진다.

```cpp
#include <chrono>
using namespace std::chrono;
using namespace std::chrono_literals;   // 9h, 30min, 500ms 리터럴

sys_seconds now = floor<seconds>(system_clock::now());   // UTC 초 단위 time_point
auto kst  = now + 9h;                                     // 한국 시각(고정 오프셋)
sys_days  d = floor<days>(kst);                           // 날짜 부분
year_month_day ymd{d};                                    // 년·월·일
hh_mm_ss  tod{kst - d};                                   // 시·분·초

int y = int(ymd.year());  unsigned m = unsigned(ymd.month());  unsigned dd = unsigned(ymd.day());
auto h = tod.hours().count();  auto mi = tod.minutes().count();
weekday wd{d};                                            // Monday, Tuesday…  wd == Saturday
sys_days back = d - days{7};                              // 날짜 산술
sys_seconds ts = sys_days{year{2026}/9/13} + 9h + 30min;  // 문자열 없이 시각 합성
std::string s = std::format("{:%Y%m%d}", ymd);            // 서식 출력(GCC 13+)
```

시간대(tzdb)가 필요하면 `zoned_time{"Asia/Seoul", now}`. 이 저장소는 UTC+9 고정으로 충분해서 `9h` 덧셈을 쓴다.

이 저장소의 전·후.

```cpp
// 전 — Quant/include/core/KstTime.h
inline struct tm to_tm(std::time_t now_utc) {
    std::time_t kt = now_utc + kOffsetSec;  struct tm out{};
#ifdef _WIN32
    gmtime_s(&out, &kt);
#else
    gmtime_r(&kt, &out);
#endif
    return out;
}
inline std::string ymd(std::time_t t) { char buf[9]; std::strftime(buf, sizeof buf, "%Y%m%d", &tm); ... }

// 후 — 플랫폼 분기 없음, 시험 가능한 순수 함수
inline std::string ymd(std::time_t now_utc) {
    const auto d = std::chrono::floor<std::chrono::days>(std::chrono::sys_seconds{std::chrono::seconds{now_utc}} + 9h);
    const std::chrono::year_month_day ymd{d};
    return std::format("{:04}{:02}{:02}", int(ymd.year()), unsigned(ymd.month()), unsigned(ymd.day()));
}
```

```cpp
// 전 — Quant/include/api/KisRestDecode.h parse_dt: stoi(substr) 여섯 번 + try/catch + _mkgmtime/timegm
// 후 — from_chars 여섯 번 뒤 합성. 예외·libc 호출 없음.
const auto tp = std::chrono::sys_days{std::chrono::year{y}/m/d} + std::chrono::hours{h} + std::chrono::minutes{mi} + std::chrono::seconds{s};
return tp.time_since_epoch().count();
```

주의. `Quant/src/ipc/OrderRouter.cpp` `today_ymd()`와 `Quant/src/risk/OrderGate.cpp` `session_remaining_ratio()`는
`localtime`을 쓴다. 지금 머신이 KST라 `KstTime`과 같은 값을 내지만, UTC 호스트에서는 갈린다. 승격하면서
`kst::ymd`로 통일한다(동작 변경이므로 따로 커밋).

---

## 3. `std::jthread`와 `std::stop_token` — 스레드 수명

`std::thread`는 소멸자에서 `joinable()`이면 `std::terminate`라 반드시 손으로 `join()`해야 했다.
`std::jthread`는 소멸자가 `request_stop()` 뒤 `join()`을 부른다. 스레드 함수의 첫 인자로 `std::stop_token`을
받으면 정지 요청을 그 토큰으로 본다.

```cpp
#include <thread>
#include <stop_token>

std::jthread worker([this](std::stop_token st) {
    while (!st.stop_requested()) { ... }
});
worker.request_stop();          // 정지 요청. 소멸자도 같은 일을 한다.

// 자면서 정지 요청도 받기 — condition_variable_any만 stop_token 오버로드가 있다.
std::condition_variable_any cv;
cv.wait_for(lock, st, 100ms, [&]{ return !queue.empty(); });   // 정지 요청이 오면 즉시 깬다

std::stop_callback cb(st, [&]{ cv.notify_all(); });            // 정지 요청 시 실행할 콜백
```

이 저장소의 전·후.

```cpp
// 전 — Quant/src/ipc/OrderRouter.cpp 미체결 취소 스레드
std::atomic<bool> stale_stop_{false};
std::thread       stale_thread_;
...
stale_stop_.store(false);
stale_thread_ = std::thread([this, rows] { for (auto& r : rows) { if (stale_stop_.load()) return; ... } });
...
~OrderRouter() { stale_stop_.store(true); if (stale_thread_.joinable()) stale_thread_.join(); }

// 후 — 플래그·소멸자 처리 삭제
std::jthread stale_thread_;
stale_thread_ = std::jthread([this, rows](std::stop_token st) { for (auto& r : rows) { if (st.stop_requested()) return; ... } });
```

`Quant/include/strategy/DeviationScaleStrategy.h`의 프리페치 스레드(`prefetch_stop_` 리셋 뒤 재기동)도 같다.
`Engine`의 다섯 스레드는 `running_`을 다른 곳(상태 JSON·WS 콜백·폴러 keep_going)도 읽으므로 플래그는 두고
`jthread`로 join만 자동화한다. 종료 순서(제어→주문→전략→데이터, 체결은 WS 끊은 뒤)는 지금처럼 명시한다.

---

## 4. `std::span` — 포인터+길이 한 쌍

연속 메모리의 (시작, 길이)를 한 값으로 담는 뷰. 소유하지 않고, 복사가 싸고(포인터 둘), `std::vector`·배열·
`std::array`에서 암묵 변환된다. 길이를 컴파일 시간에 고정할 수도 있다(`std::span<const std::byte, 32>`).

```cpp
#include <span>
void feed(std::span<const uint8_t> bytes);       // (const uint8_t*, size_t) 대신
feed(buf);                                       // std::vector<uint8_t>
feed({buf.data(), n});                           // 포인터·길이에서
for (auto b : bytes.subspan(4, 8)) ...           // 부분 뷰
std::as_bytes(span) / std::as_writable_bytes(span)   // std::byte 뷰로
```

이 저장소의 전·후.

```cpp
// 전 — Quant/include/api/KisWsDecode.h. 손으로 만든 span.
struct Fields {
    const std::string_view* data;  size_t count;
    std::string_view operator[](size_t i) const { return i < count ? data[i] : std::string_view{}; }
};

// 후 — 범위 밖 접근을 빈 값으로 돌려주던 동작은 호출자에서 size() 검사로 옮긴다.
using Fields = std::span<const std::string_view>;
```

```cpp
// 전 — Quant/src/api/WsSocket.h. 길이 검사가 구현 두 벌(Win·Posix)에 각각 있다.
std::string aes_cbc_decrypt(const std::string& cipher, const std::string& key, const std::string& iv);

// 후 — 32·16이 타입이 된다. 잘못된 길이는 호출 지점에서 걸린다.
std::string aes_cbc_decrypt(std::span<const std::byte> cipher, std::span<const std::byte, 32> key, std::span<const std::byte, 16> iv);
```

`Quant/include/ipc/OpsProtocol.h` `FrameReader::feed(const uint8_t*, size_t)`도 같은 교체다.

2단계에서는 `Fields`만 바꿨다. AES 키·IV는 config에서 읽은 `std::string`이라 길이가 런타임 값이고,
고정 길이 span으로 받아도 호출 지점에서 `std::string` → `span<…,32>` 변환에 같은 길이 검사가 다시
필요하다 — 검사가 옮겨질 뿐 사라지지 않는다. `FrameReader::feed`는 MFC 단말까지 호출자가 바뀌어
치환 범위를 넘는다. 둘 다 그 코드를 다른 이유로 만질 때 같이 한다.

---

## 5. `std::atomic<T>::wait / notify_one` — 잠들기와 깨우기

C++20부터 원자 변수 자체에 대기 함수가 있다. `wait(old)`는 값이 `old`와 같은 동안 잔다(리눅스 futex, Windows
`WaitOnAddress`). `notify_one()`은 그 변수에서 자는 스레드 하나를 깨운다. 뮤텍스·condvar·"잔다" 플래그·fence가
원자 변수 하나로 준다.

```cpp
std::atomic<uint32_t> seq_{0};
// 생산자
queue.push(x);
seq_.fetch_add(1, std::memory_order_release);
seq_.notify_one();
// 소비자
for (;;) {
    uint32_t seen = seq_.load(std::memory_order_acquire);
    while (auto item = queue.pop()) handle(*item);
    seq_.wait(seen, std::memory_order_acquire);    // seq_가 seen과 다르면 즉시 반환
}
```

`wait`에는 시간 제한이 없다. 종료 경로는 플래그를 바꾸고 `notify_all()`을 반드시 부른다.

이 저장소의 전·후. `Quant/include/utils/Logger.h`의 writer와 `Quant/src/core/Engine.cpp`의 체결 소비 스레드가
"`sleeping_.store(true)` · `fence(seq_cst)` · 큐 비었으면 `cv.wait_for`" 패턴이고, 생산자는 "`fence` · `sleeping_`이면
`notify_one`"이다. 위 순번 카운터로 바꾸면 `wake_mtx_`·`wake_cv_`·`writer_sleeping_`·fence 둘이 없어진다.
D-045 실측(120ns, p99 0.4µs)이 기준선이라 바꾼 뒤 `test_logger`·`test_pipeline_stress`를 다시 잰다. 적용 순서 마지막.

---

## 6. 문자열 — `starts_with` · `ends_with`(20) · `contains`(23)

```cpp
s.starts_with("EGW")         // s.rfind("EGW", 0) == 0
s.ends_with(".json")
s.contains("한도")            // s.find("한도") != std::string::npos   (C++23)
```

`std::string`·`std::string_view` 둘 다 있다. `Quant/include/risk/GateReasons.h`, `Quant/src/core/OrderPacer.cpp`,
`Quant/src/api/KisTransport.cpp`의 16곳.

로그에 응답 앞 200자만 남길 때 `resp.substr(0, 200)`은 새 문자열을 만든다. `std::string_view(resp).substr(0, 200)`은
할당이 없다. `std::format("{}", sv)`에 그대로 넣을 수 있다.

---

## 7. `<bit>` — 2의 거듭제곱·비트 연산

```cpp
#include <bit>
std::bit_ceil(n)         // n 이상 최소 2^k
std::has_single_bit(n)   // n이 2^k인가
std::countl_zero(x), std::countr_zero(x), std::popcount(x)
std::bit_cast<To>(from)  // memcpy 없는 타입 재해석(같은 크기, trivially copyable)
```

```cpp
// 전 — Quant/include/core/RingBuffer.h
static size_t round_up_pow2(size_t n) { size_t p = 1; while (p < n) p <<= 1; return p; }
// 후
static constexpr size_t round_up_pow2(size_t n) { return std::bit_ceil(std::max<size_t>(n, 2)); }
```

---

## 8. `[[likely]]` · `[[unlikely]]`

분기의 기대 방향을 컴파일러에 알린다. 측정 없이 남발하지 않고, 큐의 full/empty처럼 "거의 안 일어나는" 분기에만 둔다.

```cpp
if (head - tail_.load(std::memory_order_acquire) == capacity_) [[unlikely]] { return false; }
```

---

## 9. 비교 연산자 기본 정의 — `operator<=>`, `operator== = default`

```cpp
struct PosKey {
    std::string account, ticker;
    bool operator==(const PosKey&) const = default;          // 멤버 순서대로 비교
    auto operator<=>(const PosKey&) const = default;         // <, <=, >, >= 전부 생성(std::map 키로 쓸 때)
};
```

`<=>`는 세 방향 비교(three-way comparison). 결과 타입은 `std::strong_ordering` 등이고 `< 0`, `== 0`으로 판정한다.
C++20부터 `a != b`는 `!(a == b)`로 자동 재작성되므로 `!=`를 따로 쓰지 않는다.
`Quant/include/risk/OrderGate.h` `PosKey`의 손 `==`가 한 줄이 된다. 해시(`PosKeyHash`)는 표준에 없어 그대로 둔다.

---

## 10. 열거형·상수 — `using enum` · `consteval` · `constinit` · `std::to_underlying`(23)

```cpp
std::string_view to_sv(Regime r) {
    using enum Regime;                       // 스코프 안에서 Regime:: 생략
    switch (r) { case BULL: return "BULL"; case BEAR: return "BEAR"; ... }
}

consteval int square(int x) { return x * x; }   // 반드시 컴파일 시간에 평가. 런타임 인자면 오류.
constinit std::atomic<int> g_counter{0};         // 정적 초기화 강제(동적 초기화 순서 문제 차단)
std::to_underlying(side)                          // static_cast<std::underlying_type_t<Side>>(side)
```

`Quant/src/core/RegimeController.cpp` `to_string(Regime)`이 `std::string`을 돌려주며 할당하는데, `constexpr std::string_view`
표로 바꾼다. 표 크기는 `static_assert(table.size() == kRegimeCount)`로 잡는다.

---

## 11. Ranges — 알고리즘에 컨테이너를 통째로, 투영(projection)

`begin/end` 쌍 대신 컨테이너를 넘긴다. 세 번째 인자 "투영"은 각 원소에서 비교할 값을 꺼내는 함수(멤버 포인터 가능).

```cpp
#include <algorithm>
#include <ranges>
std::ranges::sort(v, std::ranges::greater{}, &Feat::score);      // score 내림차순
auto it = std::ranges::min_element(cands, {}, &Cand::z);         // z 최소
std::ranges::find_if(strategies_, [&](auto& s){ return s->id() == id; });
std::ranges::all_of(r.ticker, ::isdigit);

auto sells = held
           | std::views::filter([](auto& p){ return p.sellable > 0; })
           | std::views::transform(make_sell)
           | std::ranges::to<std::vector>();                       // to<>는 C++23
```

`views::`는 지연 평가 뷰다(복사 없이 순회). `Quant/src/universe/UniverseScanner.cpp` 정렬 7곳, `Quant/src/core/SignalDispatcher.cpp`
`force_liq_orders`/`trim_orders`의 "필터 뒤 변환" 루프에 쓴다.

---

## 12. 지정 초기화자(designated initializers)

집합체(aggregate)를 필드 이름으로 초기화한다. 선언 순서를 지켜야 하고, 빠진 필드는 기본 멤버 초기화 값이 든다.

```cpp
// 전 — Quant/tests/test_order_router.cpp
OrderAck ack{odno, orgno, std::string()};
OrderSignal s; s.ticker = "005930"; s.side = OrderSide::SELL; s.quantity = 10; s.ref_price = 70000;

// 후
OrderAck ack{.odno = odno, .krx_orgno = orgno};
OrderSignal s{.ticker = "005930", .side = OrderSide::SELL, .quantity = 10, .ref_price = 70000};
```

`Quant/include/core/Types.h`의 `OrderSignal`·`MarketData`·`TradeData`는 이미 집합체라 그대로 된다. 테스트의 필드별
대입 약 120곳이 대상이다.

---

## 13. `std::expected<T, E>`(23) — 값 또는 오류

`std::optional`에 "왜 실패했나"가 붙은 것. `KisResult<T>`가 이것과 같은 계약이다.

```cpp
#include <expected>
std::expected<Balance, KisError> get_balance();
auto r = get_balance();
if (!r) { LOG_WARN(std::format("잔고 실패 {}", r.error().code)); return; }
use(r->holdings);   // 또는 (*r).holdings, r.value()

return std::unexpected(KisError{code, msg});   // 실패 반환
return Balance{...};                            // 성공 반환(암묵 변환)

// 단조 연산 — 페이지네이션처럼 "성공이면 다음"을 잇는다
auto total = fetch_page(1).and_then(merge_next).transform(to_summary).or_else(log_and_default);
```

`Quant/include/api/KisResult.h`의 `ok()`·`fail()`·`operator bool`·`*`·`->`·`error()`가 1:1로 대응하고,
실패 경로의 `T value_{}` 기본 생성이 없어진다. `error_text()`는 자유 함수로 남긴다.

`std::optional`에도 C++23에서 같은 `transform/and_then/or_else`가 들어왔다. `Quant/include/core/RegimeFileBridge.h`의
`std::optional<bool> entry_halt`를 `if (o.entry_halt) gate.set_entry_halt(*o.entry_halt)` 대신
`o.entry_halt.transform(apply_halt)`(람다 `apply_halt`가 `gate.set_entry_halt(v)`를 부른다)로 쓸 수 있으나 가독성 이득이 작아 강제하지 않는다.

---

## 14. Concepts와 `requires` — 템플릿 인자 제약

`enable_if` 대신 조건을 이름 붙여 쓴다. 오류 메시지가 "제약 불만족: T는 nothrow move 가능해야 함" 식으로 읽힌다.

```cpp
#include <concepts>
template <class T>
concept QueueItem = std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_assignable_v<T>;

template <QueueItem T>              // 또는 template <class T> requires QueueItem<T>
class RingBuffer { ... };

void f(std::integral auto n);       // 축약 함수 템플릿
template <class F> requires std::invocable<F, const OrderSignal&>
void for_each_signal(F&& fn);
```

이 저장소는 `enable_if`가 0곳이라 교체 대상은 없고, `RingBuffer<T>`·`MpscQueue<T>`에 "예외 없는 이동" 제약을
선언으로 적는 용도다. 지금은 주석과 `static_assert`가 그 역할을 한다.

---

## 15. 람다·기타 언어 변경

```cpp
auto f = []<typename T>(std::span<T> s) { ... };     // 템플릿 람다
[=, this]                                            // [=]의 암묵 this 캡처는 C++20에서 deprecated
std::source_location::current()                      // __FILE__/__LINE__ 대신. 로거 매크로가 파일·줄을 자동으로 받을 때
if consteval { ... } else { ... }                    // C++23. 컴파일 시간 평가 중인지 분기
std::unreachable();                                  // C++23. 도달 불가 표시(switch default)
auto x = 0uz;                                        // C++23. size_t 리터럴
```

`volatile` 복합 대입(`sink += x`)은 C++20에서 deprecated다(C++23 P2327이 `|=`·`&=`·`^=`만 되살렸고 산술은 그대로).
GCC 14는 `-Wvolatile`로 경고한다. 벤치마크의 최적화 방지 sink는 `sink = sink + x`로 쓴다 — 읽고 쓰는 횟수가 같아
벤치 기준선이 안 바뀐다(`Quant/tests/bench_*.cpp`, `test_*_stress.cpp`). `std::atomic` relaxed로 바꾸는 안은
x86에서 store가 같은 mov라 지연은 같지만 sink 뜻이 흐려져 쓰지 않았다.

---

## 16. 이번에 쓰지 않는 것

- 코루틴(`co_await`/`co_yield`). WS 재연결·REST 페이지네이션이 상태기계 모양이지만 블로킹 I/O 스레드 위라
  비동기 소켓 계층 없이는 얻는 것이 없다. `std::generator`(23)로 페이지네이션을 감싸는 것은 가독성 이득만 있어 보류.
- 모듈(`import std;`). MSVC·CMake 3.28+에서 되지만 헤더 조각 구조에서 빌드 시간 이득이 불확실하고 GCC 지원이 아직 고르지 않다.
- `std::atomic_ref`, `std::latch`/`std::barrier`. 필요한 자리가 없다. 부하 테스트의 스레드 동시 출발에 `std::barrier`를 쓸 수는 있다.

---

## 17. 적용 순서(D-070)

1. 표준 23·CMake 3.20·Dockerfile 24.04, 벤치 `volatile` sink를 `sink = sink + x`로. 빌드·ctest 22·벤치 1회로 기준선.
2. 위험 0 묶음: `span`, `starts_with/contains`, `bit_ceil`, `PosKey == default`, `ranges::sort`. 지정 초기화·`using enum`은
   바꿀 자리가 없어 새 코드에서만 쓴다.
3. `std::format` — 게이트·라우터부터. `bench_gate_contention`·`bench_market_firehose` 전·후 비교.
4. `<chrono>` 달력 — `parse_dt`·`ymd_of` 통합, `localtime` 두 곳을 `kst::`로.
5. `jthread` — 교과서 2곳 먼저, Engine은 join만.
6. `KisResult` → `std::expected`.
7. `atomic::wait` — 마지막. 스트레스 테스트 반복 뒤.

### 17-1. 단계마다 무엇이 좋아지는가 — 이득이 없으면 그 단계는 하지 않는다

표준을 올리는 것 자체는 이득이 아니다. 1단계는 뒤 단계를 가능하게 하는 발판이고 동작·성능은 그대로다
(벤치 E2E p50 300ns, C++17과 같다). 단계마다 기대 이득과 재는 방법을 적어 두고, 재서 이득이 없으면 되돌린다.

| 단계 | 바뀌는 자리 | 무엇이 좋아지는가 | 성능 | 재는 방법 |
|---|---|---|---|---|
| 1 툴체인 | CMake·Dockerfile·MFC 1줄 | 컴파일러 진단이 두 세대 앞으로(C2445가 실제 형식 불일치 하나를 잡았다). GCC 14의 `-Wvolatile`·`-Wdeprecated` | 같음 | 경고 0, ctest 22, 벤치 동일 — 완료 |
| 2 위험 0 치환 | `KisWsDecode.h` `Fields` → `span`, `rfind(x,0)==0` → `starts_with` 6곳, `RingBuffer`·`MpscQueue` 2^k 루프 → `bit_ceil`, `PosKey` `== default`, 단일 키 정렬 9곳 → `ranges::sort`+투영 | 손 구현 삭제, 정렬 람다의 `a.x > b.x` 오타 자리(멤버 포인터 하나로), 비교 연산자 누락 클래스 제거 | 같음(모두 인라인·constexpr) | 삭제 줄 수, ctest, `bench_market_firehose` 전후 — 완료: 코드 14파일 +48/−73, ctest 22, E2E p50 300ns·p99 9.5us(1단계와 같음). AES `span<…,32>`·`FrameReader` span은 4절 끝의 이유로 미적용 |
| 3 `std::format` | `OrderGate.cpp` 거부 사유·중복 키, `OrderRouter.cpp` CSV 행·로그 | `snprintf` 버퍼 크기·`%d`/`%ld` 형식 불일치가 컴파일 오류로. 문장이 한 줄에 보인다 | 거부 사유는 거부된 신호에만 만들어지므로 hot path 밖. `OrderGate.cpp`의 `dedup_key` 조립은 신호마다 일어나므로 재야 한다 — `snprintf`보다 느리면 그 한 곳은 `format_to`+고정 버퍼로 | `bench_gate_contention` 전후, 거부 문장 바이트 동일 검사(`test_order_gate`) |
| 4 `<chrono>` 달력 | `KstTime.h`, `OrderRouter.cpp` `today_ymd`, `OrderGate.cpp`, `KisRestDecode.h` `parse_dt` | `localtime`(머신 TZ)과 `gmtime+9h`(KST 고정)가 섞인 것을 한 벌로 — Docker `TZ` 설정이나 Windows 시간대가 달라도 원장 날짜가 같다. 날짜 산술을 `year_month_day`로 | 같음 | `test_market_session` 확장(TZ가 UTC·KST·PST일 때 같은 결과), 원장 CSV 날짜 열 diff |
| 5 `jthread` | `OrderRouter.cpp` stale 스레드, `DeviationScaleStrategy.h` 프리페치 | 정지 깃발·`join` 누락·소멸 순서 실수 클래스 제거. 소멸자가 정지 요청과 join을 한다 | 같음 | 종료 경로 반복 100회(기동→정지) 교착 0, ctest |
| 6 `std::expected` | `KisResult.h` | 손 봉투 유지보수 종료, `and_then`/`or_else` 체이닝, 실패 경로의 `T value_{}` 기본 생성이 사라져 잔고·전광판 값 타입이 기본 생성자를 요구하지 않는다 | 같음 | `test_kis_decode`·`test_ledger_reconciler` 무수정 통과가 목표 |
| 7 `atomic::wait` | `Logger.h` writer, `Engine.cpp` 체결 스레드 | mutex+condvar 쌍이 atomic 하나로. 생산자의 `notify` 비용(락 없음)과 깨우는 지연이 줄 수 있다 | 줄 가능성 — 재서 정한다 | `bench_logger` 깨우기 p99, `test_logger`·`test_pipeline_stress` |

하지 않는 것(모듈·코루틴·`atomic_ref`)의 이유는 16절과 D-070 표.
