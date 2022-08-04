# Bridges

## Overview

This document defines the ledger objects, transactions, RPC commands, and
additional servers that are used to support bridges on the XRP ledger. A bridge
connects two ledgers: a locking-chain and an issuing-chain (some people call
these a mainchain and a sidechain, but in some cases the terminology can be
confusing). Both are independent ledgers. They have their own validators and can
have their own set of custom transactions. Importantly, there is a way to move
assets from the locking-chain to the issuing-chain and there is a way to return
those assets from the issuing-chain to the locking-chain. This key operation is
called a cross-chain transfer. A cross-chain transfer is not a single
transaction. It happens on two chains, requires multiple transactions and
involves an additional server type called a "witness". This key operation is
described in a section below.

After the mechanics of a cross-chain transfer are understood, an overview of
some supporting transactions are described. This includes transactions to create
bridges, add attestations from the witness servers, create accounts across
chains, and change bridge parameters.

The next sections describe the witness server, how to set-up a bridge, error
handling, and trust assumptions.

Finally, a low-level description of the new ledger objects and transactions are
presented as a reference.

## Nomenclature

Cross-chain transfer: A transaction that moves assets from the locking-chain to
the issuing-chain, or returns those assets from the issuing-chain back to the
locking-chain.

Claim ID: A ledger object used to prove ownership of the funds moved in a
cross-chain transfer.

Account-create ordering number: A counter on the door accounts used to order the
account create transactions.

Door accounts: The account on the locking-chain that is used to put assets into
trust, or the account on the sidechain used to issue wrapped assets. The name
comes from the idea that a door is used to move from one room to another and a
door account is used to move assets from one chain to another.

Locking-chain: Where the assets originate and are put into trust.

Issuing-chain: Where the assets from the locking-chain are wrapped.

Witness server: A server that listens for transactions on one or both of the
chains and signs attestations used to prove that certain events happened on a
chain.

## List of new transactions

XChainCreateBridge
XChainCreateClaimID
XChainCommit
XChainClaim
XChainCreateAccountCommit
XChainAddAttestation
XChainModifyBridge

## List of new ledger objects

Bridge
XChainClaimID
XChainCreateAccountClaimState

## Cross-chain transfers overview

A cross-chain transfer moves assets from the locking-chain to the issuing-chain
or returns those assets from the issuing-chain back to the locking-chain.
Cross-chain transfers need a couple primitives:

1) Put assets into trust on the locking-chain.

2) Issue wrapped assets on the issuing-chain.

3) Return or destroy the wrapped assets on the issuing-chain.

4) On the issuing-chain, prove that assets were put into trust on the locking-chain.

5) On the locking-chain, prove that assets were returned or destroyed on the
issuing-chain.

6) A way to prevent assets from being wrapped multiple times (prevent
transaction replay). The proofs that certain events happened on the different
chains are public and can be submitted multiple times. This must only be able to
wrap or unlock assets once.

In this implementation, a regular XRP ledger account is used to put assets into
trust on the locking-chain, and a regular XRP ledger account is used to issue
assets on the issuing-chain. These accounts will have their master keys disabled
and funds will move from these accounts through a new set of transactions
specifically meant for cross-chain transfers. A special set of "witness servers"
are used to prove that assets were put into trust on the locking-chain or
returned on the issuing-chain. A new ledger object called a "xchain claim id" is
used to prevent transaction replay.

A source chain is the chain where the cross-chain transfer starts - by either
putting assets into trust on the locking-chain or returning wrapped assets on
the issuing-chain. The steps for moving funds from a source chain to a
destination chain are:

1) On the destination chain, an account submits a transaction that adds a ledger
   object called a "xchain claim id" that will be used to identify the
   initiating transaction and prevent the initiating transaction from being
   claimed on the destination chain more than once. This transaction will
   include a signature reward amount, in XRP. Rewards amounts much match the
   amount specified by the sidechain ledger object, and the reward amount will
   be deducted from the account's balance and held on this ledger object.
   Collecting rewards is discussed below. The door account will keep a new
   ledger object - a "bridge". This "bridge" ledger object will keep a counter
   that is used for these "xchain claim ids". A "xchain claim id" will be
   checked out from this counter and the counter will be incremented. Once
   checked out, the claim id would be owned by the account that submitted the
   transaction. See the section below for what fields are present in the new
   "bridge" ledger object and "xchain claim id" ledger object. The actual number
   must be retrieved from the transaction metadata on a validated ledger.
   
