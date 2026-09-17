#pragma once
// json 하위 노드를 복사 없이 집어 온다. `j.value("k", json::array())`는 키가 있어도 기본값을 만들려고
//  노드 전체를 베끼므로, 원소가 수천이면 수천을 베낀다. 여기 둘은 참조만 돌려주고 없으면 빈 노드를 가리킨다.
//  규약 정본은 docs/guides/MAINTENANCE_AUTOMATION.md 4절 "복사 표기".
#include <nlohmann/json.hpp>

namespace jsonx
{
// [inv] 반환 참조는 document의 수명 안, document에 삽입이 없는 구간에서만 유효하다. 빈 노드는 정적이라 언제나 산다.
inline const nlohmann::json& array_or_empty(const nlohmann::json& document, const char* key)
{
    static const nlohmann::json kEmpty = nlohmann::json::array();
    const auto                  iterator = document.is_object() ? document.find(key) : document.end();
    return (iterator != document.end() && iterator->is_array()) ? *iterator : kEmpty;
}

// [inv] array_or_empty와 같다.
inline const nlohmann::json& object_or_empty(const nlohmann::json& document, const char* key)
{
    static const nlohmann::json kEmpty = nlohmann::json::object();
    const auto                  iterator = document.is_object() ? document.find(key) : document.end();
    return (iterator != document.end() && iterator->is_object()) ? *iterator : kEmpty;
}
}   // namespace jsonx
