#include "risk/LedgerJournal.h"

#include "utils/Logger.h"

#include <format>

namespace ledger_journal
{
// 묶음 쓰기 목록의 처음 용량. 잠금 한 번 사이에 쌓이는 레코드는 보통 몇 건이라 넉넉하다(W-2).
constexpr size_t kBatchReserve = 256;

namespace
{
// 레코드 묶음을 쓰고 fflush, 켜져 있으면 fsync까지 한다. 셋 중 하나라도 실패하면 거짓이다 — fsync가 실패한 묶음도
//  디스크에 남았다고 볼 수 없어 쓰기 실패와 같이 다룬다.
bool write_records(std::FILE* file, const std::vector<Record>& records, bool fsync)
{
    if (std::fwrite(records.data(), sizeof(Record), records.size(), file) != records.size())
    {
        return false;
    }

    if (std::fflush(file) != 0)
    {
        return false;
    }

    if (!fsync)
    {
        return true;
    }

#ifdef _WIN32
    return _commit(_fileno(file)) == 0;
#else
    return ::fsync(fileno(file)) == 0;
#endif
}

// 파일에 온전히 남아 있어야 하는 길이 — 헤더 + 열 때 있던 레코드 + 이번 실행에서 쓰기를 마친 레코드.
//  [inv] 실패한 묶음은 그 자리에서 잘라 내므로 파일은 늘 이 길이다. 이번 실행의 순번은 열 때 마지막 순번 다음부터
//  빈틈없이 이어지고, 실패 구간(failed_ranges)을 빼면 쓰기를 마친 수가 된다.
uint64_t committed_bytes(const ReplayResult& opened, uint64_t flushed_through,
                         const std::vector<std::pair<uint64_t, uint64_t>>& failed_ranges)
{
    uint64_t session_records = flushed_through > opened.last_sequence ? flushed_through - opened.last_sequence : 0;

    for (const auto& [first, last] : failed_ranges)
    {
        session_records -= last - first + 1;
    }

    return sizeof(FileHeader) + (opened.applied + session_records) * sizeof(Record);
}
} // namespace

void put_string(char* destination, size_t capacity, std::string_view text) noexcept
{
    const size_t count = text.size() < capacity - 1 ? text.size() : capacity - 1;
    std::memcpy(destination, text.data(), count);
    destination[count] = '\0';
}

void put_fill_detail(Record& record, const FillDetail& detail) noexcept
{
    std::memset(record.reason, 0, sizeof(record.reason));
    std::memcpy(record.reason, &detail, sizeof(detail));
}

std::FILE* open_journal_file(const std::filesystem::path& file, const char* mode)
{
#ifdef _WIN32
    const std::wstring wide_mode(mode, mode + std::strlen(mode));

    return _wfopen(file.c_str(), wide_mode.c_str());
#else
    return std::fopen(file.c_str(), mode);
#endif
}

uint32_t crc32(const void* data, size_t length) noexcept
{
    const auto* bytes = static_cast<const unsigned char*>(data);
    uint32_t value = 0xFFFFFFFFu;

    for (size_t index = 0; index < length; ++index)
    {
        value = detail::kCrcTable[(value ^ bytes[index]) & 0xFFu] ^ (value >> 8);
    }

    return value ^ 0xFFFFFFFFu;
}

LedgerJournal::LedgerJournal(const std::filesystem::path& directory, std::string_view date_yyyymmdd, bool fsync)
    : path_(directory / (std::string("ledger_") + std::string(date_yyyymmdd) + ".bin")), fsync_(fsync)
{
    std::error_code error_code;
    std::filesystem::create_directories(directory, error_code);

    const ReplayResult existing = replay(path_, nullptr);
    opened_ = existing;

    if (std::filesystem::exists(path_, error_code) && std::filesystem::file_size(path_, error_code) > 0 &&
        !existing.header_ok)
    {
        return; // 있는 파일의 헤더가 다르다 — 덮어쓰지 않는다
    }

    next_sequence_ = existing.last_sequence + 1;
    pending_.reserve(kBatchReserve);
    writing_.reserve(kBatchReserve);
    file_ = open_journal_file(path_, "ab");

    if (file_ == nullptr)
    {
        return;
    }

    if (!existing.header_ok)
    {
        FileHeader header;
        header.record_size = sizeof(Record);
        header.date_yyyymmdd = static_cast<uint32_t>(std::strtoul(std::string(date_yyyymmdd).c_str(), nullptr, 10));

        if (std::fwrite(&header, sizeof(header), 1, file_) != 1 || std::fflush(file_) != 0)
        {
            close();
            return;
        }
    }
    else if (existing.truncated_tail)
    {
        // 꼬리의 깨진 레코드는 리플레이가 무시했다. 그 뒤에 이어 쓰면 판독기도 같은 자리에서 멈추므로 잘라 낸다.
        //  못 자르면 열지 않는다 — 깨진 꼬리 뒤에 쓴 레코드는 다음 리플레이가 읽지 못한다(ok()가 거짓이라 기동 거부).
        close();
        std::filesystem::resize_file(path_, sizeof(FileHeader) + existing.applied * sizeof(Record), error_code);

        if (error_code)
        {
            LOG_ERROR(std::format("[LedgerJournal] 깨진 꼬리를 잘라 내지 못해 저널을 열지 않는다 - file({}) error({})",
                                  path_.filename().string(), error_code.value()));
            return;
        }

        file_ = open_journal_file(path_, "ab");
    }
}

LedgerJournal::~LedgerJournal()
{
    const FlushResult result = flush();

    if (result.failed > 0)
    {
        LOG_ERROR(std::format("[LedgerJournal] 닫기 전 마지막 기록 실패 - records({}) first_kind({})", result.failed,
                              result.first_failed_kind));
    }

    close();
}

uint64_t LedgerJournal::stage(Record& record)
{
    if (file_ == nullptr)
    {
        return 0;
    }

    std::lock_guard<std::mutex> lock(stage_mutex_);
    record.sequence = next_sequence_++;
    record.wall_us =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    record.crc32 = record_crc(record);
    pending_.push_back(record);
    return record.sequence;
}

LedgerJournal::FlushResult LedgerJournal::flush()
{
    FlushResult result;
    // 자물쇠 밖에서 남길 로그 재료 — 잘라 낸 길이, 다시 열었는지.
    bool     rolled_back     = false;
    bool     reopened        = false;
    uint64_t committed_size  = 0;
    {
        std::lock_guard<std::mutex> write_lock(write_mutex_);

        {
            std::lock_guard<std::mutex> stage_lock(stage_mutex_);
            writing_.swap(pending_);
        }

        if (writing_.empty())
        {
            return result;
        }

        // 레코드는 seq 순으로 붙어 있어 한 번의 fwrite로 나간다. 도중에 실패하면 이 묶음 전체를 못 쓴 것으로 센다.
        const bool wrote = file_ != nullptr && write_records(file_, writing_, fsync_);

        if (!wrote)
        {
            if (file_ != nullptr)
            {
                // 일부만 쓰인 바이트를 남긴 채 이어 쓰면, 다음 리플레이가 그 자리에서 멈추고 다음 기동이 꼬리를 자르며
                //  그 뒤의 정상 레코드까지 버린다. 이 묶음 앞 길이로 되돌리고 다시 연다. 못 열면 닫힌 채 두어
                //  stage()가 0을 돌려주고, 주문은 written() 거짓으로 거부된다.
                committed_size = committed_bytes(opened_, flushed_through_, failed_ranges_);
                close();
                std::error_code error_code;
                std::filesystem::resize_file(path_, committed_size, error_code);

                if (!error_code)
                {
                    file_ = open_journal_file(path_, "ab");
                }

                rolled_back = true;
                reopened    = file_ != nullptr;
            }

            result.failed            = writing_.size();
            result.first_failed_kind = writing_.front().kind;
            failed_ranges_.emplace_back(writing_.front().sequence, writing_.back().sequence);
        }

        flushed_through_ = writing_.back().sequence;
        writing_.clear();
    }

    if (rolled_back && reopened)
    {
        LOG_ERROR(std::format("[LedgerJournal] 쓰기 실패 묶음을 파일에서 잘라 냈다 - records({}) size({})", result.failed,
                              committed_size));
    }
    else if (rolled_back)
    {
        LOG_ERROR(std::format("[LedgerJournal] 쓰기 실패 뒤 저널을 다시 열지 못해 닫았다, 이후 주문은 거부된다 - "
                              "records({}) file({})",
                              result.failed, path_.filename().string()));
    }

    return result;
}

bool LedgerJournal::written(uint64_t sequence) const
{
    std::lock_guard<std::mutex> lock(write_mutex_);

    if (sequence == 0 || sequence > flushed_through_)
    {
        return false;
    }

    for (const auto& [first, last] : failed_ranges_)
    {
        if (sequence >= first && sequence <= last)
        {
            return false;
        }
    }

    return true;
}

ReplayResult LedgerJournal::replay(const std::filesystem::path& file, const std::function<void(const Record&)>& apply)
{
    ReplayResult result;
    std::FILE* handle = open_journal_file(file, "rb");

    if (handle == nullptr)
    {
        return result;
    }

    FileHeader header;

    if (std::fread(&header, sizeof(header), 1, handle) != 1 || header.magic != kMagic || header.version != kVersion ||
        header.record_size != sizeof(Record))
    {
        std::fclose(handle);
        return result;
    }

    result.header_ok = true;
    Record record;

    while (true)
    {
        const size_t read = std::fread(&record, 1, sizeof(record), handle);

        if (read == 0)
        {
            break;
        }

        if (read != sizeof(record) || record.crc32 != record_crc(record))
        {
            result.truncated_tail = true;
            break;
        }

        if (apply)
        {
            apply(record);
        }

        ++result.applied;
        result.last_sequence = record.sequence;
    }

    std::fclose(handle);
    return result;
}

void LedgerJournal::close() noexcept
{
    if (file_ != nullptr)
    {
        std::fclose(file_);
        file_ = nullptr;
    }
}

} // namespace ledger_journal
