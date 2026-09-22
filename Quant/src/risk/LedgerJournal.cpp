#include "risk/LedgerJournal.h"

namespace ledger_journal
{
void put_string(char* destination, size_t capacity, std::string_view text) noexcept
{
    const size_t count = text.size() < capacity - 1 ? text.size() : capacity - 1;
    std::memcpy(destination, text.data(), count);
    destination[count] = '\0';
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
        close();
        std::filesystem::resize_file(path_, sizeof(FileHeader) + existing.applied * sizeof(Record), error_code);
        file_ = open_journal_file(path_, "ab");
    }
}

LedgerJournal::~LedgerJournal()
{
    close();
}

bool LedgerJournal::append(Record& record) noexcept
{
    if (file_ == nullptr)
    {
        return false;
    }

    record.sequence = next_sequence_;
    record.wall_us =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    record.crc32 = record_crc(record);

    if (std::fwrite(&record, sizeof(record), 1, file_) != 1 || std::fflush(file_) != 0)
    {
        return false;
    }

    if (fsync_)
    {
#ifdef _WIN32
        _commit(_fileno(file_));
#else
        ::fsync(fileno(file_));
#endif
    }

    ++next_sequence_;
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