2) On the source chain, an initiating transaction is sent from a source account.
   This transaction will include the amount to transfer, bridge spec, "xchain
   claim id" from step (1), and an optional destination account on the
   destination chain. The asset being transferred cross-chain will be
   transferred from the source account to the door account.
   
3) When a witness server sees a new cross-chain transaction, it submits a
   transaction on the destination chain that adds a signature attesting to the
   cross-chain transaction. This will include the amount being transferred
   cross-chain, the account to send the signature reward to on the destination
   chain, the bridge spec, the sending account, and the optional destination
   account. These signatures will be accumulated on the "xchain claim id" object
   on the destination chain. The keys used in these signatures must match the
   keys on the multi-signers list on the door account.

4) When a quorum of signatures have been collected, the cross-chain funds can be
   claimed on the destination chain. If a destination account was specified in
   the initialting transaction on the source chain, the funds will move when the
   transaction that adds the last required witness signature is executed (the
   signature that made the quorum). On success, the "chain claim id" object is
   removed and the signature rewards are paid (see step 6). If there is an error
   (for example, the destination has deposit auth set) or the optional
   destination account was not specified, a "cross-chain claim" transaction must
   be used. Note, if the signers list on the door account changes while the
   signatures are collected, the signers list at the time the quorum is reached
   is controlling. When the quorum is reach, the signing keys will be checked
   against the current signers list, and if a collected signature's key is no
   longer on that list it is removed and signatures will continue to be
   collected.
   
5) On the destination chain, the the owner of the "xchain claim id" (see 1) can
   submit a "cross chain claim" transaction that includes the chain sequence
   number", the bridge spec, and a destination. The "cross "xchain claim id"
   object must exist and must have already collected enough signatures from the
   witness servers for this to succeed. On success, a payment is made from the
   door account to the specified destination, signature rewards are distributed
   (see step 6), and the "xchain claim id" is deleted. A "cross chain claim"
   transaction can only succeed once, as the "xchain claim id" for that
   transaction can only be created once. In case of error, the funds can be sent
   to an alternate account and eventually returned to the initiating account.
   Note that this transaction is only used if the optional destination account
   is not specified in step (2) or there is an error when sending funds to that
   destination account.
   
6) When funds are successfully claimed on the destination chain, the reward pool
   is distrubuted to the signature providers. The rewards will be transferred to
   the destination addresses specified in the messages the witnesses sign. These
   accounts are on the destination chain. (Note: the witness servers specify
   where the rewards for its signature goes, this is not specified on the
   sidechain ledger object).
   
The cross-chain transfer is now complete. Note that the transactions sent by the
witness servers that add their signatures may send signatures in a batch.

## Supporting transactions overview

In addition to the transactions used in a cross-chain transfer (described
above), there are new transactions for creating a bridge, changing bridge
parameters, and for using a cross-chain transfer to create a new account on the
destination chain.

The `XChainCreateBridge` transaction adds a "bridge" ledger object to the
account. This contains the two door accounts (one of which must be the same as
the sender of this transaction), the asset type that will be put into trust on
the locking-chain, the wrapped asset type on the sidechain, and the reward pool
amount in XRP. Optional, the minimum amount of XRP needed for an "account
create" transaction for the locking-chain, the minimum amount of XRP needed for
an "account create" transaction for the sidechain may be specified (only if this
is an XRP to XRP sidechain). If the amount is not specified, the
`XChainCreateAccountCommit` (see below) will be disabled for this bridge.
Currently, this ledger object can never be deleted (tho this my change) and
adding this ledger object means the bridge-specific transactions sent from other
accounts may move funds from this account.

A cross-chain transfer, as described in the section above, requires an account
on the destination chain to checkout a ""chain claim id"". This makes it
difficult to create new accounts using cross-chain transfers. A dedicated
transaction is used to create accounts: `XChainCreateAccountCommit`. This
specifies information similar to the `XChainCommit` transaction, but the
destination account is no longer optional, a signature reward amount must be
specified, and this transaction will only work for XRP to XRP sidechains. The
XRP amount must be greater than or equal to the min creation amount specified in
the sidechain ledger object. If this optional amount is not present, the
transaction will fail. Once this transaction is submitted, it works similarly to
a cross-chain transfer, except the signatures are collected on a
`XChainCreateAccountClaimState` ledger object on the door account. If the
account already exists, the transaction will attempt to transfer the funds to
the existing account. To prevent transaction replay, the transactions that
create the accounts on the destination chain must execute in the same relative
order as the the initiating `XChainCreateAccountCommit` transactions on the
source chain. This transaction ordering requirement means this transaction
should only be enabled where an account can not inadventantly (or malicously)
block subsequent transactions by failing to deliver signatures. If the witness
servers themselves submit the signatures, they are already trusted not to be
malicious and are designed to reliably submit the required signatures.

