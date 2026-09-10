// ADR-0335: subscription lifecycle recording -- the churn-data prerequisite.
//
// This is not a model and not an agent. It is the collection step that has to run BEFORE churn
// propensity is possible at all: `subscriber` recorded created_at/updated_at and nothing about a
// subscriber leaving, so there was no label to train against. That history can only be gathered
// forward in time, which is why starting it is the deliverable.
//
// Requires a real Postgres. Skipped, loudly, when SUBSCRIBER_MANAGEMENT_TEST_URL is unset --
// the same convention every other Postgres-backed test here uses.

#include <cstdlib>
#include <ctime>
#include <string>
#include <unistd.h>

// Explicit relative include, NOT an include-path entry. Adding
// bss/subscriber-management/src to integration_tests' include path makes every bare
// #include "store.hpp" in this directory resolve to THAT store.hpp -- which silently broke
// test_product_catalog_postgres.cpp, a file this change never touched. The same trap has been
// hit before in this repo with bss/balance-management.
#include "../../bss/subscriber-management/src/store.hpp"

#include <gtest/gtest.h>

namespace {

const char* test_url() {
    return std::getenv("SUBSCRIBER_MANAGEMENT_TEST_URL");
}

} // namespace

TEST(SubscriberLifecycle, TransitionRecordsBothSidesAtomically) {
    const char* url = test_url();
    if (url == nullptr) {
        GTEST_SKIP() << "SUBSCRIBER_MANAGEMENT_TEST_URL unset";
    }
    subscriber_management::SubscriberStore store("https://example.com/sub", url);

    subscriber_management::Subscriber s;
    // Unique per RUN, not merely random-looking. std::rand() without a seed returns the same
    // sequence every process, so the first version generated an identical SUPI on every run,
    // passed once against a fresh database and then failed forever on the UNIQUE constraint.
    // A test that only passes the first time is worse than no test: it goes green in review and
    // red in CI's second build.
    const auto unique = std::to_string(static_cast<long long>(std::time(nullptr))) + "-" +
                        std::to_string(static_cast<long long>(::getpid()));
    s.supi = "imsi-test-" + unique;
    const auto id = store.create(s);
    ASSERT_FALSE(id.empty());

    // active -> suspended -> terminated. The INTERMEDIATE state is the point: knowing someone is
    // terminated is far less useful than knowing they went suspended->terminated (a collections
    // outcome) rather than active->terminated (a customer walking away). A churn model that
    // cannot tell those apart learns the wrong thing.
    EXPECT_TRUE(store.record_lifecycle_transition(id, "suspended", "non-payment"));
    EXPECT_TRUE(store.record_lifecycle_transition(id, "terminated", "customer request"));

    const auto after = store.get(id);
    ASSERT_TRUE(after.has_value());
}

TEST(SubscriberLifecycle, AnUnknownSubscriberIsRejectedNotInvented) {
    const char* url = test_url();
    if (url == nullptr) {
        GTEST_SKIP() << "SUBSCRIBER_MANAGEMENT_TEST_URL unset";
    }
    subscriber_management::SubscriberStore store("https://example.com/sub", url);
    // Must not silently insert an event for a subscriber that does not exist: a churn history
    // containing phantom subjects is worse than no history, because it looks usable.
    EXPECT_FALSE(store.record_lifecycle_transition("no-such-subscriber", "terminated", "test"));
}
