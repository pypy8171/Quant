// 원장 저널 — 주문·체결·잔고 대조가 원장(OrderGate)을 바꾸기 전에 먼저 적는 append-only 파일과 그 리플레이.
//  증권사 원장과 같은 순서를 지킨다: 주문은 INTENT가 적힌 뒤에만 KIS로 나가고, KIS 응답은 ACCEPT/REJECT로,
//  체결통보는 FILL로 뒤따라 적힌다. 재기동은 오늘 파일을 처음부터 다시 적용해 보유·평단·선점·매도가능·현금을
//  되살리고, KIS 잔고는 그 뒤 대조에만 쓴다. [why D-113]
//  기록은 OrderGate가 positions_mutex_를 쥔 채 동기 append한다 — 주문 이벤트는 초당 수십 건이라 별도 스레드·큐를
//  두지 않는다. 매 append 뒤 fflush(프로세스 재기동 방어)까지가 기본이고, config `ledger_journal_fsync`가 참이면
//  fsync까지 한다(전원 장애 방어, 주문 스레드에 디스크 동기화 지연이 얹힌다).
//  파일은 거래일마다 하나(ledger_YYYYMMDD.bin) — KIS 주문은 하루를 넘기지 않으므로 어제 선점은 오늘 의미가 없고,
//  당일 손익도 새 파일에서 0부터 센다. 종목 id·계좌 인덱스는 기동마다 달라져(TickCapture.h와 같은 이유) 문자열로
//  남기고, 리플레이가 OrderGate::make_key로 다시 등록한다.
//  레코드는 고정 길이 + CRC32. 꼬리의 불완전·CRC 불일치 레코드는 그 자리에서 리플레이를 멈춘다(그 앞까지만 정본).
//  열을 더할 때는 예비 필드를 쓰고, 뜻이 바뀌면 kVersion을 올린다 — 파이썬 판독기(PYQuant/tools/ledger_dump.py)가
//  같은 배치를 struct로 읽는다.
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace ledger_journal
{

constexpr uint32_t kMagic   = 0x47444C51u; // "QLDG" 리틀엔디언
constexpr uint32_t kVersion = 1;

constexpr size_t kAccountMax  = 16;
constexpr size_t kTickerMax   = 12;
constexpr size_t kStrategyMax = 24;
constexpr size_t kReasonMax   = 48;

// 레코드 종류. 값은 파일에 남으므로 바꾸지 않고 끝에만 더한다.
enum class Kind : uint16_t
{
    SEED           = 1, // 기동 시 KIS 잔고 한 종목(quantity·price=평단·sellable)
    INTENT         = 2, // KIS 전송 직전 — 선점(reserved_)이 여기서 잡힌다
    ACCEPT         = 3, // KIS 접수(주문번호 확보). 상태 변화 없음, INTENT를 확정
    REJECT         = 4, // KIS 거부·전송 예외 — 선점 해제
    FILL           = 5, // 체결통보 — 보유·평단·매도가능·손익 갱신, 선점 해제
    CANCEL         = 6, // 취소 확인 — 남은 선점 해제
    ADJUST         = 7, // 잔고 대조가 한 종목을 KIS 값으로 맞춤(quantity·price·sellable·reserved_quantity)
    RESET_RESERVED = 8, // 잔고 대조가 선점 전체를 비움
    CASH           = 9, // 주문가능현금·총평가금 스냅샷(cash·equity)
    DAILY_PNL      = 10, // 잔고 대조가 당일 손익을 절대치로 덮어씀(pnl) — 체결 누적과 별개
};

struct FileHeader
{
    uint32_t magic         = kMagic;
    uint32_t version       = kVersion;
    uint32_t record_size   = 0;
    uint32_t date_yyyymmdd = 0;
};

static_assert(sizeof(FileHeader) == 16);

struct Record
{
    uint64_t sequence              = 0; // 파일 안 1부터 증가. 파이썬 적재기가 멱등 키로 쓴다
    int64_t  wall_us          = 0; // system_clock us
    uint64_t order_id         = 0; // 내부 주문 번호(OrderSignal.client_order_number). 주문과 무관한 레코드는 0
    uint64_t kis_order_number = 0; // ACCEPT/FILL/CANCEL — KIS ODNO 정수
    uint16_t kind             = 0; // Kind
    uint8_t  side             = 0; // OrderSide::Value(0=BUY 1=SELL 2=NONE)
    uint8_t  order_type       = 0; // OrderType(0=MARKET 1=LIMIT)
    int32_t  quantity          = 0; // INTENT/REJECT/FILL/CANCEL 수량, SEED/ADJUST 보유수량
    int32_t  reserved_quantity = 0; // ADJUST — 맞춘 뒤 선점(BUY +, SELL -)
    int32_t  sellable          = 0; // SEED/ADJUST 매도가능수량(-1 = 모름)
    double   price   = 0.0; // INTENT 선점가, FILL 체결가, SEED/ADJUST 평단
    double   cash    = 0.0; // CASH 주문가능현금(원)
    double   equity  = 0.0; // CASH 총평가금(원)
    double   pnl     = 0.0; // FILL 이번 체결 실현손익(참고), DAILY_PNL 덮어쓴 당일 손익
    char     account[kAccountMax]   = {};
    char     ticker[kTickerMax]     = {};
    char     strategy[kStrategyMax] = {};
    char     reason[kReasonMax]     = {}; // REJECT 사유, ADJUST 어느 대조가 맞췄는지
    uint32_t reserved0 = 0;
    uint32_t reserved1 = 0;
    uint32_t crc32     = 0; // 이 필드를 0으로 둔 레코드 전체의 CRC32. 패딩 없이 192바이트가 딱 맞아야 CRC가 결정적이다
};

static_assert(sizeof(Record) == 192, "레코드 크기가 바뀌면 kVersion을 올리고 ledger_dump.py의 struct 포맷을 같이 고친다");

void put_string(char* destination, size_t capacity, std::string_view text) noexcept;

// CRC32(IEEE 802.3, zlib과 같은 다항식) — 표는 컴파일 타임에 만든다.
// path::string()은 와이드 경로를 프로세스 코드페이지로 되돌린다 — 사용자 폴더 이름에 한글이 들어 있으면
//  매핑이 없어 예외를 던지고, 저널을 못 열면 엔진이 기동을 거부한다. Windows에서는 와이드 그대로 연다. [why D-113]
std::FILE* open_journal_file(const std::filesystem::path& file, const char* mode);

namespace detail
{
// CRC32 표 크기 — 한 바이트가 가질 수 있는 값의 수.
constexpr size_t kCrcTableSize = 256;

constexpr std::array<uint32_t, kCrcTableSize> make_crc_table()
{
    std::array<uint32_t, kCrcTableSize> table{};

    for (uint32_t index = 0; index < kCrcTableSize; ++index)
    {
        uint32_t value = index;

        for (int bit = 0; bit < 8; ++bit)
        {
            value = (value & 1u) ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
        }

        table[index] = value;
    }

    return table;
}

inline constexpr std::array<uint32_t, kCrcTableSize> kCrcTable = make_crc_table();
} // namespace detail

uint32_t crc32(const void* data, size_t length) noexcept;

inline uint32_t record_crc(Record record) noexcept
{
    record.crc32 = 0;
    return crc32(&record, sizeof(record));
}

struct ReplayResult
{
    bool     header_ok      = false; // 파일이 있고 헤더가 이 버전과 맞음
    uint64_t applied        = 0;     // apply를 부른 레코드 수
    bool     truncated_tail = false; // 꼬리에 불완전·CRC 불일치 레코드가 있어 거기서 멈춤
    uint64_t last_sequence       = 0;
};

class LedgerJournal
{
public:
    // directory/ledger_<date>.bin 을 append로 연다. 파일이 없으면 헤더를 쓰고, 있으면 헤더를 검사하고 마지막 seq를
    //  읽어 이어 쓴다. 못 열거나 헤더가 다르면 ok()가 false다 — 호출자(Engine)는 그때 기동을 거부한다. [why D-113]
    LedgerJournal(const std::filesystem::path& directory, std::string_view date_yyyymmdd, bool fsync);

    ~LedgerJournal();

    LedgerJournal(const LedgerJournal&)            = delete;
    LedgerJournal& operator=(const LedgerJournal&) = delete;

    [[nodiscard]] bool ok() const noexcept
    {
        return file_ != nullptr;
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

    [[nodiscard]] uint64_t next_sequence() const noexcept
    {
        return next_sequence_;
    }

    // 열 때 훑어 본 결과 — 이어 쓸 자리를 찾느라 이미 한 번 읽었다. 꼬리를 잘랐는지는 여기에만 남는다(자른 뒤
    //  다시 읽으면 멀쩡해 보인다). 호출자의 리플레이는 이 결과에 적용 건수만 채워 넣는다.
    [[nodiscard]] const ReplayResult& opened() const noexcept
    {
        return opened_;
    }

    // seq·시각·CRC를 채워 한 레코드를 붙인다. 거짓이면 디스크에 남지 않은 것이다 — 호출자는 그 변경을 되돌리고
    //  주문을 거부한다(적히지 않은 주문은 나가지 않는다). [inv] positions_mutex_ 아래서 부른다.
    [[nodiscard]] bool append(Record& record) noexcept;

    // 파일을 처음부터 읽어 레코드마다 apply를 부른다(nullptr이면 세기만). 헤더가 다르면 header_ok=false로 바로 돌아온다.
    //  꼬리의 불완전·CRC 불일치 레코드에서 멈춘다 — 그 앞까지가 정본이다.
    static ReplayResult replay(const std::filesystem::path& file, const std::function<void(const Record&)>& apply);

private:
    void close() noexcept;

    std::filesystem::path path_;
    bool                  fsync_    = false;
    uint64_t              next_sequence_ = 1;
    ReplayResult          opened_;
    std::FILE*            file_     = nullptr;
};

} // namespace ledger_journal
