#include "utils/JsonNode.h"

namespace jsonx
{
const nlohmann::json& array_or_empty(const nlohmann::json& document, const char* key)
{
    static const nlohmann::json kEmpty = nlohmann::json::array();
    const auto iterator = document.is_object() ? document.find(key) : document.end();
    return (iterator != document.end() && iterator->is_array()) ? *iterator : kEmpty;
}

const nlohmann::json& object_or_empty(const nlohmann::json& document, const char* key)
{
    static const nlohmann::json kEmpty = nlohmann::json::object();
    const auto iterator = document.is_object() ? document.find(key) : document.end();
    return (iterator != document.end() && iterator->is_object()) ? *iterator : kEmpty;
}

} // namespace jsonx
