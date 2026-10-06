#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "hi1_store.hpp"
#include "li_core/hi1.hpp"
#include "lipf.hpp"

// The LICF half of the ADMF (TS 33.127 clause 5.3.5.2, ADR-0462 step 4): the LI lifecycle workflow
// profile of TS 103 120 Annex H.5 -- what a warrant, its tasks and its documents do to the network.
//
// Two phases, as H.5.2.2 prescribes:
//   * `handle` is the request phase: it checks the message meets the workflow endpoint's
//     requirements and the HI1 object rules, then stores the objects (all or nothing) and returns
//     the positive acknowledgement. A request that does not meet them changes nothing.
//   * `reconcile_once` is the review-and-action phase: it compares what the LEA asked for (the
//     DesiredStatus members, the timespans, the delivery details) with what the LIPF has
//     provisioned, makes them agree, records the Receiver-owned Status members, and issues a
//     NotificationObject for every change. It is idempotent and driven only by stored state, so a
//     crash between phases loses nothing: the next pass finishes the job.
namespace li_admf {

enum class Workflow {
    None, // the API base URL: only GET / LIST / GETCSPCONFIG
    NewAuthorisation,
    AuthorisationExtension,
    AuthorisationCancellation,
    TaskAddition,
    TaskCancellation,
    ChangeOfDelivery,
};

// TS 103 120 table H.0b default relative paths; nullopt for a path that is no HI1 endpoint.
std::optional<Workflow> workflow_for_path(std::string_view path);

struct LifecycleConfig {
    li_core::hi1::EndpointId self; // the CSP: owner of the NotificationObjects it issues
    // Document ContentTypes beyond H.5.2.3.4's list (pdf, docx, png, jpeg, text/plain) that this CSP
    // has agreed with its LEAs.
    std::vector<std::string> extra_document_content_types;
    std::chrono::seconds reconcile_interval{5};
    // An Error task (an NE refused or was unreachable) is retried this often.
    std::chrono::seconds retry_interval{60};
    // TS 103 221-1 6.6.2: how often an X1 Keepalive goes to every NE. Must be comfortably shorter
    // than the NEs' TIME_P2 (the AMF POI's default is 180 s), or they deactivate every task.
    std::chrono::seconds keepalive_interval{30};
    // The most records a LIST returns (6.4.8: bound what one message can dump on a requester).
    std::uint64_t maximum_list_records = 500;
};

class Lifecycle {
public:
    Lifecycle(LifecycleConfig config, Hi1Store& store, Lipf& lipf);
    ~Lifecycle();
    Lifecycle(const Lifecycle&) = delete;
    Lifecycle& operator=(const Lifecycle&) = delete;

    // The reconcile worker. start() is idempotent; stop() joins.
    void start();
    void stop();
    void wake();

    struct Outcome {
        // Set when the request as a whole does not meet the workflow endpoint (H.5.2.2.3): nothing
        // was changed and the caller answers with a top-level error.
        std::optional<li_core::hi1::Failure> rejected;
        std::vector<li_core::hi1::ActionResult> results;
    };
    // `lea` is the EndpointID the mTLS peer is onboarded as; every object is scoped to it.
    Outcome handle(Workflow workflow,
                   const li_core::hi1::EndpointId& lea,
                   const li_core::hi1::Header& header,
                   const std::vector<li_core::hi1::Action>& actions);

    // TS 103 221-1 6.6.2: send a Keepalive to every NE now. The worker does this on
    // `keepalive_interval`; public so tests and a failed first start can drive it.
    void keepalive_once();

    // What an NE reports about a task (ReportTaskIssue, 6.5.2) and about itself (ReportNEIssue,
    // 6.5.4). The error code is the X1 answer: XidDoesNotExist for a task this ADMF never provisioned.
    std::optional<li_core::x1::ErrorCode> task_issue(const li_core::x1::ReportTaskIssue& report);
    std::optional<li_core::x1::ErrorCode> ne_issue(const std::string& ne_identifier, const li_core::x1::ReportNEIssue& report);

    // One review / provisioning / expiry / notification pass. Public so tests drive it
    // deterministically; the worker calls it on its interval and on wake().
    void reconcile_once();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace li_admf