The `XChainAddAttestation` transaction is used to by witness servers (or accounts
that use witness servers) to add a witness's attestation that some event
happened.

There is also a `XChainModifyBridge` transaction used to change new account
creation amounts and the reward amounts. Note: if the reward amount changes
between the time a transaction is initiated the time the reward is collected,
the old amount is used (as that is the amount the source account paid).


## Witness Server

A witness server is an independent server that helps provide proof that some
event happened on either the locking-chain or the issuing-chain. When they
detect an event of interest, they use the `XChainAddAttestation` transaction to
add their attestation that the event happened. When a quorum of signatures are
collected on the ledger, the transaction predicated on that event happening is
unlocked (and may be triggered automatically when the quorum of signatures is
reached). Witness servers are independent from the servers that run the chains
themselves.

It is possible for a witness server to provide attestations for one chain only -
and it is possible for the door account on the locking-chain to have a different
signer's list than the door account on the issuing-chain. The initial
implementation of the witness server assumes it is providing attestation for
both chains, however it is desirable to allow witness servers that only know
about one of the chains.

The current design envisions two models for how witness servers are used. In the
first model, the servers are completely private. They submit transactions to the
chains themselves and collect the rewards themselves. Allowing the servers to be
private has the advantage of greatly reducing the attack surface on these
servers. They won't have to deal with adversarial input to their RPC commands,
and since their ip address will be unknown, it will be hard to mount an DOS
attack.

In the second model, the witness server monitors events on a chain, but does not
submit their signatures themselves. Instead, another party will pay the witness
server for their signature (for example, through a subscription fee), and the
witness server allows that party to collect the signer's reward. The account
that the signer's reward goes to is part of the message that the witness server
signs. In this model, it is likely that the witness server only listens to
events on one chain. This model allows for a single witness server to act as a
witness for multiple bridges (tho only one side of the bridge).

As a side note, since submitting a signature requires submitting a transaction
and paying a fee, supporting rewards for signatures is an important requirement.
Of course, the reward can be higher than the fee, providing an incentive to
running a witness server.

## Why use the signer's list on the account

The signatures that the witness servers use must match the signatures on that
door's signer's list. But this isn't the only way to implement this. A bridge
ledger object could contain a signer's list that's independent from the door
account. The reasons for using the door account's signers list are:

1) The bridge signers list can be used to move funds from the account.
   Putting this list on the door account emphasizes this trust model.
   
2) It allows for emergency action. If something goes very, very wrong, funds
   could still be moved if the entities on the signer's list sign a regular
   transaction.

3) If the door account has multiple sidechains, a strange use-case, but one that
   is currently supported. If the sidechain share a common asset type, the trust
   model is the union of the signer's list. Keeping the signer's list on the
   account makes this explicit.

4) It's a more natural way to modify bridge parameters.

## Setting up an issuing-chain

Setting up an issuing-chain requires the following:

1) Create a new account for the door account (or use the root account - this can
   be useful for issuing-chains). Note, that while more than one sidechain on a
   door account is supported, is it discouraged.

2) If necessary, setup the trust lines needed for IOUs.

3) Create the bridge ledger object on each door account.

4) If this is an XRP to XRP sidechain, use a `SidechainAccountCreate`
transaction to create accounts for the signature rewards. Use the door account's
master key to add witness signatures for this bootstrap transaction.

5) Enable multi-signatures on the two door accounts. These keys much match the
   keys used by the witness servers. Note that the two door accounts may have
   different multi-signature lists.

6) Disable the master key, so only the keys on the multi-signature list can
   control the account.

## Distributing Signature Rewards

When funds are claimed on the destination chain, the signature rewards will be
distributed. These rewards will be distributed equally between the "reward
accounts" for the attestations that provided the quorum of signatures on the
destination chain that unlocked the claim. If the reward amount is not evenly
dividable among the signers, the remainder is kept by the door account. If a
reward is undeliverable to a reward account, it is kept by the door account.

## Preventing Transaction Replay

Normally, account sequence numbers prevent transaction replay in the XRP ledger.
However, this bridge design allows moving funds from an account from
transactions not sent by that account. All the information to replay these
transactions are publicly available. This section describes how the different
transaction prevent certain attacks - including transaction replay attacks.

