#include "TestHarness.h"

#include "Json.h"

using nlohmann::json;
using nlohmann::ordered_json;

TEST(JsonGet_MissingKeyGivesFallback) {
    json j = {{"a", 1}};
    CHECK_EQ(JsonGet(j, "b", 7), 7);
    CHECK_EQ(JsonGet(j, "b", "x"), std::string("x"));
    CHECK_EQ(JsonGet(j, "b", true), true);
    CHECK_EQ(JsonGet(j, "b", 1.5), 1.5);
}

TEST(JsonGet_ReturnsValueOfRightType) {
    json j = {{"i", 42}, {"s", "hi"}, {"b", true}, {"d", 2.5}, {"neg", -3}};
    CHECK_EQ(JsonGet(j, "i", 0), 42);
    CHECK_EQ(JsonGet(j, "neg", 0), -3);
    CHECK_EQ(JsonGet(j, "s", ""), std::string("hi"));
    CHECK_EQ(JsonGet(j, "b", false), true);
    CHECK_EQ(JsonGet(j, "d", 0.0), 2.5);
    CHECK_EQ(JsonGet(j, "i", 0.0), 42.0);  // integers read as double
}

TEST(JsonGet_WrongTypesGiveFallbackAndNeverThrow) {
    json j = {{"null", nullptr}, {"str", "14"}, {"arr", json::array({1, 2})}, {"obj", json::object()},
              {"num", 5}, {"bool", true}};
    for (const char* key : {"null", "str", "arr", "obj", "bool"}) CHECK_NOTHROW(JsonGet(j, key, 9));
    CHECK_EQ(JsonGet(j, "bool", 9), 9);
    CHECK_EQ(JsonGet(j, "str", 9), 9);         // string for int
    CHECK_EQ(JsonGet(j, "null", 9), 9);
    CHECK_EQ(JsonGet(j, "arr", 9), 9);
    CHECK_EQ(JsonGet(j, "obj", 9), 9);
    CHECK_EQ(JsonGet(j, "num", "fb"), std::string("fb"));   // int for string
    CHECK_EQ(JsonGet(j, "num", false), false);              // int for bool
    CHECK_EQ(JsonGet(j, "num", true), true);
    CHECK_EQ(JsonGet(j, "str", 1.5), 1.5);                  // string for double
    CHECK_EQ(JsonGet(j, "null", "fb"), std::string("fb"));
    CHECK_EQ(JsonGet(j, "arr", "fb"), std::string("fb"));
    CHECK_EQ(JsonGet(j, "obj", true), true);
}

TEST(JsonGet_NonObjectDocumentsGiveFallback) {
    for (const json& doc : {json(nullptr), json(5), json("s"), json::array({1, 2}), json(true), json(1.5)}) {
        CHECK_EQ(JsonGet(doc, "a", 3), 3);
        CHECK_EQ(JsonGet(doc, "a", "fb"), std::string("fb"));
        CHECK_EQ(JsonGet(doc, "a", true), true);
    }
    // A discarded (unparseable) document is not an object either.
    json bad = json::parse("{oops", nullptr, false);
    CHECK_EQ(JsonGet(bad, "a", 3), 3);
}

TEST(JsonGet_FloatsBecomeIntegers) {
    json j = {{"a", 14.0}, {"b", 14.9}, {"c", -2.5}};
    CHECK_EQ(JsonGet(j, "a", 0), 14);
    CHECK_EQ(JsonGet(j, "b", 0), 14);  // truncates
    CHECK_EQ(JsonGet(j, "c", 0), -2);
}

TEST(JsonGet_HugeNumbersDoNotThrow) {
    json j = json::parse(R"({"big": 1e300, "u64": 18446744073709551615, "i64": 9223372036854775807})");
    CHECK_NOTHROW(JsonGet(j, "u64", 0));
    CHECK_NOTHROW(JsonGet(j, "i64", 0));
    CHECK_NOTHROW(JsonGet(j, "big", 0.0));
    CHECK_EQ(JsonGet(j, "big", 0.0), 1e300);
    CHECK_EQ(JsonGet(j, "big", std::string("s")), std::string("s"));
}

TEST(JsonGet_WorksWithOrderedJson) {
    ordered_json j = {{"x", 3}, {"s", "t"}};
    CHECK_EQ(JsonGet(j, "x", 0), 3);
    CHECK_EQ(JsonGet(j, "s", ""), std::string("t"));
    CHECK_EQ(JsonGet(j, "x", ""), std::string(""));
}
