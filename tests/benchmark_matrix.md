# SW0 Benchmark Matrix — the standing acceptance test

**Purpose:** "works on ALL sites" is measured, not vibes. 20 real sites across 10 classes.
Every SW-track stage (SW1–SW8) must raise this matrix's pass rate to ship. Progress is
reported as **N/20** — never as a site-specific claim.

**Method (automated):** `PLUTO_FIELDTEST_AUTOTEST` sim-only seam (main.c) reads
`tests/fieldtest_urls.txt` at boot, forces Full JS mode (`jsEnabled=2`), navigates each site
(15s each, one retry on a transient network-error page), then scans the ENTIRE rendered
document — blocks **and** table cells — case-insensitively for the site's criterion keyword.
No criterion = PASS if any text renders (for classes whose content changes every visit).

**Scoring line** (pluto.log):
`[fieldtest] done (N sites): PASS p / FAIL f — matrix p/N`

## The matrix (20 sites / 10 classes)

| Class | Site | Criterion | Why this class matters |
|---|---|---|---|
| Search (lite) | lite.duckduckgo.com/lite | `duckduckgo` | Form + results table rendering |
| Search (results) | html.duckduckgo.com/html/?q=playdate | `playdate` | Server-rendered result list |
| News (text-first) | text.npr.org | `npr` | Headline list, minimal markup |
| News (heavy) | reuters.com | `reuters` | Heavy commercial news page |
| Wiki (article) | en.wikipedia.org/wiki/Playdate_(console) | `playdate` | Big structured articles |
| Wiki (random) | en.wikipedia.org/wiki/Special:Random | `wikipedia` | Arbitrary-page robustness |
| Forum (table UI) | news.ycombinator.com | `hacker news` | Nested tables (the fixed bug) |
| Forum (link list) | lobste.rs | `lobsters` | Community site, moderate markup |
| Search (big-tech) | bing.com/search?q=playdate | `bing` | Heavy search page |
| Search (alt) | search.marginalia.nu | `marginalia` | Independent engine, simple HTML |
| Minimal (example) | example.com | `example domain` | Baseline sanity |
| Minimal (classic) | motherfuckingwebsite.com | `motherfucking website` | Pure HTML/CSS |
| Personal (SPA-lite) | bryanwandrych.com | `bryan` | Client-side-rendered personal site |
| Blog (aggregator) | daringfireball.net | `daring fireball` | Linked-list blog |
| Social (old UI) | old.reddit.com | `reddit` | Table-era social markup |
| Forum (text board) | textboard.org | `textboard` | Minimal community |
| Books (catalog) | gutenberg.org/browse/scores/top | `gutenberg` | Large static catalog tables |
| Standards (docs) | w3.org | `w3c` | Spec-style documentation |
| News (lite mirror) | lite.cnn.com | `cnn` | Text-only mirror of a heavy site |
| News (major) | bbc.com/news | `bbc` | Heavy modern news front page |

## Rules

1. **No site-specific tuning, ever.** Fixes must be general (an API, a cap, a bug class).
2. A stage ships only if the matrix pass rate does not regress and ideally rises.
3. Device verification per AGENTS.md remains mandatory for any shipped stage — the matrix
   runs in the simulator (scoring harness is sim-only); hardware spot-checks confirm.
4. The matrix list can be revised only with a recorded reason (site died, moved, etc.),
   keeping the 10-class coverage intact.

## Baseline (2026-09-20, before any SW stage): **14/20**

PASS (14): DDG lite, DDG html results, text.npr.org, Wikipedia article, Wikipedia random,
Hacker News, lobste.rs, Marginalia, example.com, motherfuckingwebsite.com, daringfireball.net,
Gutenberg top, w3.org, BBC News.

FAIL (6), each triaged to its SW stage:

| Site | Symptom | Root cause | Owned by |
|---|---|---|---|
| reuters.com | "Please enable JS and disable any ad blocker" | bot-wall / JS-required check | SW5 (JS env surface) |
| bing.com/search | renders nav shell, no results | results are JS-built (fetch + DOM APIs) | SW5 |
| bryanwandrych.com | empty render | 502KB bundle over 64KB prefetch cap; CSR | SW2 (cap) + SW5 (DOM APIs) |
| old.reddit.com | empty render despite ran=18 | **`data:` URL scripts skipped + missing JS builtins (`Set`, `Image`)** | NEW: `data:` script support (SW2) + JS builtins (SW5) |
| lite.cnn.com | empty render, "script 1 too large" | inline script > engine single-alloc budget | SW2/SW3 (cap raise) |
| textboard.org | connection refused (-21) twice | host refused the sim; server-side, not browser | re-scope or replace in matrix |

**NEW DISCOVERY (matrix value proven):** old.reddit.com loads `data:text/javascript,…` URLs —
schema-relative scripts embedded in the page itself. Our pipeline skips them entirely. Supporting
inline data: scripts is a small, general fix that no single-site test would have surfaced.
Also confirmed missing JS builtins (`Set`, `Image`) as a real category of SW5 work.

Raw log: `logs/sim_matrix_baseline_20260920.log`.