To successfully run a `XChainClaim` transaction, the account sending the
transaction must own the `XChainClaimID` ledger object referenced in the witness
server's attestation. Since this claim id is destroyed when the funds are
successfully moved, the transaction cannot be replayed.

To successfully create an account with the `XChainCreateAccountCommit`
transaction, the ordering number must match the current order number on the
sidechain ledger object. After the transaction runs, the order number on the
sidechain ledger object is incremented. Since this number is incremented, the
transaction can not be replayed since the order number in the transaction will
never match again.

Since the `XChainCommit` can contain an optional destination account
on the destination chain, and the funds will move when the destination chain
collects enough signatures, one attack would be for an account to watch for a
`XChainCommit` to be sent and then send their own
`XChainCommit` for a smaller amount. This attack doesn't steal funds,
but it does result in the original sender losing their funds. To prevent this,
when a `XChainClaimID` is created on the destination chain, the account that
will send the `XChainCommit` on the source chain must be specified.
Only the witnesses from this transaction will be accepted on the
`XChainClaimID`.

## Error Handling

Error handling cross-chain transfers is straight forward. The "xchain claim id"
is only destroyed when a claim succeeds. If it fails for any reason - for
example the destination account doesn't exist or has deposit auth set - then an
explicit `XChainClaim` transaction may be submitted to redirect the funds.

If a cross-chain account create fails, recovering the funds are outside the
rules of the sidechain system. Assume the funds are lost (the only way to
recover them would be if the witness servers created a transaction themselves.
But this is unlikely to happen and should not be relied upon.) The "Minimum
account create" amount is meant to prevent these transactions from failing. 

If the signature reward cannot be delivered to the specified account, that portion
of the signature reward is kept by the door account.

## Trust Assumptions

The witness servers are trusted, and if a quorum of them collude they can steal
funds from the door account.

## Ledger Objects

### STXChainBridge

Many of the ledger objects and transactions contain a `STXChainBridge` object. These
are the parameters that define a bridge. It contains the following fields:

* lockingChainDoor: `AccountID` of the door account on the locking-chain. This account
  will hold assets in trust while they are used on the sidechain.
    
* lockingChainIssue: `Issue` of the asset put into trust on the locking-chain.

* issuingChainDoor: `AccountID` of the door account on the sidechain. This account
  will issue wrapped assets representing assets put into trust on the locking-chain.
    
* issuingChainIssue: `Issue` of the asset used to represent from the locking-chain.

Note: `lockingChainDoor` and `issuingChainDoor` must be distinct accounts. This is done
    to help prevent transaction replay attacks.

Note: `lockingChainIssue` and `issuingChainIssue` must both XRP or both be IOUs. This is
    done because the exchange rate is fixed at 1:1, and IOUs and XRP have a
    different numeric range and precision. This requirement may be relaxed in
    the future.


A snippet of the data for C++ class for an `STXChainBridge` is:

```c++
class STXChainBridge final : public STBase
{
    AccountID lockingChainDoor_{};
    Issue lockingChainIssue_{};
    AccountID issuingChainDoor_{};
    Issue issuingChainIssue_{};
}
```

### Bridge

The bridge ledger object is owned by the door account and defines the bridge
parameters. Note, the signatures used to attest to chain events are on the door
account, not on this ledger object. It is created with a `XChainCreateBridge`
transaction, modified with a `XChainModifyBridge` transaction (only the
`MinAccountCreateAmount` and `SignaturesReward` may be changed). It can not be
deleted.

#### Fields

The ledger object contains the following fields:

* Account: The account that owns this object. The door account. Required.

* Balance: If positive, the funds in held in trust on this door account. If
  negative, the wrapped funds issued from this door account. Required.

* SignaturesReward: Total amount, in XRP, to be rewarded for providing a
  signatures for a cross-chain transfer or for signing for the cross-chain
  reward. This will be split among the signers. Required.

* MinAccountCreateAmount: Minimum Amount, in XRP, required for an
  `XChainCreateAccountCommit` transaction. If this is not present, the
  `XChainCreateAccountCommit` will fail. May only be present on XRP to XRP
  sidechains. Optional.

* Bridge: Door accounts and assets. See `STXChainBridge` above. Required.

* XChainClaimID: A counter used to assign unique "chain claim id"s
  in the `XChainCreateClaimID` transaction. Required.
  
