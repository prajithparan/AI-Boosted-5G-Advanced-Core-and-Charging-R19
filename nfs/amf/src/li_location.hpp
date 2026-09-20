#pragma once

// NGAP UserLocationInformation -> li_core::xiri::UserLocation (TS 33.128 clause 6.2.2.2.4 form 1).
//
// This is the AMF-side half of the LI Location path: the li_core::xiri Location codec is built and
// access-network-agnostic, but it deliberately knows nothing of NGAP (the whole point of the
// symbol-hiding in ADR-0364 -- li_core must not see NGAP's 83 colliding type names). The AMF is the
// one binary that hosts BOTH, so the NGAP->xiri translation lives here, next to the NGAP handlers,
// and hands li_core a plain xiri::UserLocation.
//
// Only the two 3GPP access types NGAP's UserLocationInformation carries with a cell identity are
// modelled -- NR (nR-CGI + TAI) and E-UTRA (eUTRA-CGI + TAI) -- matching the xiri codec's own
// modelled floor. userLocationInformationN3IWF and the choice extensions return nullopt (the
// caller then reports no location rather than a fabricated one), a disclosed narrowing.

#include <optional>

#include "li_core/xiri.hpp"

// The NGAP CHOICE, forward-declared so this header stays free of the NGAP generated headers (the
// .cpp includes them); a reference parameter needs only the incomplete type.
struct UserLocationInformation;

namespace amf {

// Translate an NGAP UserLocationInformation into the xiri UserLocation the Location/LocationUpdate/
// IdentifierAssociation xIRIs carry. nullopt when the CHOICE is an unmodelled branch or malformed.
std::optional<li_core::xiri::UserLocation> parse_user_location(const UserLocationInformation& uli);

} // namespace amf
