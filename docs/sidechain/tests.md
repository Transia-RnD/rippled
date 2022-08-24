# Sidechain Transaction Tests

## BridgeCreate

### Create a bridge with the following conditions. All of these transactions should fail.

* Bridge not owned by one of the door account.

* Create twice on the same account.

* Create where both door accounts are on the same chain. The second bridge create should fail.

* Bridge where the two door accounts are equal.

* Create an bridge on an account with no enough balance to meet the new reserve

* Test all combinations of the folloing:
Locking chain is IOU with locking chain door account as issuer
Locking chain is IOU with issuing chain door account that exists on the locking chain as issuer
Locking chain is IOU with issuing chain door account that does not exists on the locking chain as issuer
Locking chain is IOU with non-locking chain door account (that exists on the locking chain ledger) as issuer
Locking chain is IOU with non-locking chain door account (that does not exist exists on the locking chain ledger) as issuer
Locking chain is XRP

Issuing chain is IOU with issuing chain door account as the issuer
Issuing chain is IOU with non-issuing chain door account (that exists on the ledger issuing chain ledger) as the issuer
Issuing chain is IOU with non-issuing chain door account (that does not exists on the ledger issuing chain ledger) as the issuer
Issuing chain is IOU with locking chain door account (that exists on the issuing chain ledger) as the issuer
Issuing chain is IOU with locking chain door account (that deos not exist exists on the issuing chain ledger) as the issuer
Issuing chain is XRP and issuing chain door account is the root account
Issuing chain is XRP and issuing chain door account is not the root account

That's 42 combinations. The only combinations that should succeed are:
Locking chain is any IOU, Issuing chain is IOU with issuing chain door account as the issuer
Locking chain is XRP, Issuing chain is XRP with issuing chain is the root account.

* Reward amount is non-xrp

* Reward amount is XRP and negative

* Reward amount is zero

* Min create amount is non-xrp

* Min create amount is zero

* Min create amount is negative

(Note: The checks that the issuing door accout must be the root account for XRP
or the issuer for IOU is not yet written - those test will unexpectly succeed
until that's in)

### Create a brige with the following conditions. All should succeed.

* Reward amount is positive and XRP

* Min create amount is positive and XRP

## Bridge modify

### Modify a brige with the following conditions. All should fail.
* Changing a non-existant bridge should fail

* Reward amount is non-xrp

* Reward amount is XRP and negative

* Reward amount is zero

* Min create amount is non-xrp

* Min create amount is zero

* Min create amount is negative

### How changing parameters effect existing objects:

* Test that the reward paid from a claim Id was the reward when the claim id was
  created, not the reward since the bridge was modified.

* Test that the signatures used to verify attestations and decide if there is a
  quorom are the current signer's list on the door account, not the signer's
  list that was in effect when the claim id was created.

## Claim

### Run a claim with the following conditions. All should fail:

* Claim against non-existant bridge

* Claim against non-existant claim id

* Claim against a claim id owned by another account

* Claim against a claim id with no attestations

* Claim against a claim id with attestations, but not enough to make a quorum

* Claim id of zero

* Claim issue that does not match the expected issue on the bridge (either
  LockingChainIssue or IssuingChainIssue, depending on the chain). The claim id
  should already have enough attestations to reach a quorum for this amount (for
  a different issuer).

* Claim to a destination that does not already exist on the chain

* Claim where the claim id owner does not have enough XRP to pay the reward

* Claim where the claim id owner has enough XRP to pay the reward, but it would
  put his balance below the reserve

* Pay to an account with deposit auth set

* Claim where the amount different from what is attested to

### The following should succeed:

* Claim where the amount matches what is attested to, to an account that exists,
  and there are enough attestations to reach a quorum

### Rewards

* Verify that rewards are paid from the account that owns the claim id

* Verify that if a reward is not evenly divisible amung the reward accounts, the remaining amount goes to the claim id owner.

* If a reward distribution fails for one of the reward accounts (the reward account doesn't exist or has deposit auth set), then the txn should still succeed, but that portion should go to the claim id owner.

* Verify that if a batch of attestations brings the signatures over quorum (say the quorum is 4 and there are 5 attestations) then the reward should be split amung the _five_ accounts.

## Commit

### The following should fail

* Commit to a non-existant bridge

* Commit a negative amount

* Commit an amount whose issue that does not match the expected issue on the bridge (either
  LockingChainIssue or IssuingChainIssue, depending on the chain).
  
* Commit an amount that would put the sender below the required reserve (if XRP)

* Commit an amount above the account's balance (for both XRP and IOUs)

## Create claim id

### The following should fail

* Non-existant bridge
* Creating the new object would put the account below the reserve
* The specified reward doesn't match the reward on the bridge (test by giving the reward amount for the other side, as well as a completely non-matching reward)
* A reward amount that isn't XRP

## Add attestation

* Add an attestation to a claim id that has already reached quorum. This should succeed and share in the reward.

* Check that attestations are being added to the correct chain

* Add a batch of attestations where one has an invalid signature. The entire transaction should fail.

* Test combinations of the following when adding a batch of attestations for different claim ids:
All the claim id exist
One claim id exists and other has already been claimed
None of the claim ids exist
When the claim ids exist, test for both reaching quorum, going over quorum, and not reaching qurorum.

* Add attestations where some of the attestations are inconsistent with each
  other. The entire transaction should fail. Being inconsistent means attesting
  to different values (including different chains).
  
* Test that signature weights are correctly handled. Assign signature weights of 1,2,4,4 and a quorum of 7. Check that the 4,4 signatures reach a quorum, the 1,2,4, reach a quorum, but the 4,2, 4,1 and 1,2 do not.

* Add more than the maximum number of allowed attestations (8). This should fail.

* Add attestations for both account create and claims.

* Confirm that account create transactions happen in the correct order. If they reach
quorum out of order they should not execute until they reach quorum. Re-adding an attestation
should move funds.

* Check that creating an account with less the minimum reserve fails.

* Check that sending funds with an account create txn to an existing account works.

* Check that sending funds to an existing account with deposit auth set fails - for both claim and account create transactions.

* If an account is unable to pay the reserver, check that it fails.

* Create several account with a single batch attestation. This should succeed.

* If an attestation already exists for that server and claim id, the new attestation should replace the old attestation.

* If attestation moves funds, confirm the claim ledger objects are removed (for both account create and "regualar" transactions)

## XChain create account

## Delete door account

* Deleting a account that owns bridge should fail

* Deleting an account that owns a claim id should fail

## Witness tests

* Claim transaction that fails
* Create transaction that fails