* XChainAccountCreateCount: A counter used to order the execution of account
  create transactions. It is incremented every time a successful
  `XChainAccountCreate` transaction is run for the source chain.

* XChainAccountClaimCount: A counter used to order the execution of account
  create transactions. It is incremented every time an `XChainAccountCreate`
  transaction is "claimed" on the destination chain. When the "claim"
  transaction is run on the destination chain, the `XChainAccountClaimCount`
  must match the value that the `XChainAccountCreateCount` had at the time the
  `XChainAccountClaimCount` was run on the source chain. This orders the claims
  to run in the same order that the `XChainAccountCreate` transactions ran on
  the source chain and prevents transaction replay.

The c++ code for this ledger object format is:
```c++
    add(jss::Bridge,
        ltBRIDGE,
        {
            {sfAccount,                soeREQUIRED},
            {sfBalance,                soeREQUIRED},
            {sfSignaturesReward,       soeREQUIRED},
            {sfMinAccountCreateAmount, soeOPTIONAL},
            {sfXChainBridge,              soeREQUIRED},
            {sfXChainClaimID,         soeREQUIRED},
            {sfXChainAccountCreateCount, soeREQUIRED},
            {sfXChainAccountClaimCount, soeREQUIRED},
            {sfOwnerNode,              soeREQUIRED},
            {sfPreviousTxnID,          soeREQUIRED},
            {sfPreviousTxnLgrSeq,      soeREQUIRED}
        },
        commonFields);
```

#### Ledger ID

The ledger id is a hash of a unique prefix for sidechain object, and the fields
in `STXChainBridge`. The C++ code for this is:

```c++
Keylet
bridge(STXChainBridge const& bridge)
{
    return {
        ltBRIDGE,
        indexHash(
            LedgerNameSpace::BRIDGE,
            bridge.lockingChainDoor(),
            bridge.lockingChainIssue(),
            bridge.issuingChainDoor(),
            bridge.issuingChainIssue())};
}
```

### XChainClaimID

The "chain claim id" ledger object must be acquired on the destination before
submitting a `XChainCommit` on the source chain. A `XChainCreateClaimID`
transaction is used for this. Its purpose is to prevent transaction replay
attacks and is also used as a place to collect attestations from witness
servers. It is destroyed when the funds are successfully claimed on the
destination chain.

#### Fields

* Account: The account that owns this object. Required.

* Bridge: Door accounts and assets. See `STXChainBridge` above. Required.

* XChainClaimID: Integer unique sequence number for a cross-chain transfer.
  Required.

* SourceAccount: Account that must send the `XChainCommit` on the
  other chain. Since the destination may be specified in the
  `XChainCommit` transaction, if the `SourceAccount` wasn't specified
  another account to try to specify a different destination and steal the funds.
  This also allows tracking only a single set of signatures, since we know which
  account will send the `XChainCommit` transaction. Required.

* Signatures: Signatures collected from the witness servers. This includes the
  parameters needed to recreate the message that was signed, including the
  amount, optional destination, and reward account for that signature. Required.

* SignatureRewardsBalance: Amount of XRP currently locked for rewarding signers.
  This is paid by the account that creates this object. It must match the value
  on the bridge ledger object at the time of creation. If the value changes on
  the bridge ledger object, the value at the time of creation is still used as
  the reward. Required.

The c++ code for this ledger object format is:
```c++
    add(jss::XChainClaimID,
        ltCROSSCHAIN_SEQUENCE_NUMBER,
        {
            {sfAccount,              soeREQUIRED},
            {sfXChainBridge,            soeREQUIRED},
            {sfXChainClaimID,       soeREQUIRED},
            {sfSourceAccount,        soeREQUIRED},
            {sfSignatures,           soeREQUIRED},
            {sfSignaturesRewardBalance,soeREQUIRED},
            {sfOwnerNode,            soeREQUIRED},
            {sfPreviousTxnID,        soeREQUIRED},
            {sfPreviousTxnLgrSeq,    soeREQUIRED}
        },
        commonFields);
```
#### Ledger ID

The ledger id is a hash of a unique prefix for "chain claim id"s, the
sequence number, and the fields in `STXChainBridge`. The C++ code for this is:

```c++
Keylet
xChainSeqNum(STXChainBridge const& bridge, std::uint32_t seq)
{
    return {
        ltCROSSCHAIN_SEQUENCE_NUMBER,
        indexHash(
            LedgerNameSpace::XCHAIN_SEQ,
            bridge.lockingChainDoor(),
            bridge.lockingChainIssue(),
            bridge.issuingChainDoor(),
            bridge.issuingChainIssue(),
            seq)};
}

```

