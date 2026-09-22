// api/HttpGet.h — KIS 밖 주소를 부르는 HTTP 조회 한 줄. 인증 헤더도, KIS 초당 한도 버킷도 붙지 않는다.
//  구현은 api/KisTransport.cpp에 둔다 — 플랫폼별 전송(WinHTTP/libcurl)이 이미 거기 있고, 복사본을 하나 더
//  만들지 않으려는 것이다. 부르는 쪽이 헤더를 전부 준다.
#pragma once

#include <string>
#include <vector>

namespace http
{

// 성공하면 응답 본문, 실패하면 빈 문자열. GET이라 전송 실패·5xx는 전송부가 두 번까지 다시 보낸다.
std::string get(const std::string& url, const std::vector<std::string>& headers);

} // namespace http
