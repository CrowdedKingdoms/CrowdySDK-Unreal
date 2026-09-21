# AGENTS

## Branches

The default branch is `prod`. Work happens on `dev`, `test`, and `prod`.
Do not recreate `main`. `v2_0` is an old snapshot; do not branch from it or merge to it.

A direct push to `dev`, `test`, or `prod` is refused for everyone, including an org admin.
Open a pull request.

| Branch | What you do |
|---|---|
| `dev` | Branch off `origin/dev`. Open a pull request into `dev`. Merge it yourself. An ordinary change needs no review. |
| `test` | Open a pull request from `dev` into `test`. Do not merge it. Ask an org admin to merge. |
| `prod` | Open a pull request from `test` into `prod`. Do not merge it. Ask an org admin to merge. |

On `dev`, merge with the API. `gh pr merge` can refuse before it sends the request:

```bash
gh api repos/CrowdedKingdoms/CrowdySDK-Unreal/pulls/<n>/merge -X PUT -f merge_method=merge
```

`prod` holds released versions (`vX.Y.Z` tags). `dev` holds everything merged since the last release. `test` sits between them. Promote by merging forward, one branch at a time.
