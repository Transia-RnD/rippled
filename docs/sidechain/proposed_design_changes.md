## Overview

This document present a change to the side chain design. I understand that
proposing changes at this late stage is going to raise a lot of flags. Even so,
I believe this change address the current pain points of the current design, is
easier to understand and reason about, it extensible to other ledgers, separates
the federators from the rippled code, and is much more likely to be launched
without issues. 

## Cross chain transaction

We all know how we currently implement a cross chain transaction. In the current
design, sending assets from a source chain to a destination chain takes two
transactions:

1) An initiating transaction on the source chain sent from a source account to a
   door account. This is sent by the source account's wallet. This includes the
   destination chain destination account as a memo in the transaction. 
   
2) An issuing transaction on the destination chain sent from the door account to
   the destination account specified in the initiating chain's memo. This is
   sent by the federators. The federators need to agree on the transaction's fee
   and sequence fields to make this work, and if the transaction fails the
   federators need to handle the failure.
   
The new design proposes that federators no longer sign and submit transactions.
Instead, they will be used to attest to the fact that transactions have taken
place on the different chains. This design uses three transactions to send to
send an asset from a source chain to a destination chain.

1) On the destination chain, an account submits a transaction that adds a ledger
   object that will be used to identify the initiating transaction and prevent
   the initiating transaction from being claimed on the destination chain more
   than once. The door account will keep a new ledger object - a sidechain. This
   sidechain ledger object will keep a counter that is used for "cross chain
   sequence numbers". A "cross chain sequence number" will be checked out
   from this counter and the counter will be incremented. Once checked out, the
   sequence number would be owned by the account that submitted the transaction.
   See the section below for what fields are present in the new sidechain ledger
   object and cross chain sequence number ledger object.
   
2) On the source chain, an initiating transaction is sent from a source account
   to the door account. This transaction will include the "cross chain sequence
   number" from step (1). Note that this transaction does not include a
   destination on the side chain.
   
3) On the destination chain, the the owner of the "cross chain sequence number"
   (see 1) submits a new transaction type called "cross chain claim" that
   includes the "cross chain sequence number", the sidechain, the destination,
   and proof that the transaction deposited a specified asset using a specified
   "cross chain sequence number" occurred on the source chain. If the proof is
   satisfied, and the "cross chain sequence number" is owned by this account, a
   payment is made from the door account to the destination. If the payment
   succeeds, the "cross chain sequence number" is deleted. A "cross chain claim"
   transaction can only succeed once, as the "cross chain sequence number" for
   that transaction can only be created once. In case of error, the funds can be
   sent to an alternate account and eventually returned to the initiating
   account.
  
The last missing piece is the proof that a transaction deposited a specified
asset using a specified cross chain sequence number. One way to do this would be
to use trust trees, but this results in large transactions. Instead, the proof
will consist of a set of signatures that sign the the following data:
 * The cross chain sequence number
 * The amount sent (including asset type)
 * The sidechain (the sidechain includes the two door addresses and src asset
   type and dst asset type).
 
The set of trusted signatures have a role that's analogous to the current
federator signatures. In the new design, instead of a set of federators that
sign transactions directly, there is a server that tracks validated transactions
sent to the door account. Wallets can request signatures attesting to this
information. It will then submit these signatures along with the "cross chain
claim" transaction.
  
## New ledger objects

### Sidechain

This is a ledger object owned by the door account. It contains:

1) The sidechain specification (two door accounts and two asset types).
2) A counter used to create "cross chain sequence numbers"
2) The set of public keys that are used to attest to transactions
3) The number signatures in a attestation needed to confirm a transaction
4) How to map assets assets from the source chain onto this chain. In this
   initial design, both assets have to be either XRP or both have to be IOU, and
   both assets are exchanged at a 1:1 exchange rate. This may be relaxed in the
   future, but it allows the design to not have to deal with an IOU value that
   can't be represented as an XRP value.

The ledger id is a hash of the a constant prefix, the src door account, the src
chain issue, the dst door account, and the dst chain issue. Multiple side chains
may be present on a single account (with different values for the sidechain
parameters, of course).

