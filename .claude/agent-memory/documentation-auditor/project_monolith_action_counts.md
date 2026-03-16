---
name: monolith_action_counts
description: Current verified Monolith action counts per namespace — use to catch stale totals in CLAUDE.md and MEMORY.md
type: project
---

As of 2026-03-15 (C++ source indexer + trigger_project_reindex added):

| Namespace | Count |
|-----------|-------|
| core | 4 |
| blueprint | 47 |
| material | 25 |
| animation | 62 |
| niagara | 47 |
| editor | 13 |
| config | 6 |
| source | 11 |
| project | 5 |
| **Total** | **220** |

Changes from prior snapshot (219):
- source: 10 → 11 (added `trigger_project_reindex` — incremental project-only C++ re-index)
- Python tree-sitter indexer (`Scripts/source_indexer/`) is now legacy; MonolithSource runs a native C++ indexer
- `UMonolithQueryCommandlet` (-run=MonolithQuery) replaces `monolith_offline.py` as preferred offline CLI

**Why:** Action counts drift frequently. CLAUDE.md and MEMORY.md both embed these numbers. Keeping a verified snapshot here means future audits can check without calling monolith_discover().

**How to apply:** When auditing CLAUDE.md or MEMORY.md, compare stated totals against this table. If numbers diverge, call `monolith_discover()` to get the live count and update this file too.