### XChainCreateAccountClaimState

This ledger object is used to collect signatures for creating an account using a
cross-chain transfer. It is created when an `XChainAddAttestation` transaction
adds a signature attesting to a `XChainAccountCreate` transaction and the
"account create ordering number" is greater than or equal to the current
`XChainAccountClaimCount` on the sidechain ledger object.

#### Fields

* Account: Owner of this object. The door account. Required.

* Bridge: Door accounts and assets. See `STXChainBridge` above. Required.

* TxnID: The transaction ID of the initiating transaction on the other chain.

* Amount: Amount, in XRP, to be used for account creation.

* Signatures: Signatures collected from the witness servers. This includes the
  parameters needed to recreate the message that was signed, including the
  amount, destination, signature reward amount, and reward account for that
  signature. With the exception of the reward account, all signature must sign
  the message created with common parameters.
  
* Order: An integer that determines the order that accounts created through cross-chain
transfers must be performed. Smaller numbers must execute before larger numbers.

TBD: C++ code.

#### LedgerID

The ledger id is a hash of a unique prefix for cross-chain account create
signatures, the sidechain, and the initiating transaction ID.

## Transactions

### XChainCreateBridge

Attach a new bridge to a door account. Once this is done, the cross-chain
transfer transactions may be used to transfer funds from this account.

#### Fields

The transaction contains the following fields:

* Bridge: Door accounts and assets. See `STXChainBridge` above. Required.

* SignaturesReward: Total amount, in XRP, to be rewarded for providing a
  signatures for a cross-chain transfer or for signing for the cross-chain
  reward. This will be split among the signers. Required.

* MinAccountCreateAmount: Minimum Amount, in XRP, required for an
  `XChainCreateAccountCommit` transaction. If this is not present, the
  `XChainCreateAccountCommit` will fail. Only applicaple for XRP to XRP
  sidechains. Optional.

See notes in the `STXChainBridge` section and the `Bridge` ledger object section for
restrictions on these fields (i.e. door account must be unique, assets must both
be XRP or both be IOU).

```c++
    add(jss::XChainCreateBridge,
        ttXCHAIN_CREATE_BRIDGE,
        {
            {sfXChainBridge,              soeREQUIRED},
            {sfSignaturesReward,       soeREQUIRED},
            {sfMinAccountCreateAmount, soeOPTIONAL},
        },
        commonFields);
```


### XChainCreateClaimID

The first step in a cross-chain transfer. The sidechain sequence number must be
created on the destination chain before the `XChainCommit` transaction (which
must reference this number) can be sent on the source chain. The account that
will send the `XChainCommit` on the source chain must be specified in this
transaction (see note on the `SourceAccount` field in the `XChainClaimID` ledger
object for justification). The actual sequence number must be retrieved from a
validated ledger.

#### Fields

* Bridge: Door accounts and assets. See `STXChainBridge` above. Required.

* SourceAccount: Account that must send the `XChainCommit` on the
  other chain. Since the destination may be specified in the
  `XChainCommit` transaction, if the `SourceAccount` wasn't specified
  another account to try to specify a different destination and steal the funds.
  This also allows tracking only a single set of signatures, since we know which
  account will send the `XChainCommit` transaction. Required.

* SignaturesReward: Amount, in XRP, to be used to reward the witness servers for
  providing signatures. Must match the amount on the bridge ledger object.
  This could be optional, but it is required so the sender can be made
  positively aware that these funds will be deducted from their account.
  Required.

```c++
    add(jss::XChainCreateClaimID,
        ttXCHAIN_CREATE_CLAIM_ID,
        {
            {sfXChainBridge,     soeREQUIRED},
            {sfOtherChainAccount, soeREQUIRED},
            {sfSignaturesReward, soeREQUIRED},
        },
        commonFields);
```

### XChainCommit

Put assets into trust on the locking-chain so they may be wrapped on the sidechain,
or return wrapped assets on the sidechain so they can be unlocked on the
locking-chain. The second step in a cross-chain transfer.

#### Fields

* Bridge: Door accounts and assets. See `STXChainBridge` above. Required.

* XChainClaimID: Integer unique id for a cross-chain transfer. Must be acquired
  on the destination chain and checked from a validated ledger before submitting
  this transaction. If an incorrect sequence number is specified, the funds will
  be lost. Required.
  
