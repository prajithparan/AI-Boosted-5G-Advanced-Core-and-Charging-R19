# Architecture diagram — conventions

The diagram and the product/license table live in `README.md`, because that is what a reader sees
first. This file is the rulebook for keeping them true.

## Legend

- **Solid box**: exists in this repository *and* has a test or a live run behind it.
- **Dashed box** (`:::planned`): in scope per `CLAUDE.md`, not built. A box moves from dashed to
  solid in the same commit that lands the NF's first tested service — never before.
- **Edge label**: the real reference point or protocol (N4, N28, N40, Gy, M3UA). No edge is
  drawn for a call that does not happen in code.

## When it must be updated

Part of every NF's definition of done (`CLAUDE.md`). Concretely, the diagram or table changes
when any of these does:

1. an NF or BSS component is added, or first gets a passing integration test;
2. a datastore, broker, or runtime product is added, removed, or **changes version across a
   license boundary** (Redis 7.2 → 7.4 was exactly that, and the table exists so it is seen);
3. a reference point is wired for the first time;
4. something on the dashed list is built.

## Why Mermaid, not a rendered image

It is text: it diffs with the code, and it changes in the same commit as the change it describes.
GitHub renders it natively and follows the viewer's light/dark theme, like the SVG banners above
it. A generated image needs a render step, and "update after each phase" does not survive a
render step someone has to remember.

## Why the license column is there

P1 is strict OSI-only. The column is the audit. Two products that would have passed a glance did
not pass the table: Redis 7.4 (RSALv2/SSPL — replaced by Valkey, ADR-0044/0356) and tl-expected
(CC0-1.0, which OSI declined to list — flagged, not hidden). A product's license is taken from
its port metadata or its own LICENSE file; nothing in the table is recalled.
