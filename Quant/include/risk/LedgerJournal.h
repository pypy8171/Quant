// 원장 저널 — 주문·체결·잔고 대조가 원장(PositionLedger)에 준 변경을 순서대로 남기는 append-only 파일과 그 리플레이.
//  주문만 KIS보다 먼저 적는다: INTENT가 적힌 뒤에만 KIS로 나가고, 못 적으면 선점을 되돌려 주문을 내지 않는다.
//  FILL·REJECT·CANCEL·SEED는 원장을 바꾼 뒤 같은 positions_mutex_ 안에서 순번을 받아, 파일 순서가 원장 갱신 순서와 같다.
//  재기동은 오늘 파일을 처음부터 다시 적용해 보유·평단·선점·매도가능·현금을 되살린다. config
//  `bootstrap_ledger_from_balance`가 참이면 그 뒤 KIS 잔고로 보유를 덮어쓰고 SEED를 적는다
//  (`Quant/src/core/LedgerReconciler.cpp`의 bootstrap). [why D-113]
//  쓰기는 두 단계다. 원장 잠금 안에서는 stage()로 순번을 받아 메모리 버퍼에 쌓기만 하고, 잠금을 푼 뒤 flush()가
//  모아 쓴다 — 디스크가 느린 순간에도 주문 판정·원장 읽기가 디스크를 기다리지 않는다. 먼저 flush()에 온 스레드가
//  뒤에 쌓인 것까지 같이 써서 fsync 한 번이 여러 건을 덮는다. 별도 스레드는 두지 않는다(주문 이벤트는 초당 수십 건).
//  flush마다 fflush(프로세스 재기동 방어)까지가 기본이고, config `ledger_journal_fsync`가 참이면 fsync까지 한다
//  (전원 장애 방어). 잠금 안에서 쓰던 때 fsync를 켜면 원장 읽기가 최대 77ms 막혔다(bench_gate_contention
//  journal=2, 09-25). [why CODE_REVIEW W-2]
//  파일은 거래일마다 하나(ledger_YYYYMMDD.bin) — KIS 주문은 하루를 넘기지 않으므로 어제 선점은 오늘 의미가 없고,
//  당일 손익도 새 파일에서 0부터 센다. 종목 id·계좌 인덱스는 기동마다 달라져(TickCapture.h와 같은 이유) 문자열로
//  남기고, 리플레이가 LedgerKeys::make로 다시 등록한다.
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
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

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
    RESET_DAY      = 11, // 장 시작 하루 리셋 — 선점 전체 만료. reserved0 = 그 KST 거래일(yyyymmdd)
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

// CRC32(IEEE 802.3, zlib과 같은 다항식) — 표는 컴파일 타임에 만든다.
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

    // 열 때 훑어 본 결과 — 이어 쓸 자리를 찾느라 이미 한 번 읽었다. 꼬리를 잘랐는지는 여기에만 남는다(자른 뒤
    //  다시 읽으면 멀쩡해 보인다). 호출자의 리플레이는 이 결과에 적용 건수만 채워 넣는다.
    [[nodiscard]] const ReplayResult& opened() const noexcept
    {
        return opened_;
    }

    // 한 번 모아 쓴 결과 — failed는 디스크에 못 남긴 레코드 수, first_failed_kind는 그 첫 레코드의 종류.
    struct FlushResult
    {
        uint64_t failed            = 0;
        uint16_t first_failed_kind = 0;
    };

    // seq·시각·CRC를 채워 버퍼에 쌓고 seq를 돌려준다(파일이 없으면 0). 디스크는 건드리지 않는다 — 원장 잠금 안에서
    //  불러 파일 순서를 원장 갱신 순서와 맞춘다.
    uint64_t stage(Record& record);

    // 쌓인 레코드를 한 번에 쓴다. 여러 스레드가 불러도 쓰기는 한 줄로 선다. 원장 잠금을 쥔 채 부르지 않는다.
    FlushResult flush();

    // seq가 디스크에 남았는지. flush() 뒤에 묻는다 — 거짓이면 그 레코드는 파일에 없다. 호출자는 그 변경을 되돌리고
    //  주문을 거부한다(적히지 않은 주문은 나가지 않는다).
    [[nodiscard]] bool written(uint64_t sequence) const;

    // 파일을 처음부터 읽어 레코드마다 apply를 부른다(nullptr이면 세기만). 헤더가 다르면 header_ok=false로 바로 돌아온다.
    //  꼬리의 불완전·CRC 불일치 레코드에서 멈춘다 — 그 앞까지가 정본이다.
    static ReplayResult replay(const std::filesystem::path& file, const std::function<void(const Record&)>& apply);

private:
    void close() noexcept;

    std::filesystem::path path_;
    bool                  fsync_    = false;
    ReplayResult          opened_;
    std::FILE*            file_     = nullptr;
    // [lock-order] write_mutex_ → stage_mutex_(잎). stage_mutex_ 안에서는 메모리만 만진다.
    mutable std::mutex    stage_mutex_;
    std::vector<Record>   pending_;           // stage_mutex_ — 아직 안 쓴 레코드, seq 순
    uint64_t              next_sequence_ = 1; // stage_mutex_
    mutable std::mutex    write_mutex_;
    std::vector<Record>   writing_;           // write_mutex_ — pending_와 맞바꿔 들고 나와 쓴다(버퍼를 번갈아 재사용)
    uint64_t              flushed_through_ = 0; // write_mutex_ — 이 seq까지 쓰기를 마쳤다(성공·실패 모두)
    std::vector<std::pair<uint64_t, uint64_t>> failed_ranges_; // write_mutex_ — 못 쓴 seq 구간(양 끝 포함)
};

} // namespace ledger_journal
