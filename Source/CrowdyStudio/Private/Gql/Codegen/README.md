# CrowdyStudio GraphQL codegen

`generate_studio_queries.py` reads the Crowded Kingdoms SDL and emits schema-grounded
query-string constants for the studio console's operations, plus a drift check.

## Regenerate

From this folder:

```
py -3 generate_studio_queries.py
```

Optional overrides: `py -3 generate_studio_queries.py <SCHEMA_DIR> <OUTPUT_HEADER>`.

Defaults:
- **Schema** — the sibling `cks-docs/static/schema/*.graphql` checkout (eight levels up from
  here, next to the Unreal project). The three files (management-api, game-api, crowdyjs) are
  merged, so an op is found regardless of which API defines it.
- **Output** — `../Generated/CrowdyStudioGeneratedQueries.h` (committed; not built by UBT).

The script prints a `WARNING: ops not found in schema (drift?)` line listing any manifest op
whose root field no longer exists — that is the drift signal to act on after a schema bump.

## What it does / doesn't do

- For each op in the manifest (`QUERY_OPS` / `MUTATION_OPS` at the top of the script) it looks
  up the root field, declares its arguments as GraphQL variables, and expands the return type's
  fields into a selection set (recursing into nested objects up to `MAX_SELECTION_DEPTH`).
- The generated header is a **reference** — the editor still compiles against the hand-written
  bodies in `CrowdyStudioQueries.cpp`. Those are narrower (only the fields the parsers read) and
  hand-tuned; the generated selections are deliberately wide. Diff the two after a schema change
  to catch renamed/removed fields and arguments, then update the hand-written body to match.
- It does not emit response structs or wire generated strings into the build. Promoting the
  editor onto generated strings (and generating the runtime response types) is the larger
  follow-up the Phase 2 doc calls the "full generator"; this is the lightweight standing-up.