The current ledger format looks like this:
```
    add(jss::Sidechain,
        ltSIDECHAIN,
        {
            {sfOwner,                soeREQUIRED},
            {sfSidechain,            soeREQUIRED},
            {sfXChainSequence,       soeREQUIRED},
            {sfSignerQuorum,         soeREQUIRED},
            // TODO: This should store public keys, not account ids
            {sfSignerEntries,        soeREQUIRED},
            {sfOwnerNode,            soeREQUIRED},
            {sfPreviousTxnID,        soeREQUIRED},
            {sfPreviousTxnLgrSeq,    soeREQUIRED}
        },
        commonFields);
```

### Sidechain Sequence Number

This is a ledger object owned the account that will submit the cross chain claim
transaction. It contains:

1) the "cross chain sequence number" 
2) the sidechain

The ledger id is a hash of the a constant prefix, the src door account, the src
chain issue, the dst door account, the dst chain issue, and the sequence number. 

The current ledger format looks like this:
```
    add(jss::CrosschainSeqNum,
        ltCROSSCHAIN_SEQUENCE_NUMBER,
        {
            {sfOwner,                soeREQUIRED},
            {sfSidechain,            soeREQUIRED},
            {sfXChainSequence,       soeREQUIRED},
            {sfOwnerNode,            soeREQUIRED},
            {sfPreviousTxnID,        soeREQUIRED},
            {sfPreviousTxnLgrSeq,    soeREQUIRED}
        },
        commonFields);
```

## New Transactions

### Create sidechain

Creates a new sidechain. Currently, the transaction format looks like this,
but note that signer entries will be replaced with public keys.

```
    add(jss::SidechainCreate,
        ttSIDECHAIN_CREATE,
        {
            {sfSidechain, soeREQUIRED},
            {sfSignerQuorum, soeREQUIRED},
            {sfSignerEntries, soeREQUIRED},
        },
        commonFields);
```

### Checkout cross chain sequence number

This requests a new sidechain sequence number from the sidechain object. 
Currently, the transaction format looks like this:
```
    add(jss::SidechainXChainSeqNumCreate,
        ttSIDECHAIN_XCHAIN_SEQNUM_CREATE,
        {
            {sfSidechain, soeREQUIRED},
        },
        commonFields);
```

### Initiate cross chain transaction

This could also be a regular payment transaction that uses memo fields for
auxiliary data, but a dedicated transaction is friendlier to use. Currently, the
transaction format looks like this:

```
    add(jss::SidechainXChainTransfer,
        ttSIDECHAIN_XCHAIN_TRANSFER,
        {
            {sfSidechain, soeREQUIRED},
            {sfXChainSequence, soeREQUIRED},
            {sfAmount, soeREQUIRED},
        },
        commonFields);
```

This will use the payment engine to make the transfer. The issue on the `Amount`
must match the issue specified in the sidechain ledger object.

### Claim cross chain transaction

This is the last step of a cross chain transaction, and occurs on the
destination chain. The transaction format looks like this:

```
    add(jss::SidechainXChainClaim,
        ttSIDECHAIN_XCHAIN_CLAIM,
        {
            {sfXChainClaimProof, soeREQUIRED},
            {sfDestination, soeREQUIRED},
        },
        commonFields);
```

Where the proof includes fields for:
* The sidechain
* The amount sent from the other chain
* The signatures attesting to the data

Note it does not include the message that was signed. The message is implied by
the data.

On success it will send the calculated destination amount from the door account
to the destination account and remove the "cross chain sequence number" from
this account. The destination value is calculated from the asset map on the
sidechain object.

On failure, the "cross chain sequence number" is not removed and may be reused.

This transaction will fail if the cross chain sequence number object does not
exist, or the attestations do not attest for the source amount and sequence
number, or the signatures do not validate the attestations, or the public keys
do not match the public keys in the sidechain object. Note: only a subset of
public keys need to be used. This will use the payment engine to make the
transfer.

### Create Sidechain Account

One disadvantage of this new design is it requires the sender to have an account
on the sidechain. This makes it impossible to use a cross chain transaction to create
an account on the sidechain. Two new transactions are added to work around this restriction:
* XChainAccountCreate
* XChainAccountClaim

This transaction will fail if the account meant to be created already exists.

These two transaction are more awkward to use to use than regular cross chain
transaction, and require that the created account be undeletable (or the
transaction could be replayed - see "disadvantages" section below for a
discussion).

