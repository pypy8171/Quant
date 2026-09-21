// 미체결 선점(reserved_) 상태의 로컬 append-only 저널과 그 리플레이. reserved_는 프로세스 메모리뿐이라
//  재기동하면 비고, KIS 잔고(ord_psbl_qty)는 체결이 끝난 주문에만 반영된다 — 재기동 직후 아직 KIS에
//  미체결로 남은 주문은 이 저널이 아니면 어디서도 보이지 않는다. [why D-101 reserved_ 드리프트]
//  기록은 OrderGate::on_accept·release_reservation·reset_reserved가 positions_mutex_를 쥔 채 동기 append한다.
//  주문 이벤트는 틱과 달리 초당 수십 건 수준이라 TickCapture(core/TickCapture.h)처럼 별도 기록 스레드·큐를
//  두지 않는다 — 매 append 뒤 fflush()까지만 한다(fsync 아님, 프로세스 재기동만 방어 대상이라 OS 버퍼
//  flush로 충분하고 디스크 동기화 지연을 시퀀서에 얹지 않는다).
//  symbol_id·계좌 인덱스는 기동마다 달라지므로(TickCapture.h와 같은 이유) 저널에는 계좌·티커 문자열을
//  쓴다 — 리플레이는 OrderGate::make_key(account, ticker)로 다시 등록한다.
#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string_view>
#include <type_traits>

namespace reservation_journal
{

constexpr size_t kAccountMax = 16;
constexpr size_t kTickerMax  = 12;

struct Record
{
    char    account[kAccountMax] = {};
    char    ticker[kTickerMax]   = {};
    int32_t delta   = 0; // on_accept·release_reservation에 넘긴 델타 그대로(BUY 선점 +, SELL 선점 -)
    double  price   = 0.0;
    int64_t wall_us = 0; // system_clock us — 감사용, 리플레이는 안 씀
};

static_assert(std::is_trivially_copyable_v<Record>);

inline void put_string(char* destination, size_t capacity, std::string_view text)
{
    const size_t count = text.size() < capacity - 1 ? text.size() : capacity - 1;
    std::memcpy(destination, text.data(), count);
    destination[count] = '\0';
}

class ReservationJournal
{
public:
    // 파일을 열지 못하면 ok()가 false고 append는 아무것도 하지 않는다 — 저널 실패가 매매를 막지 않는다.
    explicit ReservationJournal(std::filesystem::path file) : path_(std::move(file))
    {
        open_append();
    }

    ~ReservationJournal()
    {
        close();
    }

    ReservationJournal(const ReservationJournal&)            = delete;
    ReservationJournal& operator=(const ReservationJournal&) = delete;

    [[nodiscard]] bool ok() const noexcept
    {
        return file_ != nullptr;
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

    // [inv] 호출 전에 positions_mutex_를 잡는다 — on_accept·release_reservation과 같은 락 아래서 부른다.
    void append(std::string_view account, std::string_view ticker, int delta, double price) noexcept
    {
        if (file_ == nullptr)
        {
            return;
        }

        Record record;
        put_string(record.account, kAccountMax, account);
        put_string(record.ticker, kTickerMax, ticker);
        record.delta   = delta;
        record.price   = price;
        record.wall_us = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();

        std::fwrite(&record, sizeof(record), 1, file_);
        std::fflush(file_);
    }

    // reset_reserved() 직후 호출 — reserved_가 통째로 비었으니 저널도 비운다. [inv] positions_mutex_ 보유.
    void truncate() noexcept
    {
        close();
        std::error_code error_code;
        std::filesystem::remove(path_, error_code);
        open_append();
    }

    // 기동 시 1회. 파일이 없거나 비어 있으면 apply는 한 번도 불리지 않는다.
    static void replay(const std::filesystem::path& file,
                        const std::function<void(std::string_view, std::string_view, int, double)>& apply)
    {
        std::FILE* handle = std::fopen(file.string().c_str(), "rb");

        if (handle == nullptr)
        {
            return;
        }

        Record record;

        while (std::fread(&record, sizeof(record), 1, handle) == 1)
        {
            apply(std::string_view(record.account), std::string_view(record.ticker), record.delta, record.price);
        }

        std::fclose(handle);
    }

private:
    void open_append()
    {
        std::error_code error_code;
        std::filesystem::create_directories(path_.parent_path(), error_code);
        file_ = std::fopen(path_.string().c_str(), "ab");
    }

    void close() noexcept
    {
        if (file_ != nullptr)
        {
            std::fclose(file_);
            file_ = nullptr;
        }
    }

    std::filesystem::path path_;
    std::FILE*             file_ = nullptr;
};

} // namespace reservation_journal