* OtherChainDestination: Destination account on the other chain. Must exist on the other
  chain or the transaction will fail. However, if the transaction fails in this
  case, the funds can be recovered with a `XChainClaim` transaction. Optional.

Note: Only the account specified in the `SourceAccount` field of the
`XChainCreateClaimID` transaction should send this transaction. If it is sent
from another account the funds will be lost.

```c++
    add(jss::XChainCommit,
        ttXCHAIN_COMMIT,
        {
            {sfXChainBridge, soeREQUIRED},
            {sfXChainClaimID, soeREQUIRED}
            {sfAmount, soeREQUIRED},
            {sfOtherChainDestination, soeOPTIONAL},
        },
        commonFields);
```

### XChainClaim

Claim funds from a `XChainCommit` transaction. This is normally not needed, but
may be used to handle transaction failures or if the destination account was not
specified in the `XChainCommit` transaction. It may only be used after a quorum
of signatures have been sent from the witness servers.

If the transaction succeeds in moving funds, the referenced `XChainClaimID`
ledger object will be destroyed. This prevents transaction replay. If the
transaction fails, the `XChainClaimID` will not be destroyed and the transaction
may be re-run with different parameters.

#### Fields

* Bridge: Door accounts and assets. See `STXChainBridge` above. Required.

* XChainClaimID: Integer unique sequence number that identifies the claim and
  was referenced in the `XChainCommit` transaction.

* Destination: Destination account on this chain. Must exist on the other chain
  or the transaction will fail. However, if the transaction fails in this case,
  the sequence number and collected signatures will not be destroyed and the
  transaction may be rerun with a different destination address.
  
* Amount: Amount to claim on the destination chain. Must match the amount
  attested to on the claim id's chain attestations.
  
```c++
    add(jss::XChainClaim,
        ttXCHAIN_CLAIM,
        {
            {sfXChainBridge, soeREQUIRED},
            {sfXChainClaimID, soeREQUIRED},
            {sfDestination, soeREQUIRED},
            {sfAmount, soeREQUIRED},
        },
        commonFields);
```

### XChainCreateAccountCommit

This is a special transaction used for creating accounts through a cross-chain
transfer. A normal cross-chain transfer requires a "chain claim id" (which
requires an existing account on the destination chain). One purpose of the
"chain claim id" is to prevent transaction replay. For this transaction, we use
a different mechanism: the accounts must be claimed on the destination chain in
the same order that the `XChainCreateAccountCommit` transactions occured on the
source chain.

This transaction can only be used for XRP to XRP sidechains.

IMPORTANT: This transaction should only be enabled if the witness attestations
will be reliably delivered to the destination chain. If the signatures are not
delivered (for example, the chain relies on use accounts to collect signatures)
then account creation would be blocked for all transactions that happened after
the one waiting on attestations. This could be used maliciously. To disable this
transaction on XRP to XRP bridges, the bridge's `MinAccountCreateAmount` should
not be present.

Note: If this account already exists, the XRP is transfered to the existing
account. However, note that unlike the `XChainCommit` transaction, there is no
error handling mechanism. If the claim transaction fails, there is no mechanism
for refunds. The funds are permanently lost. This transaction should still only
be used for account creation.

#### Fields

* Bridge: Door accounts and assets. See `STXChainBridge` above. Required.

* OtherChainDestination: Destination account on the other chain. Required.
  
* Amount: Amount, in XRP, to use for account creation. Must be greater than or
  equal to the amount specified in the sidechain ledger object. Required.
  
* SignaturesReward: Amount, in XRP, to be used to reward the witness servers for
  providing signatures. Must match the amount on the sidechain ledger object.
  This could be optional, but it is required so the sender can be made
  positively aware that these funds will be deducted from their account.
  Required.

```c++

    add(jss::XChainCreateAccountCommit,
        tXCHAIN_CREATE_ACCOUNT_COMMIT,
        {
            {sfXChainBridge, soeREQUIRED},
            {sfOtherChainDestination, soeREQUIRED},
            {sfAmount, soeREQUIRED},
            {sfSignaturesReward, soeREQUIRED},
        },
        commonFields);
```

### XChainAddAttestation

Provide a signature attesting to some event on the other chain. The signatures
must be from one of the keys on the door's signer's list at the time the
signature was provided. However, if the signature list changes between the time
the signature was submitted and the quorum is reached, the new signature set is
used and some of the currently collected signatures may be removed. Also note
the reward is only sent to accounts that have keys on the current list.

