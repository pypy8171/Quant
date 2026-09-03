#pragma once

// KIS(한국투자증권) OpenAPI가 주문 거부 시 응답 메시지에 담는 오류코드 문자열.
// 여러 소스에서 reject_reason.find()로 이 값을 찾으므로, 한 곳에 모아 드리프트를 막는다.
namespace kis_err
{
// 초당 거래건수 초과 — KIS가 '접수 전' 단계에서 거부한다(중복주문 위험 없음).
inline constexpr const char* kRateLimit = "EGW00201";
// 주문가능수량 없음("잔고내역이 없습니다") — 보유분이 예약매도·미결제로 묶여 매도 불가.
inline constexpr const char* kNoSellableQty = "40240000";
} // namespace kis_err
