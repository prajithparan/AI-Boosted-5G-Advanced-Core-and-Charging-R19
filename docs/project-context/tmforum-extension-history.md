# TM Forum API extension history (SID entity -> TMF API list)

Moved verbatim from CLAUDE.md (section "Charging domain: 3GPP + TM Forum SID", parenthetical of the SID-aligned bullet); see `docs/optimization/MOVE_LOG.md`.

(Extended 2026-08-10 from the original 620/622/
  632/635/637/666/676/678/727: `docs/CHARGING_MAPPING.md`'s research found
  Service, Resource, Agreement, and BalanceTopUp each have a real
  TM-Forum-designated home API that wasn't in the original list -- TMF633/638
  (Service Catalog/Inventory, mirroring the existing Product Catalog=620/
  Inventory=637 split), TMF639 (Resource Inventory), TMF651 (Agreement),
  TMF654 (Prepay Balance) -- added rather than leaving those 4 SID entities
  without a home API. Also note: TMF727 is Service Usage Management, not
  Product Offering Qualification as an earlier reference implied -- that's
  TMF679, not currently in scope. Extended again same day: `Event` (named in
  PROMPT.md's fuller SID list but dropped from this file's condensed one) has
  a real home, TMF688 Event Management -- added, asked and approved, same as
  the first four. `CustomerOrder` and `Policy` remain named in PROMPT.md but
  not here; `Policy`'s real TMF723 API is confirmed by name/number but no
  public source for its field list was found (flagged, not resolved) --
  neither added without being asked first.)