To help with transaction throughput and to minimize fees, signatures can be
submitted in batch. If any signature is added, the transaction succeeds. The
metadata must be used to find out which specific signatures succeeded.

Note that any account can submit signatures. This is important to support
witness servers that work on the "subscription" model.

An attestation bears witness to a particular event on the other chain. It contains:

* Bridge: Door accounts and assets. See `STXChainBridge` above. Required.
* SignatureRewardAccount: Account to send this signer's share of the signer's
  reward. Required.
* SignatureReward: Signature reward for this event.
  Optional (required for `XChainCreateAccountCommit` transactions only).
* PublicKey: Public key used to verify the signature.
* Signature: Signature bearing witness to the event on the other chain.
* Event type: Type of event (XChainAccountCreate, XChainTransfer)
* Event data: Data needed to recreate the message signed by the witness servers.
  The data depends on event. TBD.
  
Note a quorum of signers need to agree on the `SignatureReward`, the same way
they need to agree on the other data. A single witness server cannot provide an
incorrect value for this in an attempt to collect a larger reward.

To add a witness to a `XChainClaimID`, that ledger object must already exist.
If the `XChainCreateAccountClaimState` does not already exist, and the ordering
number is greater than the current ordeing number, it will be created.

Since `XChainCreateAccountClaimState` are ordered, it's possible for this object
to collect a quorum of signatures but not be albe to execute yet. For this
reason an empty signature may be sent to an `XChainCreateAccountClaimState` and
if it already has a quorum of signatures it will execute. The witness servers
will detect when this needs to happen.

#### Fields

* Attestations. A collection of attestations, witnessing a particular event on
  the other chain. This collection may be contain attestations for different
  sidechain ledger objects. See section above for fields in an attestation.

### XChainModifyBridge

Change the `SignaturesReward` field or the `MinAccountCreateAmount` on the
sidechain object. Note that this is a regular transaction that is sent by the
door account and requires the entities that control the witness servers to
co-operate and provide the signatures for this transaction. This happens outside
the ledger.

The `SignaturesReward` and `MinAccountCreateAmount` of a transaction are the
values that were in effect at the time the transaction was submitted.

Note that the signer's list for the sidechain is not modified through this
transaction. The signer's list is on the door account itself and is changed in
the same way signer's lists are changed on accounts.

#### Fields

The transaction contains the following fields:

* Bridge: Door accounts and assets. See `STXChainBridge` above. Required.

* SignaturesReward: Total amount, in XRP, to be rewarded for providing a
  signatures for a cross-chain transfer or for signing for the cross-chain
  reward. This will be split among the signers. Optional.

* MinAccountCreateAmount: Minimum Amount, in XRP, required for an
  `XChainCreateAccountCommit` transaction. If this is zero, the field will be
  removed from the ledger object. Optional
  
At least one of `SignaturesReward` and `MinAccountCreateAmount` must be present.

```c++
    add(jss::XChainModifyBridge,
        ttSIDECHAIN_MODIFY,
        {
            {sfXChainBridge,              soeREQUIRED},
            {sfSignaturesReward,       soeOPTIONAL},
            {sfMinAccountCreateAmount, soeOPTIONAL},
        },
        commonFields);
```


## New RPC Commands

* Given a sidechain description, get the sidechain ledger object

## Extensions

The `XChainCreateAccountCommit` transaction can be extended to provide a
refund on the source chain when the transaction fails on the destination chain.
The idea is similar to a `XChainAccountCreate` transaction. It would be to
increment the `XChainAccountCreateCount` on the destination chain, and the
transaction would have to be ordered on the source chain. This could be
introduced with an amendment at a later time.

## Alternate designs

One alternate design that was implemented was a set of servers similar to the
witness servers called "federators". These servers communicated among
themselves, collected signatures needed to submit transactions on behalf of the
door accounts directly, and submitted those transactions. This design is
attractive from a usability point of view. There is not "cross-chain sequence
number" that needs to be obtained on the destination chain, and creating
accounts using cross-chain transfers is straight forward. The disadvantages of
this design are the complexity in the federators. In order to submit a
transaction, the federators needed to agree on a transaction's sequence number
and fees. This required the federators to stay in "sync" with each other and
required that these federators be much more complex than the "witness" servers
proposed here. In addition, handing fee escalation, failed transactions, and
servers falling behind was much more complex. Finally, because all the
transactions were submitted from the same account (the door account) this
presented a challenge for transaction throughput as the XRP ledger limits the
number of transactions an account can submit in a single ledger.
