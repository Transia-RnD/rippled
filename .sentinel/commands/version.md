# Version / Release Process

This command handles the release lifecycle for rippled amendments.

## Rippled Release Model

Rippled uses an amendment-based release process. There is no traditional stage/prod deploy.
Instead, features are gated behind amendments that validators vote on. The lifecycle is:

1. **Development** (`Supported::no`) — Feature is implemented but disabled on all networks.
2. **Release candidate** (`Supported::yes, VoteBehavior::DefaultNo`) — Feature ships in a release.
   Validators can vote for it, but won't by default.
3. **Default vote** (`Supported::yes, VoteBehavior::DefaultYes`) — After sufficient testing and
   validator confidence, the default vote flips. Validators must explicitly opt out.
4. **Activation** — When 80%+ of validators vote yes for 2 consecutive weeks, the amendment
   activates permanently on the network. There is no rollback.

## When to Use This Command

Run `/version` when a feature is ready to move between lifecycle stages.

## Workflow

### Stage 1: Development to Release Candidate

The feature is tested and ready to ship in a binary release.

1. Open `include/xrpl/protocol/detail/features.macro`
2. Find your amendment entry (it should be `Supported::no`)
3. Change to `Supported::yes`:
```
XRPL_FEATURE(YourFeature, Supported::yes, VoteBehavior::DefaultNo)
```
4. Verify the feature gate still works:
   - Build the project
   - Run tests with the amendment disabled: confirm `temDISABLED`
   - Run tests with the amendment enabled: confirm `tesSUCCESS`
5. This change ships in the next binary release. Validators who upgrade get the code
   but must explicitly vote to enable it.

### Stage 2: Release Candidate to Default Vote

After the feature has been running on testnet and validators are confident:

1. Open `include/xrpl/protocol/detail/features.macro`
2. Change vote behavior:
```
XRPL_FEATURE(YourFeature, Supported::yes, VoteBehavior::DefaultYes)
```
3. This is a significant change — it means validators who upgrade will automatically
   vote yes unless they opt out. Only do this after:
   - Testnet validation period (minimum 2 weeks recommended)
   - No reported issues from early-adopting validators
   - Community consensus that the feature is ready

### Fix Amendments

Fix amendments (`XRPL_FIX`) follow the same lifecycle but often move faster:
- Security fixes may go directly to `Supported::yes, DefaultYes`
- Bug fixes typically spend less time at `DefaultNo`

## Checklist Before Version Change

- [ ] All tests pass with amendment enabled AND disabled
- [ ] No merge conflicts with current `develop`
- [ ] Amendment transition test exists (disabled -> enabled mid-ledger)
- [ ] Test combinations with other recent amendments
- [ ] For `DefaultYes`: testnet validation period completed
- [ ] For `DefaultYes`: no open issues against the feature
- [ ] PR description documents the version change and rationale

## For Non-Rippled Projects

If this project uses a conventional deploy pipeline instead of amendments:

1. **Stage deploy**: Push to staging environment, run integration tests
2. **Staging validation**: Verify with smoke tests, check monitoring dashboards
3. **Production deploy**: Promote to production after staging sign-off
4. **Rollback plan**: Document how to revert if issues are found post-deploy

The specific commands and environments should be defined in the project's CI/CD config
(`.github/workflows/`, `Makefile`, deployment manifests).
