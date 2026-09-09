#!/usr/bin/env python3
"""Every 3GPP API root a server registers must equal its YAML's servers[0].url.

ADR-0325. Ten roots in NEF were wrong for as long as they existed and 580 tests passed over
them, because a test that builds its request URL from the same constant the server registers
is self-consistent by construction: it cannot see that the constant disagrees with the spec.
The only thing that can see it is a comparison against the YAML, which is what this does.

The roots were wrong in a specific way -- derived from the spec's *filename*
(TS29522_CagInfoParamProvision -> "/3gpp-cag-info-provision/v1") rather than read from its
servers[0].url ("/3gpp-caginfo-pp/v1"). 3GPP does not derive one from the other, and the
guesses were plausible enough to survive review.

Usage: validate_api_roots.py <repo-root>
"""
import re
import sys
from pathlib import Path

import yaml

# Roots that are legitimately not 3GPP-declared. TM Forum Open API roots come from the TMF
# specifications, which are not in specs/; the /bss-api/ root is this project's own and has no
# external spec. Anything starting "/3gpp-" or "/n<nf>-" must be in a YAML -- entries may not
# be added here to silence a mismatch on one of those.
NON_3GPP_ROOTS = {
    "/tmf-api/prepayBalanceManagement/v4",
    "/tmf-api/productCatalogManagement/v4",
    "/tmf-api/agreementManagement/v4",
    "/tmf-api/party/v4",
    "/bss-api/subscriberManagement/v1",
}

ROOT_CONST = re.compile(r'constexpr const char\* (k\w*(?:ApiRoot|Root)) = "([^"]+)"')


def spec_roots(specs_dir: Path) -> dict[str, list[str]]:
    """Concrete API roots declared by the R19 YAML, mapped to the files declaring them.

    A spec whose only server is a bare "{apiRoot}" (TS29122_MsisdnLessMoSms,
    TS29522_NIDDConfigurationTrigger) declares no fixed root -- it is invoked at a URI supplied
    at runtime -- so it contributes nothing to match against and is skipped rather than
    recorded as an empty root.
    """
    roots: dict[str, list[str]] = {}
    for path in sorted(specs_dir.glob("*.yaml")):
        try:
            doc = yaml.safe_load(path.read_text())
        except yaml.YAMLError:
            continue
        if not isinstance(doc, dict) or not doc.get("paths") or not doc.get("servers"):
            continue
        url = doc["servers"][0].get("url", "").replace("{apiRoot}", "")
        if url:
            roots.setdefault(url, []).append(path.stem)
    return roots


def main() -> int:
    repo = Path(sys.argv[1]).resolve()
    specs = repo / "specs" / "5G_APIs-REL-19"
    if not specs.is_dir():
        print(f"FAIL: spec directory not found: {specs}")
        return 1

    declared = spec_roots(specs)
    if not declared:
        print(f"FAIL: no API roots parsed out of {specs}; the check would pass vacuously")
        return 1

    sources = sorted(
        p
        for pattern in ("nfs/*/src/*.cpp", "nfs/*/src/*.hpp", "bss/*/src/*.cpp")
        for p in repo.glob(pattern)
    )
    checked = 0
    failures = []
    for source in sources:
        for name, value in ROOT_CONST.findall(source.read_text()):
            checked += 1
            if value in declared or value in NON_3GPP_ROOTS:
                continue
            rel = source.relative_to(repo)
            failures.append(f"  {rel}: {name} = \"{value}\" matches no servers[0].url")

    if not checked:
        print("FAIL: no API root constants found; the check would pass vacuously")
        return 1

    print(f"checked {checked} API root constants against {len(declared)} spec-declared roots")
    if failures:
        print(f"FAIL: {len(failures)} root(s) not declared by any R19 YAML:")
        print("\n".join(failures))
        print("\nRead servers[0].url from the spec. Do not derive the root from the filename.")
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
