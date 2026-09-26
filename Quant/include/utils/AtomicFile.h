#pragma once
// 파일을 통째로 바꿔 쓰기 — 임시 파일에 다 쓴 뒤 이름을 바꿔 넣어, 읽는 쪽이 반쯤 쓰인 파일을 보지 않게 한다.
//  시세판 유니버스 파일(universe/MarketBoard.cpp)과 국면 판정 파일(regime/RegimeFeed.cpp)이 같이 쓴다.
#include <string>

namespace file_io
{

// 성공하면 true. 실패하면 임시 파일을 지우고 원래 파일은 그대로 둔다.
bool write_atomically(const std::string& path, const std::string& text);

} // namespace file_io
