// ADR-0476: JSON bodies that repeat a member name are rejected before any handler runs. This file
// tests the detector only (no ports). The server-level 400 (hook in http2_server.cpp) has NO test
// yet: it needs a listening server, so it waits for an idle runner (recorded in ADR-0476).
#include "sbi_core/json_body.hpp"

#include <gtest/gtest.h>

using sbi_core::http2::json_has_duplicate_keys;

TEST(SbiDuplicateKeys, TopLevelRepeat) {
    EXPECT_TRUE(json_has_duplicate_keys(R"({"supi":"imsi-1","supi":"imsi-2"})"));
}

TEST(SbiDuplicateKeys, NestedRepeat) {
    EXPECT_TRUE(json_has_duplicate_keys(R"({"a":{"b":1,"b":2}})"));
    EXPECT_TRUE(json_has_duplicate_keys(R"({"list":[{"x":1},{"y":1,"y":2}]})"));
}

TEST(SbiDuplicateKeys, SameNameInDifferentObjectsIsFine) {
    EXPECT_FALSE(json_has_duplicate_keys(R"({"a":{"id":1},"b":{"id":2}})"));
    EXPECT_FALSE(json_has_duplicate_keys(R"([{"id":1},{"id":2},{"id":3}])"));
    EXPECT_FALSE(json_has_duplicate_keys(R"({"a":[{"k":1}],"b":[{"k":2}]})"));
}

TEST(SbiDuplicateKeys, EscapedSpellingOfTheSameNameIsADuplicate) {
    EXPECT_TRUE(json_has_duplicate_keys(R"({"a":1,"a":2})"));
}

TEST(SbiDuplicateKeys, ValuesAndScalarsAreNotKeys) {
    EXPECT_FALSE(json_has_duplicate_keys(R"({"a":"a","b":"a","c":null,"d":true,"e":1.5})"));
    EXPECT_FALSE(json_has_duplicate_keys("42"));
    EXPECT_FALSE(json_has_duplicate_keys("[]"));
    EXPECT_FALSE(json_has_duplicate_keys("{}"));
}

TEST(SbiDuplicateKeys, MalformedJsonIsLeftToTheHandler) {
    EXPECT_FALSE(json_has_duplicate_keys(R"({"a":1,"b")"));
    EXPECT_FALSE(json_has_duplicate_keys("not json"));
    EXPECT_FALSE(json_has_duplicate_keys(""));
}
