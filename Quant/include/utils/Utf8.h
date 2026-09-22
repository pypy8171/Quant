#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

// UTF-8 문자열 터미널 표시폭 계산 및 패딩 유틸
// CJK 문자는 2칸, ASCII는 1칸으로 계산
//
// 선두 바이트 값으로 문자 길이를 판별한다(UTF-8 규격):
//   <0x80  = ASCII 1바이트(폭 1)
//   <0xE0  = 2바이트 시퀀스(폭 2 — 라틴 확장·한글 자모 등)
//   <0xF0  = 3바이트 시퀀스(폭 2 — 한글 완성형·CJK 등)
//   그 외  = 4바이트 시퀀스(폭 2 — 이모지·보조평면)

namespace utf8 {

int display_width(const std::string& text);

std::string pad_right(const std::string& text, int target);

std::string truncate(const std::string& text, int max_width);

// 바이트열이 제대로 된 UTF-8인지 본다 — 이어지는 바이트 개수·과다 인코딩·서러게이트·U+10FFFF 초과를 거른다.
bool is_valid_utf8(std::string_view text);

// 설정 json의 문자열은 UTF-8이다(nlohmann). 그 바이트를 그대로 std::filesystem::path에 넣으면 Windows는
//  프로세스 ANSI 코드페이지(CP949)로 읽어 변환에 실패한다 — 사용자 폴더 이름에 한글이 든 리플레이 경로에서 기동 중
//  예외로 죽었다(09-22 실측, "No mapping for the Unicode character exists in the target multi-byte code page").
//  char8_t 생성자는 입력을 UTF-8로 규정하므로 코드페이지를 타지 않는다.
//  다만 같은 자리에 path::string()이 만든 코드페이지 바이트가 들어오기도 한다(test_engine이 temp 경로를 그렇게 넘긴다).
//  그걸 UTF-8로 읽으면 이번엔 반대 방향으로 예외가 나 기동이 죽으므로, UTF-8이 아닐 때는 예전대로 코드페이지로 읽는다.
//  경로를 좁은 문자열로 주고받는 한 이 분기는 남는다 — 타입을 std::filesystem::path로 올리는 것이 정리다. [why D-071]
std::filesystem::path path_from_utf8(std::string_view text);

} // namespace utf8
