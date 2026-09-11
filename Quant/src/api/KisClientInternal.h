// api/KisClientInternal.h — KisClient 구현 파일들이 공유하는 include·상수. 공개 헤더가 아니다.
//  구현은 도메인별로 나뉜다(D-048): KisTransport(전송·한도·인증 헤더) · KisAuth(토큰) · KisMarket(주식 시세)
//  · KisIndex(지수·수급·선물) · KisOrder(주문) · KisAccount(잔고·미체결) · KisUniverse(순위·유니버스).
#pragma once

#include "api/KisClient.h"
#include "api/KisErrorCodes.h"
#include "utils/EtfFilter.h"
#include "utils/Logger.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#ifndef _WIN32
#include <fcntl.h>    // open — fsync용 (V-2)
#include <sys/stat.h> // chmod — 토큰 캐시 0600 (W-4)
#include <unistd.h>   // fsync, close (V-2)
#endif

using json = nlohmann::json;

// KST = UTC+9. UTC now()에 더해 KST 기준 '오늘' 날짜를 뽑는 데 쓴다.
static constexpr int kKstOffsetSec = 9 * 3600;
