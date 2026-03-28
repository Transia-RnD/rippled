# XRPL Amendment Creation Skill

This skill guides you through creating XRPL amendments, whether for brand new features or fixes/extensions to existing functionality.

## Amendment Types

There are two main types of amendments:

1. **New Feature Amendment** (`feature{Name}`) - Entirely new functionality
2. **Fix/Extension Amendment** (`fix{Name}`) - Modifications to existing functionality

## Step 1: Determine Amendment Type

Ask the user:
- Is this a **brand new feature** (new transaction type, ledger entry, or capability)?
- Or is this a **fix or extension** to existing functionality?

---

## For NEW FEATURE Amendments

### Checklist:

#### 1. Feature Definition in features.macro
**ONLY FILE TO EDIT:** `include/xrpl/protocol/detail/features.macro`

- [ ] Add to TOP of features.macro (reverse chronological order):
```
XRPL_FEATURE(YourFeatureName, Supported::no, VoteBehavior::DefaultNo)
```
- [ ] This creates the variable `featureYourFeatureName` automatically
- [ ] Follow naming convention: Use the feature name WITHOUT the "feature" prefix
- [ ] Examples: `Batch` → `featureBatch`, `LendingProtocol` → `featureLendingProtocol`

#### 2. Support Status
- [ ] Start with `Supported::no` during development
- [ ] Change to `Supported::yes` when ready for network voting
- [ ] Use `VoteBehavior::DefaultNo` (validators must explicitly vote for it)

#### 3. Code Implementation
- [ ] Implement new functionality (transaction type, ledger entry, etc.)
- [ ] Add feature gate check in preflight:
```cpp
if (!env.current()->rules().enabled(feature{Name}))
{
    return temDISABLED;
}
```

#### 4. Disable Route Handling
- [ ] Ensure transaction returns `temDISABLED` when amendment is disabled
- [ ] Implement early rejection in preflight/preclaim phase
- [ ] Add appropriate error messages

#### 5. Test Implementation
Create comprehensive test suite with this structure:

```cpp
class {FeatureName}_test : public beast::unit_test::suite
{
public:
    void testEnable(FeatureBitset features)
    {
        testcase("enabled");

        // Test with feature DISABLED
        {
            auto const amendNoFeature = features - feature{Name};
            Env env{*this, amendNoFeature};

            env(transaction, ter(temDISABLED));
        }

        // Test with feature ENABLED
        {
            Env env{*this, features};

            env(transaction, ter(tesSUCCESS));
            // Validate new functionality works
        }
    }

    void testPreflight(FeatureBitset features)
    {
        testcase("preflight");
        // Test malformed transaction validation
    }

    void testPreclaim(FeatureBitset features)
    {
        testcase("preclaim");
        // Test signature and claim phase validation
    }

    void testWithFeats(FeatureBitset features)
    {
        testEnable(features);
        testPreflight(features);
        testPreclaim(features);
        // Add feature-specific tests
    }

    void run() override
    {
        using namespace test::jtx;
        auto const all = supported_amendments();
        testWithFeats(all);
    }
};
```

#### 6. Test Coverage Requirements
- [ ] Test amendment DISABLED state (returns `temDISABLED`)
- [ ] Test amendment ENABLED state (returns `tesSUCCESS`)
- [ ] Test malformed transactions
- [ ] Test signature validation
- [ ] Test edge cases specific to feature
- [ ] Test amendment transition behavior

#### 7. Documentation
- [ ] Create specification document (XLS_{NAME}.md)
- [ ] Document new transaction types, ledger entries, or capabilities
- [ ] Create test plan document
- [ ] Document expected behavior when enabled/disabled

---

## For FIX/EXTENSION Amendments

### Checklist:

#### 1. Fix Definition in features.macro
**ONLY FILE TO EDIT:** `include/xrpl/protocol/detail/features.macro`

