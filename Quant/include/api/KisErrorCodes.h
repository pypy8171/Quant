#pragma once

// KIS(한국투자증권) OpenAPI가 주문 거부 시 응답 메시지에 담는 오류코드 문자열.
// OrderAck::err_code가 이 값을 담는다(D-039). reject_reason 꼬리표 " [코드]"에도 같은 문자열이 남는다.
namespace kis_err
{
// KIS 코드가 아닌 자체 코드 — 응답이 없거나(전송 실패) 응답을 못 읽었을 때. 접수 여부를 모른다.
inline constexpr const char* kTransport = "E_TRANSPORT";
// KIS가 rt_cd≠0으로 거부했는데 msg_cd가 비어 있을 때.
inline constexpr const char* kUnknown = "E_UNKNOWN";
// 초당 거래건수 초과 — KIS가 '접수 전' 단계에서 거부한다(중복주문 위험 없음).
inline constexpr const char* kRateLimit = "EGW00201";
// 주문가능수량 없음("잔고내역이 없습니다") — 보유분이 예약매도·미결제로 묶여 매도 불가.
inline constexpr const char* kNoSellableQty = "40240000";
} // namespace kis_err
