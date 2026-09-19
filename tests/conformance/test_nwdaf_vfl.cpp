// The NWDAF VFL hook's subscription store (ADR-0380): create/get/replace/merge-patch/remove,
// backing the Nnwdaf_VFLTraining and Nnwdaf_VFLInference subscription resources.

#include <nlohmann/json.hpp>

#include "vfl_subscription_store.hpp"

#include <gtest/gtest.h>

TEST(NwdafVflStore, CrudLifecycleAndMergePatch) {
    nwdaf::VflSubscriptionStore store("vflt-");

    const auto id = store.create(
        nlohmann::json{{"notificationURI", "https://mtlf/notify"}, {"vflTrainingReq", {{"a", 1}}}});
    EXPECT_EQ(id.rfind("vflt-", 0), 0u); // id carries the family prefix

    auto got = store.get(id);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ((*got)["notificationURI"], "https://mtlf/notify");
    EXPECT_FALSE(store.get("missing").has_value());

    // Replace (PUT).
    EXPECT_TRUE(store.replace(id, nlohmann::json{{"notificationURI", "https://mtlf/other"}}));
    EXPECT_FALSE(store.replace("missing", nlohmann::json::object()));
    EXPECT_EQ((*store.get(id))["notificationURI"], "https://mtlf/other");

    // RFC 7386 merge patch (PATCH): add a field, keep the rest.
    const auto merged = store.merge_patch(id, nlohmann::json{{"extra", 42}});
    ASSERT_TRUE(merged.has_value());
    EXPECT_EQ((*merged)["extra"], 42);
    EXPECT_EQ((*merged)["notificationURI"], "https://mtlf/other");
    EXPECT_FALSE(store.merge_patch("missing", nlohmann::json::object()).has_value());

    // Delete (DELETE) is idempotent-ish: second remove is false.
    EXPECT_TRUE(store.remove(id));
    EXPECT_FALSE(store.remove(id));
    EXPECT_FALSE(store.get(id).has_value());
}