- [ ] Add to TOP of features.macro (reverse chronological order):
```
XRPL_FIX(YourFixName, Supported::no, VoteBehavior::DefaultNo)
```
- [ ] This creates the variable `fixYourFixName` automatically
- [ ] Follow naming convention: Use the fix name WITHOUT the "fix" prefix (it's added automatically)
- [ ] Examples: `TokenEscrowV1` → `fixTokenEscrowV1`, `DirectoryLimit` → `fixDirectoryLimit`
- [ ] Start with `Supported::no` during development, change to `Supported::yes` when ready

#### 2. Backward Compatibility Implementation
**Critical**: Use enable() with if/else to preserve existing functionality

```cpp
// Check if fix is enabled
bool const fix{Name} = env.current()->rules().enabled(fix{Name});

// Conditional logic based on amendment state
if (fix{Name})
{
    // NEW behavior with fix applied
    // This is the corrected/improved logic
}
else
{
    // OLD behavior (legacy path)
    // Preserve original functionality for backward compatibility
}
```

**Alternative pattern with ternary operator:**
```cpp
auto& viewToUse = sb.rules().enabled(fix{Name}) ? sb : legacyView;
```

#### 3. Multiple Fix Versions Pattern
For iterative fixes, use version checking:

```cpp
bool const fixV1 = rv.rules().enabled(fixXahauV1);
bool const fixV2 = rv.rules().enabled(fixXahauV2);

switch (transactionType)
{
    case TYPE_1:
        if (fixV1) {
            // Behavior with V1 fix
        } else {
            // Legacy behavior
        }
        break;

    case TYPE_2:
        if (fixV2) {
            // Behavior with V2 fix
        } else if (fixV1) {
            // Behavior with only V1
        } else {
            // Legacy behavior
        }
        break;
}
```

#### 4. Test Both Paths
Always test BOTH enabled and disabled states:

```cpp
void testFix(FeatureBitset features)
{
    testcase("fix behavior");

    for (bool withFix : {false, true})
    {
        auto const amend = withFix ? features : features - fix{Name};
        Env env{*this, amend};

        // Setup test scenario
        env.fund(XRP(1000), alice);
        env.close();

        if (!withFix)
        {
            // Test OLD behavior (before fix)
            env(operation, ter(expectedErrorWithoutFix));
            // Verify old behavior is preserved
        }
        else
        {
            // Test NEW behavior (after fix)
            env(operation, ter(expectedErrorWithFix));
            // Verify fix works correctly
        }
    }
}
```

#### 5. Security Fix Pattern
For security-critical fixes (like fixBatchInnerSigs):

```cpp
// Test vulnerability exists WITHOUT fix
{
    auto const amendNoFix = features - fix{Name};
    Env env{*this, amendNoFix};

    // Demonstrate vulnerability
    // Expected: Validity::Valid (WRONG - vulnerable!)
    BEAST_EXPECT(result == Validity::Valid);
}

// Test vulnerability is FIXED WITH amendment
{
    Env env{*this, features};

    // Demonstrate fix
    // Expected: Validity::SigBad (CORRECT - protected!)
    BEAST_EXPECT(result == Validity::SigBad);
}
```

#### 6. Test Coverage Requirements
- [ ] Test fix DISABLED (legacy behavior preserved)
- [ ] Test fix ENABLED (new behavior applied)
- [ ] Test amendment transition
- [ ] For security fixes: demonstrate vulnerability without fix
- [ ] For security fixes: demonstrate protection with fix
- [ ] Test edge cases that triggered the fix
- [ ] Test combinations with other amendments

#### 7. Documentation
- [ ] Document what was broken/suboptimal
- [ ] Document the fix applied
- [ ] Document backward compatibility behavior
- [ ] Create test summary showing both paths

---

## Best Practices for All Amendments

### 1. Naming Conventions
- New features: `feature{DescriptiveName}` (e.g., `featureBatch`, `featureHooks`)
- Fixes: `fix{IssueDescription}` (e.g., `fixBatchInnerSigs`, `fixNSDelete`)
- Use CamelCase without underscores

### 2. Feature Flag Checking
```cpp
// At the point where behavior diverges:
bool const amendmentEnabled = env.current()->rules().enabled(feature{Name});

// Or in view/rules context:
if (!rv.rules().enabled(feature{Name}))
    return {};  // or legacy behavior
```

### 3. Error Codes
- New features when disabled: `temDISABLED`
- Fixes may return different validation errors based on state
- Document all error code changes

### 4. Test Structure Template
```cpp
class Amendment_test : public beast::unit_test::suite
{
public:
    // Core tests
    void testEnable(FeatureBitset features);      // Enable/disable states
    void testPreflight(FeatureBitset features);   // Validation
    void testPreclaim(FeatureBitset features);    // Claim phase

    // Feature-specific tests
    void test{SpecificScenario1}(FeatureBitset features);
    void test{SpecificScenario2}(FeatureBitset features);

    // Master orchestrator
    void testWithFeats(FeatureBitset features)
    {
        testEnable(features);
        testPreflight(features);
        testPreclaim(features);
        test{SpecificScenario1}(features);
        test{SpecificScenario2}(features);
    }

    void run() override
    {
        using namespace test::jtx;
        auto const all = supported_amendments();
        testWithFeats(all);
    }
};

BEAST_DEFINE_TESTSUITE(Amendment, app, ripple);
```

### 5. Documentation Files
Create these files:
- **Specification**: `XLS_{FEATURE_NAME}.md` - Technical specification
- **Test Plan**: `{FEATURE}_COMPREHENSIVE_TEST_PLAN.md` - Test strategy
- **Test Summary**: `{FEATURE}_TEST_IMPLEMENTATION_SUMMARY.md` - Test results
- **Review Findings**: `{FEATURE}_REVIEW_FINDINGS.md` (if applicable)

### 6. Amendment Transition Testing
Test the moment an amendment activates:

```cpp
void testAmendmentTransition(FeatureBitset features)
{
    testcase("amendment transition");

    // Start with amendment disabled
    auto const amendNoFeature = features - feature{Name};
    Env env{*this, amendNoFeature};

    // Perform operations in disabled state
    env(operation1, ter(temDISABLED));

    // Enable amendment mid-test (if testing mechanism supports it)
    // Verify state transitions correctly

    // Perform operations in enabled state
    env(operation2, ter(tesSUCCESS));
}
```

### 7. Cache Behavior
If amendment affects caching (like fixBatchInnerSigs):
- [ ] Test cache behavior without fix
- [ ] Test cache behavior with fix
- [ ] Document cache invalidation requirements

### 8. Multi-Amendment Combinations
Test interactions with other amendments:

```cpp
void testMultipleAmendments(FeatureBitset features)
{
    // Test all combinations
    for (bool withFeature1 : {false, true})
    for (bool withFeature2 : {false, true})
    {
        auto amend = features;
        if (!withFeature1) amend -= feature1;
        if (!withFeature2) amend -= feature2;

        Env env{*this, amend};
        // Test interaction behavior
    }
}
```

### 9. Performance Considerations
- [ ] Minimize runtime checks (cache `rules().enabled()` result if used multiple times)
- [ ] Avoid nested feature checks where possible
- [ ] Document performance impact

### 10. Code Review Checklist
- [ ] Both enabled/disabled paths are tested
- [ ] Backward compatibility is preserved (for fixes)
- [ ] Error codes are appropriate
- [ ] Documentation is complete
- [ ] Security implications are considered
- [ ] Cache behavior is correct
- [ ] Edge cases are covered

---

## Common Patterns Reference

### Pattern: New Transaction Type
```cpp
// In transactor code:
TER doApply() override
{
    if (!ctx_.view().rules().enabled(feature{Name}))
        return temDISABLED;

    // New transaction logic here
    return tesSUCCESS;
}
```

### Pattern: New Ledger Entry Type
```cpp
// In ledger entry creation:
if (!view.rules().enabled(feature{Name}))
    return temDISABLED;

auto const sle = std::make_shared<SLE>(ltNEW_TYPE, keylet);
view.insert(sle);
```

### Pattern: Behavioral Fix
```cpp
// At decision point:
bool const useFix = view.rules().enabled(fix{Name});

if (useFix)
{
    // Corrected behavior
    return performCorrectValidation();
}
else
{
    // Legacy behavior (preserved for compatibility)
    return performLegacyValidation();
}
```

### Pattern: View Selection
```cpp
// Select which view to use based on amendment:
auto& applyView = sb.rules().enabled(feature{Name}) ? newView : legacyView;
```

---

## Example Workflows

### Workflow 1: Creating a New Feature Amendment

1. User requests: "Add a new ClaimReward transaction type"
2. Skill asks: "What should the amendment be called? (e.g., BalanceRewards - without 'feature' prefix)"
3. Add to features.macro:
```
XRPL_FEATURE(BalanceRewards, Supported::no, VoteBehavior::DefaultNo)
```
4. Implement transaction with `temDISABLED` gate using `featureBalanceRewards`
5. Create test suite with testEnable, testPreflight, testPreclaim
6. Run tests with amendment enabled and disabled
7. When ready, update to `Supported::yes` in features.macro
8. Create specification document
9. Review checklist

### Workflow 2: Creating a Fix Amendment

1. User requests: "Fix the signature validation bug in batch transactions"
2. Skill asks: "What should the fix be called? (e.g., BatchInnerSigs - without 'fix' prefix)"
3. Add to features.macro:
```
XRPL_FIX(BatchInnerSigs, Supported::no, VoteBehavior::DefaultNo)
```
4. Implement fix with if/else using `fixBatchInnerSigs` to preserve old behavior
5. Create test demonstrating vulnerability without fix
6. Create test showing fix works when enabled
7. When ready, update to `Supported::yes` in features.macro
8. Document both code paths
9. Review checklist

---

## Quick Reference: File Locations

- **Amendment definitions (ONLY place to add)**: `include/xrpl/protocol/detail/features.macro`
- **Feature.h (auto-generated, DO NOT EDIT)**: `include/xrpl/protocol/Feature.h`
- **Feature.cpp (auto-generated, DO NOT EDIT)**: `src/libxrpl/protocol/Feature.cpp`
- **Test files**: `src/test/app/` or `src/test/protocol/`
- **Specifications**: Project root (e.g., `XLS_SMART_CONTRACTS.md`)
- **Test plans**: Project root (e.g., `BATCH_COMPREHENSIVE_TEST_PLAN.md`)

## How the Macro System Works

The amendment system uses C preprocessor macros:

1. **features.macro** - Single source of truth (ONLY file you edit):
```
XRPL_FEATURE(Batch, Supported::yes, VoteBehavior::DefaultNo)
XRPL_FIX(TokenEscrowV1, Supported::yes, VoteBehavior::DefaultNo)
```

2. **Feature.h** - Auto-generated declarations from macro:
```cpp
extern uint256 const featureBatch;
extern uint256 const fixTokenEscrowV1;
```

3. **Feature.cpp** - Auto-generated registrations from macro:
```cpp
uint256 const featureBatch = registerFeature("Batch", ...);
uint256 const fixTokenEscrowV1 = registerFeature("fixTokenEscrowV1", ...);
```

**DO NOT** modify Feature.h or Feature.cpp directly - they process features.macro automatically.

---

## When to Use This Skill

Invoke this skill when:
- Creating a new XRPL amendment
- Adding a new transaction type or ledger entry
- Fixing existing XRPL functionality
- Need guidance on amendment best practices
- Setting up amendment tests
- Reviewing amendment implementation

The skill will guide you through the appropriate workflow based on amendment type.
