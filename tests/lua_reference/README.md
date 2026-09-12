# Lua reference captures — provenance

These directories hold authoritative outputs captured from the **verbatim
CometBrowser Lua source** (the reference implementation) so the C port can
assert byte-exact parity instead of hand-echoing behavior.

- `p17_dom/` — tokenizer→DOM tree shapes (tc1–tc8/tc5b/tc6), generated during P17.
- `p18_document/` — parse helpers + parse-skeleton outputs, generated during P18.
- `p19_walker/` — FULL pipeline (tokenizer→DOM→Document.parse MODE_RAW_HTML)
  over a 46-case corpus covering every element-walker branch; `expected.txt` is
  the byte-exact battery expectation. Captured during P19 analysis
  (2026-09-05).

Regenerating: `lua tests/lua_reference/<dir>/run.lua > <dir>/expected*.txt`
(requires plain `lua` — the scripts dofile CometBrowser sources directly,
so they must be re-run on a machine with the reference project present).