---
name: Monolith action counts
description: Current Monolith action counts per module (349 total as of 2026-03-25). Use when writing or updating any docs that reference action counts.
type: project
---

As of 2026-03-25, Monolith has **349 total actions** across 10 modules (13 MCP tools):

| Module | Namespace | Actions |
|--------|-----------|---------|
| MonolithCore | monolith | 4 |
| MonolithBlueprint | blueprint | 66 |
| MonolithMaterial | material | 57 |
| MonolithAnimation | animation | 74 |
| MonolithNiagara | niagara | 65 |
| MonolithEditor | editor | 19 |
| MonolithConfig | config | 6 |
| MonolithIndex | project | 5 |
| MonolithSource | source | 11 |
| MonolithUI | ui | 42 |

Material was upgraded from 48 to 57 actions on 2026-03-25 (Material Function Full Suite: 9 new actions + 1 extended).

**Why:** Action counts appear in CLAUDE.md, SPEC.md (multiple locations), skill files, and agent definitions. Keeping them in sync is critical.

**How to apply:** When any doc update touches action counts, verify against this table. Update this memory when new actions are added.
