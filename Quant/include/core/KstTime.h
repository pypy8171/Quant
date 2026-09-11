#pragma once
// UTC 초 → KST 분해 시각. 거래일(YYYYMMDD)·틱 시각(HHMMSS)·장 시간 판정이 같은 변환을 쓴다. [why D-062]
#include <ctime>
#include <string>

namespace kst
{
inline constexpr int kOffsetSec = 9 * 3600;

// gmtime의 플랫폼 차이(gmtime_s/gmtime_r)를 한 곳에 둔다. 로컬 타임존에 기대지 않는다.
inline struct tm to_tm(std::time_t now_utc)
{
    std::time_t kt = now_utc + kOffsetSec;
    struct tm   out{};
#ifdef _WIN32
    gmtime_s(&out, &kt);
#else
    gmtime_r(&kt, &out);
#endif
    return out;
}

// 거래일 YYYYMMDD. 손익 기준선 파일과 날짜별 표식 파일이 쓴다.
inline std::string ymd(std::time_t now_utc)
{
    const struct tm t = to_tm(now_utc);
    char            buf[9];
    std::strftime(buf, sizeof(buf), "%Y%m%d", &t);
    return std::string(buf);
}

// 틱 시각 HHMMSS. REST 대체 틱의 TradeData.time이 WS 체결(H0STCNT0)과 같은 모양을 갖게 한다.
inline std::string hhmmss(std::time_t now_utc)
{
    const struct tm t = to_tm(now_utc);
    char            buf[7];
    std::strftime(buf, sizeof(buf), "%H%M%S", &t);
    return std::string(buf);
}
} // namespace kst
