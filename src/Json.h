#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <type_traits>

// Type-checked reads from JSON that came from disk or the network. Unlike
// json::value(), which throws when a key has an unexpected type, these never
// throw: a non-object document, a missing key or a value of the wrong type
// all give the fallback. Works with json and ordered_json.
//   JsonGet<std::string>(j, "title", "")   JsonGet(j, "keep_audio_days", 14)
template <typename T, typename Json>
T JsonGet(const Json& j, const char* key, T fallback) {
    if (!j.is_object()) return fallback;
    auto it = j.find(key);
    if (it == j.end()) return fallback;
    const Json& v = *it;
    if constexpr (std::is_same_v<T, bool>) {
        return v.is_boolean() ? v.template get<bool>() : fallback;
    } else if constexpr (std::is_integral_v<T>) {
        if (v.is_number_integer()) return v.template get<T>();
        if (v.is_number_float()) return static_cast<T>(v.template get<double>());
        return fallback;
    } else if constexpr (std::is_floating_point_v<T>) {
        return v.is_number() ? v.template get<T>() : fallback;
    } else if constexpr (std::is_same_v<T, std::string>) {
        return v.is_string() ? v.template get<std::string>() : fallback;
    } else {
        static_assert(sizeof(T) == 0, "JsonGet supports bool, integers, floating point and std::string");
    }
}

template <typename Json>
std::string JsonGet(const Json& j, const char* key, const char* fallback) {
    return JsonGet<std::string>(j, key, std::string(fallback));
}
