// 파일 통째로 바꿔 쓰기 구현. 왜 있는지는 utils/AtomicFile.h 머리말.
#include "utils/AtomicFile.h"

#include <chrono>
#include <filesystem>
#include <fstream>

namespace file_io
{

// 임시 이름에 시각을 붙이는 것은 모의·실계좌 엔진 둘이 같은 경로에 쓸 때 서로의 임시 파일을 덮지 않게 하려는 것이다.
bool write_atomically(const std::string& path, const std::string& text)
{
    const std::string temporary =
        path + ".tmp" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);

        if (!file)
        {
            return false;
        }

        file.write(text.data(), static_cast<std::streamsize>(text.size()));

        if (!file)
        {
            return false;
        }
    }

    std::error_code error;
    std::filesystem::rename(temporary, path, error);

    if (error)
    {
        std::filesystem::remove(temporary, error);
        return false;
    }

    return true;
}

} // namespace file_io
