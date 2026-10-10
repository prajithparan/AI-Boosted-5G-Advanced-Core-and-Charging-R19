## ADR-0336: the training threshold was low enough to be cleared by nothing

**Date:** 2026-09-11. **Status:** Closed (work pushed; last citing commit e1e11c3 on origin/main, 2026-10-06). accepted.

`train_quota_sizing.py` refused to train on real data below `MIN_REAL_EXAMPLES = 20`. Measuring the
lab's actual Doris contents showed why that number was dangerous:

| | |
|---|---|
| CDR rows | 63 |
| Distinct subscribers | 16 |
| Usage-bearing rows | 24 |
| **Usable training pairs** | **12** |

24 usage rows yield only 12 examples, because each needs a *prior* usage-bearing row for the same
`(subscriber, ratingGroup)`. But the count was close enough to 20 that a little more lab traffic
would have crossed it — and the next run would have trained on ~20 points spread across ~16
subscribers and tagged the artifact **`data_source=real_cdr`**.

That label is the problem. It is technically true and materially misleading: a model fitted to
twenty points generalises to nothing and would very likely be **worse** than the documented
synthetic bootstrap it replaced, while carrying the one label a reader would trust.

**A threshold's job is not to be cleared.** It is to refuse until the data means something, and 20
made "real" the easier label to earn than the honest one.

### Two gates now, not one

`MIN_REAL_EXAMPLES = 2000` **and** `MIN_REAL_SUBSCRIBERS = 200`. Both, because row count alone
cannot distinguish a subscriber base from one very chatty test SIM: 2000 rows from 5 subscribers is
a model of five people. The distinct-subscriber count is computed in `fetch_real_examples` and
travels with the data rather than being inferred later.

The refusal message now prints both counts and both bars, and says *why* it is refusing rather than
only that it did — the operator reading it needs to know how far off they are.

Verified by running it: *"real data is too thin to train on: 12 examples (need >= 2000) from 12
subscribers (need >= 200)"*, falling back to the clearly-labelled synthetic bootstrap.

### What actually unblocks this

Volume, not a better model and not an open-source one — no pretrained model exists for *this*
network's subscribers and tariffs, and none could transfer. Phase 8's traffic generator is the
thing that produces real CDRs at scale, and it is the next increment.