The transaction formats of these two transactions are:

```
    add(jss::SidechainXChainAccountCreate,
        ttSIDECHAIN_XCHAIN_ACCOUNT_CREATE,
        {
            {sfSidechain, soeREQUIRED},
            {sfDestination, soeREQUIRED},
            {sfAmount, soeREQUIRED},
        },
        commonFields);

    add(jss::SidechainXChainAccountClaim,
        ttSIDECHAIN_XCHAIN_ACCOUNT_CLAIM,
        {
            {sfSidechain, soeREQUIRED},
            {sfDestination, soeREQUIRED},
            {sfAmount, soeREQUIRED},
        },
        commonFields);
```

### Modify Sidechain Signatures

TBD

## Attestation Server

TBD. This is similar to the current federator. It will subscribe to an RPC
stream of the transactions on the two chains, and it will sign a statement
attesting to the amount sent to the door account with the given cross chain
sequence number. The stages to build this could be:

1) no verification of transactions, no communications between servers.
2) verify transactions with proof trees
3) communicate between the servers and provide all the signatures at once

I intend to have Greg build this. This is a good introduction to the rippled RPC
commands, signing, and it also lets him build a new greenfield project.

## Advantages for the new design

1) The biggest motivation for the change is the new design can be tested and
   debugged by reasoning about transactions and ledger objects. The current
   design requires testing and debugging a set of communicating servers. I've
   been repeating that when we find bugs in the current design, there is high
   variability for how long those bugs will take to resolve. With the new
   design, the bugs become "regular bugs", that will take a more predictable
   time to fix. The new design also greatly reduces the risk of high priority
   bugs where the federators get out of sync and and unable to get back into
   sync.

2) Handling fee escalation and preventing fees from draining funds from the door
   account. In the new design, door accounts do not send transaction and do not
   pay fees. Fee escalation is handled by the account sending the transaction
   the same way it's handled now, there is no need for special logic in the
   federators.

3) Failed transactions require sending refunds back to the initiating account.
   And if the refund itself fails, we don't have a good solution for that. With
   the new design, if the transaction fails, it can be sent to a different
   account. Failures can be handled similarly to how failed transactions are
   handled now.
   
4) There's special logic to handle the case of federators falling behind
   triggering transactions. This requires sending "out of band" transactions
   using tickets. The new design doesn't need to handle this at all.
   
4) There's a bottle-neck of sending many transaction from the same door account.
   The new design sends transactions from different accounts.

5) Changing the trusted signatures is much simpler.

6) "Multi-hop" transactions become simpler.

7) Issuing side chain assets onto the main chain becomes straight forward.

8) The server used to attest to transactions can be separate from rippled.

9) It is extensible to interacting with other ledgers - it is much less XRP
   ledger specific.
   
## Disadvantages for the new design

1) The new design requires that the account sending a cross chain transaction
   has an account on both the main chain and the side chain. The old design did
   not require this. The solution to this problem is to use special "create side
   chain account" cross chain transactions. This would not require a
   preallocated "cross chain sequence number". The attestors could sign this
   transaction that would be submitted on behalf of an account controlled by the
   attestors keys (although the funds would still come from the door account).
   This transaction would fail if the account already exists. This prevents the
   transaction from being submitted multiple times (which would normally be
   prevented by the "cross chain sequence number"). Given the new simplified
   attester server, the sequence number for this transaction may be incorrect
   when the transaction is submitted. This can be handled by signing multiple
   transactions with different sequence numbers. Since the transaction fails if
   the account exists, and the account is undeletable, this is not a security
   issue. However, it does make the transaction awkward to use.
   
   Notes: 
       * if for some reason this fails there may not be a way to get a refund. 
       * This account should not be deletable, or the txn could be replayed. 
       * If there are lots of these transactions it will be difficult to
         correctly guess what the sequence number should be. 
       * If the destination asset is different from the asset used to pay fees
         it is unclear how the fee will be paid.
   
2) The new design requires new transaction types and ledger objects.

3) The new design puts more complexity into wallets.

## Effects on schedule

TDB. Honestly, I expect this design will be ready go into production sooner than
the existing design. It's just so much easier to deal with transactions and
ledger objects than interactions between distributed servers.

## Effects on tooling and documentation

TBD. This will require the most changes.

