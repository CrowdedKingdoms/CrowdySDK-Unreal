# AGENTS

## Branches

The default branch is `prod`. Work happens on `dev`, `test`, and `prod`.
Do not recreate `main`. `v2_0` is an old snapshot; do not branch from it or merge to it.

A direct push to `dev`, `test`, or `prod` is refused for everyone, including an org admin.
Open a pull request.

| Branch | What you do |
|---|---|
| `dev` | Branch off `origin/dev`. Open a pull request into `dev`. Merge it yourself. An ordinary change needs no review. |
| `test` | Open a pull request from `dev` into `test`. @Shady-S25 merges it. Anyone else asks an org admin. |
| `prod` | Open a pull request from `test` into `prod`. @Shady-S25 merges it. Anyone else asks an org admin. |

That promotion right is this repository only. @Shady-S25 still cannot push straight to `test` or `prod`. Merge with the API. `gh pr merge` can refuse before it sends the request when a bypass is what lets the merge through:

```bash
gh api repos/CrowdedKingdoms/CrowdySDK-Unreal/pulls/<n>/merge -X PUT -f merge_method=merge
```

`prod` holds released versions (`vX.Y.Z` tags). `dev` holds everything merged since the last release. `test` sits between them. Promote by merging forward, one branch at a time.
