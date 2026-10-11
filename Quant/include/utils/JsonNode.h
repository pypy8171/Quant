#pragma once
// json 하위 노드를 복사 없이 집어 온다. `j.value("k", json::array())`는 키가 있어도 기본값을 만들려고
//  노드 전체를 베끼므로, 원소가 수천이면 수천을 베낀다. 여기 둘은 참조만 돌려주고 없으면 빈 노드를 가리킨다.
//  규약 정본은 docs/guides/CODE_CONVENTIONS.md 6.3(안 해도 되는 복사).
#include <nlohmann/json.hpp>

namespace jsonx
{
// [inv] 반환 참조는 document의 수명 안, document에 삽입이 없는 구간에서만 유효하다. 빈 노드는 정적이라 언제나 산다.
const nlohmann::json& array_or_empty(const nlohmann::json& document, const char* key);

// [inv] array_or_empty와 같다.
const nlohmann::json& object_or_empty(const nlohmann::json& document, const char* key);
}   // namespace jsonx
