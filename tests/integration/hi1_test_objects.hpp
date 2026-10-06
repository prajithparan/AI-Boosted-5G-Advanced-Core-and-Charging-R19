#pragma once

// HI1 object builders for the ADMF tests: an Authorisation, LITask and Document the way an LEA
// would send them (TS 103 120 clauses 7.2, 8.2, 7.3), with the options the tests vary.

#include <cstdio>
#include <ctime>
#include <string>

#include "li_core/hi1.hpp"

#include <gtest/gtest.h>

namespace nf_test::hi1obj {

namespace hi1 = li_core::hi1;

constexpr const char* kNs =
    R"(xmlns="http://uri.etsi.org/03120/common/2019/10/Core" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" )"
    R"(xmlns:common="http://uri.etsi.org/03120/common/2016/02/Common" xmlns:auth="http://uri.etsi.org/03120/common/2020/09/Authorisation" )"
    R"(xmlns:task="http://uri.etsi.org/03120/common/2020/09/Task" xmlns:doc="http://uri.etsi.org/03120/common/2020/09/Document" )"
    R"(xmlns:etsi="http://uri.etsi.org/03280/common/2017/07")";

inline std::string ts(long offset_seconds) {
    const std::time_t t = std::time(nullptr) + offset_seconds;
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

inline std::string uuid(int n) {
    char buf[40];
    std::snprintf(buf,
                  sizeof(buf),
                  "%08x-0000-4000-8000-%012x",
                  static_cast<unsigned>(0xA0000000U + static_cast<unsigned>(n)),
                  static_cast<unsigned>(n));
    return buf;
}

inline hi1::Object parse(const std::string& xml) {
    auto o = hi1::Object::from_xml(xml);
    EXPECT_TRUE(o.has_value()) << (o ? "" : o.error()) << "\n" << xml;
    return o ? *o : hi1::Object{};
}

struct AuthOpts {
    std::string end = ts(86400);
    std::string start;   // empty: no StartTime
    std::string desired; // AuthorisationDesiredStatus value, "" = absent
    std::string status;  // a (forbidden) AuthorisationStatus, for the negative test
    std::string generation;
};

inline hi1::Object authorisation(const std::string& id, const AuthOpts& o = {}) {
    std::string x = std::string("<HI1Object ") + kNs +
                    R"( xsi:type="auth:AuthorisationObject"><ObjectIdentifier>)" + id +
                    "</ObjectIdentifier>";
    if (!o.generation.empty()) {
        x += "<Generation>" + o.generation + "</Generation>";
    }
    x += "<auth:AuthorisationReference>W-" + id.substr(0, 8) + "</auth:AuthorisationReference>";
    if (!o.status.empty()) {
        x += "<auth:AuthorisationStatus><common:Owner>ETSI</"
             "common:Owner><common:Name>AuthorisationStatus</common:Name><common:Value>" +
             o.status + "</common:Value></auth:AuthorisationStatus>";
    }
    if (!o.desired.empty()) {
        x += "<auth:AuthorisationDesiredStatus><common:Owner>ETSI</"
             "common:Owner><common:Name>AuthorisationDesiredStatus</common:Name><common:Value>" +
             o.desired + "</common:Value></auth:AuthorisationDesiredStatus>";
    }
    x += "<auth:AuthorisationTimespan>";
    if (!o.start.empty()) {
        x += "<auth:StartTime>" + o.start + "</auth:StartTime>";
    }
    x += "<auth:EndTime>" + o.end + "</auth:EndTime></auth:AuthorisationTimespan></HI1Object>";
    return parse(x);
}

// An update that names only the Authorisation's timespan end.
inline hi1::Object
authorisation_end_update(const std::string& id, const std::string& end, bool with_start = false) {
    std::string x = std::string("<HI1Object ") + kNs +
                    R"( xsi:type="auth:AuthorisationObject"><ObjectIdentifier>)" + id +
                    "</ObjectIdentifier><auth:AuthorisationTimespan>";
    if (with_start) {
        x += "<auth:StartTime>" + ts(-10) + "</auth:StartTime>";
    }
    x += "<auth:EndTime>" + end + "</auth:EndTime></auth:AuthorisationTimespan></HI1Object>";
    return parse(x);
}

inline hi1::Object authorisation_desired(const std::string& id, const std::string& desired) {
    return parse(
        std::string("<HI1Object ") + kNs +
        R"( xsi:type="auth:AuthorisationObject"><ObjectIdentifier>)" + id +
        "</ObjectIdentifier><auth:AuthorisationDesiredStatus><common:Owner>ETSI</"
        "common:Owner><common:Name>AuthorisationDesiredStatus</common:Name><common:Value>" +
        desired + "</common:Value></auth:AuthorisationDesiredStatus></HI1Object>");
}

struct TaskOpts {
    std::string imsi = "999700000000001";
    std::string format = "SUPIIMSI";
    std::string address = "192.0.2.10:9443";
    std::string delivery = "IRIOnly";
    std::string start;
    std::string end;
    std::string desired;
};

inline hi1::Object task(const std::string& id,
                        const std::string& auth_id,
                        const std::string& liid,
                        const TaskOpts& o = {}) {
    std::string x =
        std::string("<HI1Object ") + kNs + R"( xsi:type="task:LITaskObject"><ObjectIdentifier>)" +
        id + "</ObjectIdentifier><AssociatedObjects><AssociatedObject>" + auth_id +
        "</AssociatedObject></AssociatedObjects><task:Reference>" + liid + "</task:Reference>";
    if (!o.desired.empty()) {
        x += "<task:DesiredStatus><common:Owner>ETSI</common:Owner><common:Name>TaskDesiredStatus</"
             "common:Name><common:Value>" +
             o.desired + "</common:Value></task:DesiredStatus>";
    }
    if (!o.start.empty() || !o.end.empty()) {
        x += "<task:Timespan>";
        if (!o.start.empty()) {
            x += "<task:StartTime>" + o.start + "</task:StartTime>";
        }
        if (!o.end.empty()) {
            x += "<task:EndTime>" + o.end + "</task:EndTime>";
        }
        x += "</task:Timespan>";
    }
    x += "<task:TargetIdentifier><task:TargetIdentifierValues><task:TargetIdentifierValue><task:"
         "FormatType><task:FormatOwner>ETSI</task:FormatOwner>"
         "<task:FormatName>" +
         o.format + "</task:FormatName></task:FormatType><task:Value>" + o.imsi +
         "</task:Value></task:TargetIdentifierValue></task:TargetIdentifierValues></"
         "task:TargetIdentifier>"
         "<task:DeliveryType><common:Owner>ETSI</common:Owner><common:Name>TaskDeliveryType</"
         "common:Name><common:Value>" +
         o.delivery + "</common:Value></task:DeliveryType>";
    const auto colon = o.address.rfind(':');
    x += "<task:DeliveryDetails><task:DeliveryDestination><task:DeliveryAddress><task:"
         "IPAddressPort><etsi:address><etsi:IPv4Address>" +
         o.address.substr(0, colon) +
         "</etsi:IPv4Address></etsi:address><etsi:port><etsi:TCPPort>" +
         o.address.substr(colon + 1) +
         "</etsi:TCPPort></etsi:port></task:IPAddressPort></task:DeliveryAddress></"
         "task:DeliveryDestination></task:DeliveryDetails>"
         "</HI1Object>";
    return parse(x);
}

inline hi1::Object task_update(const std::string& id, const std::string& inner) {
    return parse(std::string("<HI1Object ") + kNs +
                 R"( xsi:type="task:LITaskObject"><ObjectIdentifier>)" + id +
                 "</ObjectIdentifier>" + inner + "</HI1Object>");
}

inline std::string task_desired_xml(const std::string& value) {
    return "<task:DesiredStatus><common:Owner>ETSI</common:Owner><common:Name>TaskDesiredStatus</"
           "common:Name><common:Value>" +
           value + "</common:Value></task:DesiredStatus>";
}

inline hi1::Object document(const std::string& id,
                            const std::string& associated,
                            const std::string& content_type = "") {
    std::string x =
        std::string("<HI1Object ") + kNs + R"( xsi:type="doc:DocumentObject"><ObjectIdentifier>)" +
        id + "</ObjectIdentifier><AssociatedObjects><AssociatedObject>" + associated +
        "</AssociatedObject></AssociatedObjects><doc:DocumentName>warrant.pdf</doc:DocumentName>";
    if (!content_type.empty()) {
        x += "<doc:DocumentBody><doc:Contents>JVBERi0=</doc:Contents><doc:ContentType>" +
             content_type + "</doc:ContentType></doc:DocumentBody>";
    }
    x += "</HI1Object>";
    return parse(x);
}

inline hi1::Action create(std::uint64_t id, hi1::Object o) {
    return {id, hi1::CreateAction{std::move(o)}};
}
inline hi1::Action update(std::uint64_t id, hi1::Object o) {
    return {id, hi1::UpdateAction{std::move(o)}};
}

} // namespace nf_test::hi1obj
