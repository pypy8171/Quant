#pragma once
// KIS 접속점(도메인·포트)을 한 곳에 둔다. 모의투자와 실계좌는 REST 도메인·포트와 WebSocket 포트가 다르고, 고르는 기준은
//  KisConfig::is_paper 하나다. 실계좌 전환 때 놓치는 리터럴이 없도록 URL·포트는 여기에만 적는다(T-13-3).
//  시세 REST는 모의 서버가 HTTP 500이라 EngineConfigure가 시세 전용 클라이언트를 is_paper=false로 따로 만든다 — 그 선택도
//  결국 여기 rest_base_url()을 거친다.
//  근거: 모의 서버 시세 500은 실측 — Quant/logs/archive/quant_trader_2026-08-06.log 509행(openapivts 지수 일봉 HTTP 500), docs/DECISIONS.md D-097
//  (2026-09-23, 모의계좌는 시세가 막혀 있다). 공식 샘플은 같은 시세 TR을 모의에도 쓴다고 적어 샘플과 실측이 다르다.
#include <string_view>

namespace kis_endpoints
{

inline constexpr std::string_view kRestBaseUrlReal    = "https://openapi.koreainvestment.com:9443";
inline constexpr std::string_view kRestBaseUrlPaper   = "https://openapivts.koreainvestment.com:29443";
inline constexpr std::string_view kWebSocketHost      = "ops.koreainvestment.com"; // 모의·실계좌 같은 호스트, 포트만 다르다
inline constexpr int              kWebSocketPortReal  = 21000;
inline constexpr int              kWebSocketPortPaper = 31000;
// 근거: 실측 — 이 주소로 운영 중이다(REST는 Quant/logs/archive/quant_trader_2026-08-06.log 509행의 openapivts:29443, 모의 WebSocket 31000은
//  DAILY_LOG.md 2026-06-08 항목). 공식 샘플의 접속 설정 파일은 2026-09-27 MCP로 읽지 못해 주소 자체의 문서 대조는 못 했다.

// REST 기본 URL(경로 없음). 토큰 발급·주문·잔고·시세·WebSocket 승인키(/oauth2/Approval) 전부 이 앞에 붙는다.
// [wire] 출처: KIS 공식 샘플 auth_token(/oauth2/tokenP)·auth_ws_token(/oauth2/Approval)이 REST 기본 URL 뒤에 붙인다, 2026-09-27 MCP 확인.
[[nodiscard]] inline constexpr std::string_view rest_base_url(bool is_paper) noexcept
{
    return is_paper ? kRestBaseUrlPaper : kRestBaseUrlReal;
}

[[nodiscard]] inline constexpr int websocket_port(bool is_paper) noexcept
{
    return is_paper ? kWebSocketPortPaper : kWebSocketPortReal;
}

static_assert(rest_base_url(true) != rest_base_url(false), "모의·실계좌 REST 접속점은 달라야 한다");
static_assert(websocket_port(true) != websocket_port(false), "모의·실계좌 WebSocket 포트는 달라야 한다");

} // namespace kis_endpoints
