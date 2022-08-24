//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <ripple/app/paths/Flow.h>
#include <ripple/app/tx/impl/SignerEntries.h>
#include <ripple/app/tx/impl/Transactor.h>
#include <ripple/app/tx/impl/XChainBridge.h>
#include <ripple/basics/Log.h>
#include <ripple/basics/XRPAmount.h>
#include <ripple/basics/chrono.h>
#include <ripple/beast/utility/Journal.h>
#include <ripple/ledger/ApplyView.h>
#include <ripple/ledger/PaymentSandbox.h>
#include <ripple/ledger/View.h>
#include <ripple/protocol/AccountID.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/Indexes.h>
#include <ripple/protocol/PublicKey.h>
#include <ripple/protocol/SField.h>
#include <ripple/protocol/STAmount.h>
#include <ripple/protocol/STObject.h>
#include <ripple/protocol/STXChainAttestationBatch.h>
#include <ripple/protocol/STXChainBridge.h>
#include <ripple/protocol/STXChainClaimProof.h>
#include <ripple/protocol/TER.h>
#include <ripple/protocol/TxFlags.h>
#include <ripple/protocol/XChainAttestations.h>
#include <ripple/protocol/digest.h>
#include <ripple/protocol/st.h>

#include <unordered_map>
#include <unordered_set>

namespace ripple {

/*
   Bridges connect two independent ledgers: a "locking chain" and an "issuing
   chain". An asset can be moved from the locking chain to the issuing chain by
   putting it into trust on the locking chain, and issuing a "wrapped asset"
   that represents the locked asset on the issuing chain.

   Note that a bridge is not an exchange. There is no exchange rate: one wrapped
   asset on the issuing chain always represents one asset in trust on the
   locking chain. The bridge also does not exchange an asset on the locking
   chain for an asset on the issuing chain.

   A good model for thinking about bridges is a box that contains an infinite
   number of "wrapped tokens". When a token from the locking chain
   (locking-chain-token) is put into the box, a wrapped token is taken out of
   the box an put onto the issuing chain (issuing-chain-token). No one can use
   the locking-chain-token while it remains in the box. When an
   issuing-chain-token is returned to the box, one locking-chain-token is taken
   out of the box and put back onto the locking chain.

   This requires a way to put assets into trust on one chain (put a
   locking-chain-token into the box). A regular XRP account is used for this.
   This account is called a "door account". Much in the same way that a door is
   used to go from one room to another, a door account is used to move from one
   chain to another. This account will be jointly controlled by a set of witness
   servers by using the ledger's multi-signature support. The master key will be
   disabled. These witness servers are trusted in the sense that if a quorum of
   them collude, they can steal the funds put into trust.

   This also requires a way to prove that assets were put into the box - either
   a locking-chain-token on the locking chain or returning an
   issuing-chain-token on the issuing chain. A set of servers called "witness
   servers" fill this role. These servers watch the ledger for these
   transactions, and attests that the given events happened on the different
   chains by signing messages with the event information.

   There needs to be a way to prevent the attestations from the witness
   servers from being used more than once. "Claim ids" fill this role. A claim
   id must be acquired on the destination chain before the asset is "put into
   the box" on the source chain. This claim id has a unique id, and once it is
   destroyed it can never exist again (it's a simple counter). The attestations
   reference this claim id, and are accumulated on the claim id. Once a quorum
   is reached, funds can move. Once the funds move, the claim id is destroyed.

   Finally, a claim id requires that the sender has an account on the
   destination chain. For some chains, this can be a problem - especially if
   the wrapped asset represents XRP, and XRP is needed to create an account.
   There's a bootstrap problem. To address there, there is a special transaction
   used to create accounts. This transaction does not require a claim id.

   See the document "docs/sidechain/spec.md" for a full description of how
   bridges and their transactions work.
*/

namespace {

enum class TransferHelperCanCreateDst { no, yes };

TER
transferHelper(
    PaymentSandbox& psb,
    AccountID const& src,
    AccountID const& dst,
    std::optional<std::uint32_t> const& dstTag,
    std::optional<AccountID> const& claimOwner,
    STAmount const& amt,
    TransferHelperCanCreateDst canCreate,
    beast::Journal j)
{
    if (dst == src)
        return tesSUCCESS;

    auto const dstK = keylet::account(dst);
    if (auto sleDst = psb.read(dstK))
    {
        // Check dst tag and deposit auth

        if ((sleDst->getFlags() & lsfRequireDestTag) && !dstTag)
            return tecDST_TAG_NEEDED;

        if ((dst != claimOwner) && (sleDst->getFlags() & lsfDepositAuth) &&
            (!psb.exists(keylet::depositPreauth(dst, src))))
        {
            return tecNO_PERMISSION;
        }
    }
    else if (!amt.native() || canCreate == TransferHelperCanCreateDst::no)
    {
        return tecNO_DST;
    }

    if (amt.native())
    {
        auto const sleSrc = psb.peek(keylet::account(src));
        assert(sleSrc);
        if (!sleSrc)
            return tecINTERNAL;

        auto const ownerCount = sleSrc->getFieldU32(sfOwnerCount);
        auto const reserve = psb.fees().accountReserve(ownerCount);

        if ((*sleSrc)[sfBalance] < amt + reserve)
        {
            return tecINSUFFICIENT_FUNDS;
        }

        auto sleDst = psb.peek(dstK);
        if (!sleDst)
        {
            if (canCreate == TransferHelperCanCreateDst::no)
            {
                // Already checked, but ok to check again
                return tecNO_DST;
            }

            // Create the account.
            std::uint32_t const seqno{
                psb.rules().enabled(featureDeletableAccounts) ? psb.seq() : 1};

            sleDst = std::make_shared<SLE>(dstK);
            sleDst->setAccountID(sfAccount, dst);
            sleDst->setFieldU32(sfSequence, seqno);

            psb.insert(sleDst);
        }

        (*sleSrc)[sfBalance] = (*sleSrc)[sfBalance] - amt;
        (*sleDst)[sfBalance] = (*sleDst)[sfBalance] + amt;
        psb.update(sleSrc);
        psb.update(sleDst);

        return tesSUCCESS;
    }

    auto const result = flow(
        psb,
        amt,
        src,
        dst,
        STPathSet{},
        /*default path*/ true,
        /*partial payment*/ false,
        /*owner pays transfer fee*/ true,
        /*offer crossing*/ false,
        /*limit quality*/ std::nullopt,
        /*sendmax*/ std::nullopt,
        j);

    {
        auto const r = result.result();
        if (isTesSuccess(r) || isTecClaim(r) || isTerRetry(r))
            return r;
        return tecXCHAIN_PAYMENT_FAILED;
    }
}

// move the funds
// if funds moved, remove the claimid
// distribute the reward pool
TER
finalizeClaimHelper(
    PaymentSandbox& psb,
    STXChainBridge const& bridgeSpec,
    AccountID const& dst,
    std::optional<std::uint32_t> const& dstTag,
    AccountID const& claimOwner,
    STAmount const& sendingAmount,
    AccountID const& rewardPoolSrc,
    STAmount const& rewardPool,
    std::vector<AccountID> const& rewardAccounts,
    bool wasLockingChainSend,
    // sle for the claim id (may be an NULL or XChainClaimID or
    // XChainCreateAccountClaimID). Don't read fields that aren't in common with
    // those two types and always check for NULL. Remove on success (if not
    // null).
    std::shared_ptr<SLE> const& sleCID,
    beast::Journal j)
{
    STAmount const thisChainAmount = [&] {
        STAmount r = sendingAmount;
        auto const issue = wasLockingChainSend ? bridgeSpec.issuingChainIssue()
                                               : bridgeSpec.lockingChainIssue();
        r.setIssue(issue);
        return r;
    }();
    auto const& thisDoor = wasLockingChainSend ? bridgeSpec.issuingChainDoor()
                                               : bridgeSpec.lockingChainDoor();

    auto const thTer = transferHelper(
        psb,
        thisDoor,
        dst,
        dstTag,
        claimOwner,
        thisChainAmount,
        TransferHelperCanCreateDst::yes,
        j);

    if (!isTesSuccess(thTer))
        return thTer;

    if (sleCID)
    {
        auto const cidOwner = (*sleCID)[sfAccount];
        {
            // Remove the sequence number
            // It's important that the sequence number is only removed if the
            // payment succeeds
            auto const sleOwner = psb.peek(keylet::account(cidOwner));
            auto const page = (*sleCID)[sfOwnerNode];
            if (!psb.dirRemove(
                    keylet::ownerDir(cidOwner), page, sleCID->key(), true))
            {
                JLOG(j.fatal())
                    << "Unable to delete xchain seq number from owner.";
                return tefBAD_LEDGER;
            }

            // Remove the sequence number from the ledger
            psb.erase(sleCID);

            adjustOwnerCount(psb, sleOwner, -1, j);
        }
    }

    if (!rewardAccounts.empty())
    {
        // distribute the reward pool
        STAmount const share = [&] {
            STAmount const den{rewardAccounts.size()};
            return divide(rewardPool, den, rewardPool.issue());
        }();
        STAmount distributed = rewardPool.zeroed();
        for (auto const& ra : rewardAccounts)
        {
            auto const thTer = transferHelper(
                psb,
                rewardPoolSrc,
                ra,
                /*dstTag*/ std::nullopt,
                // claim owner is not relavent to distributing rewards
                /*claimOwner*/ std::nullopt,
                share,
                TransferHelperCanCreateDst::no,
                j);

            if (thTer == tecINSUFFICIENT_FUNDS || thTer == tecINTERNAL)
                return thTer;

            if (isTesSuccess(thTer))
                distributed += share;

            // TODO: Update spec - remaining reward pool stays with the pool
            // source

            // let txn succeed if error distributing rewards (other than
            // inability to pay)
        }

        if (distributed > rewardPool)
            return tecINTERNAL;
    }

    // TODO: Update the bridge balance
    return tesSUCCESS;
}

std::tuple<std::unordered_map<AccountID, std::uint32_t>, std::uint32_t, TER>
getSignersListAndQuorum(ApplyView& view, SLE const& sleB, beast::Journal j)
{
    std::unordered_map<AccountID, std::uint32_t> r;
    std::uint32_t q = std::numeric_limits<std::uint32_t>::max();

    auto const sleS = view.read(keylet::signers(sleB[sfAccount]));
    if (!sleS)
        return {r, q, tecXCHAIN_NO_SIGNERS_LIST};
    q = (*sleS)[sfSignerQuorum];

    auto const accountSigners = SignerEntries::deserialize(*sleS, j, "ledger");

    if (!accountSigners)
    {
        return {r, q, tecINTERNAL};
    }

    for (auto const& as : *accountSigners)
    {
        r[as.account] = as.weight;
    }

    return {std::move(r), q, tesSUCCESS};
};
}  // namespace
//------------------------------------------------------------------------------

NotTEC
BridgeCreate::preflight(PreflightContext const& ctx)
{
    if (!ctx.rules.enabled(featureXChainBridge))
        return temDISABLED;

    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    if (ctx.tx.getFlags() & tfUniversalMask)
        return temINVALID_FLAG;

    auto const account = ctx.tx[sfAccount];
    auto const reward = ctx.tx[sfSignatureReward];
    auto const minAccountCreate = ctx.tx[~sfMinAccountCreateAmount];
    auto const bridge = ctx.tx[sfXChainBridge];
    if (bridge.lockingChainDoor() == bridge.issuingChainDoor())
    {
        return temEQUAL_DOOR_ACCOUNTS;
    }

    if (bridge.lockingChainDoor() != account &&
        bridge.issuingChainDoor() != account)
    {
        return temSIDECHAIN_NONDOOR_OWNER;
    }

    if (isXRP(bridge.lockingChainIssue()) != isXRP(bridge.issuingChainIssue()))
    {
        // Because ious and xrp have different numeric ranges, both the src and
        // dst issues must be both XRP or both IOU.
        return temSIDECHAIN_BAD_ISSUES;
    }

    if (!isXRP(reward) || reward.signum() <= 0)
    {
        return temXCHAIN_BRIDGE_BAD_REWARD_AMOUNT;
    }

    if (minAccountCreate &&
        (!isXRP(*minAccountCreate) || minAccountCreate->signum() <= 0))
    {
        return temXCHAIN_BRIDGE_BAD_MIN_ACCOUNT_CREATE_AMOUNT;
    }

    if (isXRP(bridge.issuingChainIssue()))
    {
        // Issuing account must be the root account for XRP
        static auto const rootAccount = calcAccountID(
            generateKeyPair(
                KeyType::secp256k1, generateSeed("masterpassphrase"))
                .first);
        if (bridge.issuingChainDoor() != rootAccount)
        {
            return temSIDECHAIN_BAD_ISSUES;
        }
    }
    else
    {
        // Issuing account must be the issuer for non-XRP
        if (bridge.issuingChainDoor() != bridge.issuingChainIssue().account)
        {
            return temSIDECHAIN_BAD_ISSUES;
        }
    }

    return preflight2(ctx);
}

TER
BridgeCreate::preclaim(PreclaimContext const& ctx)
{
    auto const account = ctx.tx[sfAccount];
    auto const bridge = ctx.tx[sfXChainBridge];

    if (ctx.view.read(keylet::bridge(bridge)))
    {
        return tecDUPLICATE;
    }

    bool const isLockingChain = (account == bridge.lockingChainDoor());

    if (isLockingChain)
    {
        if (!isXRP(bridge.lockingChainIssue()) &&
            !ctx.view.read(keylet::account(bridge.lockingChainIssue().account)))
        {
            return tecNO_ISSUER;
        }
    }
    else
    {
        // issuing chain
        if (!isXRP(bridge.issuingChainIssue()) &&
            !ctx.view.read(keylet::account(bridge.issuingChainIssue().account)))
        {
            return tecNO_ISSUER;
        }
    }

    {
        // Check reserve
        auto const sle = ctx.view.read(keylet::account(account));
        if (!sle)
            return terNO_ACCOUNT;

        auto const balance = (*sle)[sfBalance];
        auto const reserve =
            ctx.view.fees().accountReserve((*sle)[sfOwnerCount] + 1);

        if (balance < reserve)
            return tecINSUFFICIENT_RESERVE;
    }

    return tesSUCCESS;
}

TER
BridgeCreate::doApply()
{
    auto const account = ctx_.tx[sfAccount];
    auto const bridge = ctx_.tx[sfXChainBridge];
    auto const reward = ctx_.tx[sfSignatureReward];
    auto const minAccountCreate = ctx_.tx[~sfMinAccountCreateAmount];

    auto const sleAcc = ctx_.view().peek(keylet::account(account));
    if (!sleAcc)
        return tecINTERNAL;

    Keylet const bridgeKeylet = keylet::bridge(bridge);
    auto const sleB = std::make_shared<SLE>(bridgeKeylet);

    (*sleB)[sfAccount] = account;
    (*sleB)[sfSignatureReward] = reward;
    if (minAccountCreate)
        (*sleB)[sfMinAccountCreateAmount] = *minAccountCreate;
    (*sleB)[sfXChainBridge] = bridge;
    (*sleB)[sfXChainClaimID] = 0;
    (*sleB)[sfXChainAccountCreateCount] = 0;
    (*sleB)[sfXChainAccountClaimCount] = 0;
    // TODO: Initialize the balance

    // Add to owner directory
    {
        auto const page = ctx_.view().dirInsert(
            keylet::ownerDir(account), bridgeKeylet, describeOwnerDir(account));
        if (!page)
            return tecDIR_FULL;
        (*sleB)[sfOwnerNode] = *page;
    }

    adjustOwnerCount(ctx_.view(), sleAcc, 1, ctx_.journal);

    ctx_.view().insert(sleB);
    ctx_.view().update(sleAcc);

    return tesSUCCESS;
}

//------------------------------------------------------------------------------

NotTEC
BridgeModify::preflight(PreflightContext const& ctx)
{
    if (!ctx.rules.enabled(featureXChainBridge))
        return temDISABLED;

    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    if (ctx.tx.getFlags() & tfUniversalMask)
        return temINVALID_FLAG;

    auto const account = ctx.tx[sfAccount];
    auto const reward = ctx.tx[~sfSignatureReward];
    auto const minAccountCreate = ctx.tx[~sfMinAccountCreateAmount];
    auto const bridge = ctx.tx[sfXChainBridge];

    if (!reward && !minAccountCreate)
    {
        // Must change something
        return temMALFORMED;
    }

    if (bridge.lockingChainDoor() != account &&
        bridge.issuingChainDoor() != account)
    {
        return temSIDECHAIN_NONDOOR_OWNER;
    }

    if (reward && (!isXRP(*reward) || reward->signum() <= 0))
    {
        return temXCHAIN_BRIDGE_BAD_REWARD_AMOUNT;
    }

    if (minAccountCreate &&
        (!isXRP(*minAccountCreate) || minAccountCreate->signum() <= 0))
    {
        return temXCHAIN_BRIDGE_BAD_MIN_ACCOUNT_CREATE_AMOUNT;
    }

    return preflight2(ctx);
}

TER
BridgeModify::preclaim(PreclaimContext const& ctx)
{
    auto const bridge = ctx.tx[sfXChainBridge];

    if (!ctx.view.read(keylet::bridge(bridge)))
    {
        // TODO: custom return code for no sidechain?
        return tecNO_ENTRY;
    }

    return tesSUCCESS;
}

TER
BridgeModify::doApply()
{
    auto const account = ctx_.tx[sfAccount];
    auto const bridge = ctx_.tx[sfXChainBridge];
    auto const reward = ctx_.tx[~sfSignatureReward];
    auto const minAccountCreate = ctx_.tx[~sfMinAccountCreateAmount];

    auto const sleAcc = ctx_.view().peek(keylet::account(account));
    if (!sleAcc)
        return tecINTERNAL;

    auto const sleB = ctx_.view().peek(keylet::bridge(bridge));
    if (!sleB)
        return tecINTERNAL;

    if (reward)
        (*sleB)[sfSignatureReward] = *reward;
    if (minAccountCreate)
    {
        // TODO: How do I modify minAccountCreate to clear it? With a flag?
        (*sleB)[sfMinAccountCreateAmount] = *minAccountCreate;
    }
    ctx_.view().update(sleB);

    return tesSUCCESS;
}

//------------------------------------------------------------------------------

NotTEC
XChainClaim::preflight(PreflightContext const& ctx)
{
    if (!ctx.rules.enabled(featureXChainBridge))
        return temDISABLED;

    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    if (ctx.tx.getFlags() & tfUniversalMask)
        return temINVALID_FLAG;

    STXChainBridge const bridgeSpec = ctx.tx[sfXChainBridge];
    auto const amount = ctx.tx[sfAmount];

    if (amount.signum() <= 0 ||
        amount.issue() != bridgeSpec.lockingChainIssue() ||
        amount.issue() != bridgeSpec.issuingChainIssue())
    {
        return temBAD_AMOUNT;
    }

    return preflight2(ctx);
}

TER
XChainClaim::preclaim(PreclaimContext const& ctx)
{
    AccountID const account = ctx.tx[sfAccount];
    STXChainBridge bridgeSpec = ctx.tx[sfXChainBridge];
    STAmount const& thisChainAmount = ctx.tx[sfAmount];
    auto const claimID = ctx.tx[sfXChainClaimID];

    auto const sleB = ctx.view.read(keylet::bridge(bridgeSpec));
    if (!sleB)
    {
        // TODO: custom return code for no sidechain?
        return tecNO_ENTRY;
    }

    if (!ctx.view.read(keylet::account(ctx.tx[sfDestination])))
    {
        return tecNO_DST;
    }

    auto const thisDoor = (*sleB)[sfAccount];
    bool isLockingChain = false;
    {
        if (thisDoor == bridgeSpec.lockingChainDoor())
            isLockingChain = true;
        else if (thisDoor == bridgeSpec.issuingChainDoor())
            isLockingChain = false;
        else
            return tecINTERNAL;
    }

    {
        // Check that the amount specified matches the expected issue

        if (isLockingChain)
        {
            if (bridgeSpec.lockingChainIssue() != thisChainAmount.issue())
                return tecBAD_XCHAIN_TRANSFER_ISSUE;
        }
        else
        {
            if (bridgeSpec.issuingChainIssue() != thisChainAmount.issue())
                return tecBAD_XCHAIN_TRANSFER_ISSUE;
        }
    }

    if (isXRP(bridgeSpec.lockingChainIssue()) !=
        isXRP(bridgeSpec.issuingChainIssue()))
    {
        // Should have been caught when creating the bridge
        // Detect here so `otherChainAmount` doesn't switch from IOU -> XRP
        // and the numeric issues that need to be addressed with that.
        return tecINTERNAL;
    }

    auto const otherChainAmount = [&]() -> STAmount {
        STAmount r(thisChainAmount);
        if (isLockingChain)
            r.setIssue(bridgeSpec.issuingChainIssue());
        else
            r.setIssue(bridgeSpec.lockingChainIssue());
        return r;
    }();

    auto const sleCID =
        ctx.view.read(keylet::xChainClaimID(bridgeSpec, claimID));
    {
        // Check that the sequence number is owned by the sender of this
        // transaction
        if (!sleCID)
        {
            return tecXCHAIN_NO_CLAIM_ID;
        }

        if ((*sleCID)[sfAccount] != account)
        {
            // Sequence number isn't owned by the sender of this transaction
            return tecXCHAIN_BAD_CLAIM_ID;
        }
    }

    // quorum is checked in `doApply`
    return tesSUCCESS;
}

TER
XChainClaim::doApply()
{
    PaymentSandbox psb(&ctx_.view());

    AccountID const account = ctx_.tx[sfAccount];
    auto const dst = ctx_.tx[sfDestination];
    STXChainBridge bridgeSpec = ctx_.tx[sfXChainBridge];
    STAmount const& thisChainAmount = ctx_.tx[sfAmount];
    auto const claimID = ctx_.tx[sfXChainClaimID];

    auto const sleAcc = psb.peek(keylet::account(account));
    auto const sleB = psb.peek(keylet::bridge(bridgeSpec));
    auto const sleCID = psb.peek(keylet::xChainClaimID(bridgeSpec, claimID));

    if (!(sleB && sleCID && sleAcc))
        return tecINTERNAL;

    AccountID const thisDoor = (*sleB)[sfAccount];
    bool isLockingChain = false;
    {
        if (thisDoor == bridgeSpec.lockingChainDoor())
            isLockingChain = true;
        else if (thisDoor == bridgeSpec.issuingChainDoor())
            isLockingChain = false;
        else
            return tecINTERNAL;
    }

    auto const sendingAmount = [&]() -> STAmount {
        STAmount r(thisChainAmount);
        if (isLockingChain)
            r.setIssue(bridgeSpec.issuingChainIssue());
        else
            r.setIssue(bridgeSpec.lockingChainIssue());
        return r;
    }();

    auto const wasLockingChainSend = !isLockingChain;

    auto const [signersList, quorum, slTer] =
        getSignersListAndQuorum(ctx_.view(), *sleB, ctx_.journal);

    if (!isTesSuccess(slTer))
        return slTer;

    XChainClaimAttestations curAtts{
        sleCID->getFieldArray(sfXChainClaimAttestations)};

    auto claimR = curAtts.onClaim(
        sendingAmount, wasLockingChainSend, quorum, signersList);
    if (!claimR.has_value())
        return claimR.error();

    auto const& rewardAccounts = claimR.value();
    auto const& rewardPoolSrc = (*sleCID)[sfAccount];

    std::optional<std::uint32_t> dstTag = ctx_.tx[~sfDestinationTag];

    auto const r = finalizeClaimHelper(
        psb,
        bridgeSpec,
        dst,
        dstTag,
        /*claimOwner*/ account,
        sendingAmount,
        rewardPoolSrc,
        (*sleCID)[sfSignatureReward],
        rewardAccounts,
        wasLockingChainSend,
        sleCID,
        ctx_.journal);
    if (!isTesSuccess(r))
        return r;

    psb.apply(ctx_.rawView());

    return tesSUCCESS;
}

//------------------------------------------------------------------------------

TxConsequences
XChainCommit::makeTxConsequences(PreflightContext const& ctx)
{
    auto const maxSpend = [&] {
        auto const amount = ctx.tx[sfAmount];
        if (amount.native() && amount.signum() > 0)
            return amount.xrp();
        return XRPAmount{beast::zero};
    }();

    return TxConsequences{ctx.tx, maxSpend};
}

NotTEC
XChainCommit::preflight(PreflightContext const& ctx)
{
    if (!ctx.rules.enabled(featureXChainBridge))
        return temDISABLED;

    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    if (ctx.tx.getFlags() & tfUniversalMask)
        return temINVALID_FLAG;

    auto const amount = ctx.tx[sfAmount];

    if (amount.signum() <= 0 || !isLegalNet(amount))
        return temBAD_AMOUNT;

    return preflight2(ctx);
}

TER
XChainCommit::preclaim(PreclaimContext const& ctx)
{
    auto const sidechain = ctx.tx[sfXChainBridge];
    auto const amount = ctx.tx[sfAmount];

    auto const sleB = ctx.view.read(keylet::bridge(sidechain));
    if (!sleB)
    {
        // TODO: custom return code for no sidechain?
        return tecNO_ENTRY;
    }

    auto const thisDoor = (*sleB)[sfAccount];

    bool isLockingChain = false;
    {
        if (thisDoor == sidechain.lockingChainDoor())
            isLockingChain = true;
        else if (thisDoor == sidechain.issuingChainDoor())
            isLockingChain = false;
        else
            return tecINTERNAL;
    }

    if (isLockingChain)
    {
        if (sidechain.lockingChainIssue() != ctx.tx[sfAmount].issue())
            return tecBAD_XCHAIN_TRANSFER_ISSUE;
    }
    else
    {
        if (sidechain.issuingChainIssue() != ctx.tx[sfAmount].issue())
            return tecBAD_XCHAIN_TRANSFER_ISSUE;
    }

    return tesSUCCESS;
}

TER
XChainCommit::doApply()
{
    PaymentSandbox psb(&ctx_.view());

    auto const account = ctx_.tx[sfAccount];
    auto const amount = ctx_.tx[sfAmount];
    auto const bridge = ctx_.tx[sfXChainBridge];

    auto const sle = psb.peek(keylet::account(account));
    if (!sle)
        return tecINTERNAL;

    auto const sleB = psb.read(keylet::bridge(bridge));
    if (!sleB)
        return tecINTERNAL;

    auto const dst = (*sleB)[sfAccount];

    auto const thTer = transferHelper(
        psb,
        account,
        dst,
        /*dstTag*/ std::nullopt,
        /*claimOwner*/ std::nullopt,
        amount,
        TransferHelperCanCreateDst::no,
        ctx_.journal);

    if (!isTesSuccess(thTer))
        return thTer;

    psb.apply(ctx_.rawView());

    return tesSUCCESS;
}

//------------------------------------------------------------------------------

NotTEC
XChainCreateClaimID::preflight(PreflightContext const& ctx)
{
    if (!ctx.rules.enabled(featureXChainBridge))
        return temDISABLED;

    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    if (ctx.tx.getFlags() & tfUniversalMask)
        return temINVALID_FLAG;

    auto const reward = ctx.tx[sfSignatureReward];

    if (!isXRP(reward) || reward.signum() < 0 || !isLegalNet(reward))
        return temXCHAIN_BRIDGE_BAD_REWARD_AMOUNT;

    return preflight2(ctx);
}

TER
XChainCreateClaimID::preclaim(PreclaimContext const& ctx)
{
    auto const account = ctx.tx[sfAccount];
    auto const bridgeSpec = ctx.tx[sfXChainBridge];
    auto const bridge = ctx.view.read(keylet::bridge(bridgeSpec));

    if (!bridge)
    {
        // TODO: custom return code for no sidechain?
        return tecNO_ENTRY;
    }

    // Check that the reward matches
    auto const reward = ctx.tx[sfSignatureReward];

    if (reward != (*bridge)[sfSignatureReward])
    {
        return tecXCHAIN_REWARD_MISMATCH;
    }

    {
        // Check reserve
        auto const sle = ctx.view.read(keylet::account(account));
        if (!sle)
            return terNO_ACCOUNT;

        auto const balance = (*sle)[sfBalance];
        auto const reserve =
            ctx.view.fees().accountReserve((*sle)[sfOwnerCount] + 1);

        if (balance < reserve)
            return tecINSUFFICIENT_RESERVE;
    }

    return tesSUCCESS;
}

TER
XChainCreateClaimID::doApply()
{
    auto const account = ctx_.tx[sfAccount];
    auto const bridge = ctx_.tx[sfXChainBridge];
    auto const reward = ctx_.tx[sfSignatureReward];
    auto const otherChainSrc = ctx_.tx[sfOtherChainSource];

    auto const sleAcc = ctx_.view().peek(keylet::account(account));
    if (!sleAcc)
        return tecINTERNAL;

    auto const sleB = ctx_.view().peek(keylet::bridge(bridge));
    if (!sleB)
        return tecINTERNAL;

    std::uint32_t const claimID = (*sleB)[sfXChainClaimID] + 1;
    if (claimID == 0)
        return tecINTERNAL;  // overflow

    (*sleB)[sfXChainClaimID] = claimID;

    Keylet const seqKeylet = keylet::xChainClaimID(bridge, claimID);
    if (ctx_.view().read(seqKeylet))
        return tecINTERNAL;  // already checked out!?!

    auto const sleQ = std::make_shared<SLE>(seqKeylet);

    (*sleQ)[sfAccount] = account;
    (*sleQ)[sfXChainBridge] = bridge;
    (*sleQ)[sfXChainClaimID] = claimID;
    (*sleQ)[sfOtherChainSource] = otherChainSrc;
    (*sleQ)[sfSignatureReward] = reward;
    sleQ->setFieldArray(
        sfXChainClaimAttestations, STArray{sfXChainClaimAttestations});

    // Add to owner directory
    {
        auto const page = ctx_.view().dirInsert(
            keylet::ownerDir(account), seqKeylet, describeOwnerDir(account));
        if (!page)
            return tecDIR_FULL;
        (*sleQ)[sfOwnerNode] = *page;
    }

    adjustOwnerCount(ctx_.view(), sleAcc, 1, ctx_.journal);

    ctx_.view().insert(sleQ);
    ctx_.view().update(sleB);
    ctx_.view().update(sleAcc);

    return tesSUCCESS;
}

//------------------------------------------------------------------------------

NotTEC
XChainAddAttestation::preflight(PreflightContext const& ctx)
{
    if (!ctx.rules.enabled(featureXChainBridge))
        return temDISABLED;

    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    if (ctx.tx.getFlags() & tfUniversalMask)
        return temINVALID_FLAG;

    STXChainAttestationBatch const batch = ctx.tx[sfXChainAttestationBatch];

    if (batch.numAttestations() > 8)
    {
        return temXCHAIN_TOO_MANY_ATTESTATIONS;
    }

    if (!batch.verify())
        return temBAD_XCHAIN_PROOF;

    if (!batch.noConflicts())
    {
        // TODO: return a better error here
        return temBAD_XCHAIN_PROOF;
    }

    if (!batch.validAmounts())
        return temBAD_XCHAIN_PROOF;

    auto const& bridgeSpec = batch.bridge();
    // If any attestation is for a negative amount or for an amount
    // that isn't expected by the given bridge, the whole transaction is bad
    auto checkAmount = [&](auto const& att) -> bool {
        if (att.sendingAmount.signum() <= 0)
            return false;
        auto const expectedIssue = att.wasLockingChainSend
            ? bridgeSpec.lockingChainIssue()
            : bridgeSpec.issuingChainIssue();
        if (att.sendingAmount.issue() != expectedIssue)
            return false;
        return true;
    };

    auto const& creates = batch.creates();
    auto const& claims = batch.claims();
    if (!(std::all_of(creates.begin(), creates.end(), checkAmount) &&
          std::all_of(claims.begin(), claims.end(), checkAmount)))
    {
        return temBAD_XCHAIN_PROOF;
    }
    return preflight2(ctx);
}

TER
XChainAddAttestation::preclaim(PreclaimContext const& ctx)
{
    // TBD
    return tesSUCCESS;
}

// Precondition: all the claims in the range are consistant. They must sign for
// the same event (amount, sending account, claim id, ect).
TER
XChainAddAttestation::applyClaims(
    STXChainAttestationBatch::TClaims::const_iterator attBegin,
    STXChainAttestationBatch::TClaims::const_iterator attEnd,
    STXChainBridge const& bridgeSpec,
    bool isLockingChain,
    std::unordered_map<AccountID, std::uint32_t> const& signersList,
    std::uint32_t quorum)
{
    if (attBegin == attEnd)
        return tesSUCCESS;

    PaymentSandbox psb(&ctx_.view());

    auto const sleCID =
        psb.peek(keylet::xChainClaimID(bridgeSpec, attBegin->claimID));
    if (!sleCID)
        return tecXCHAIN_NO_CLAIM_ID;
    AccountID const cidOwner = (*sleCID)[sfAccount];

    // Add claims that are part of the signer's list to the "claims" vector
    std::vector<AttestationBatch::AttestationClaim> atts;
    atts.reserve(std::distance(attBegin, attEnd));
    for (auto att = attBegin; att != attEnd; ++att)
    {
        if (!signersList.count(calcAccountID(att->publicKey)))
            continue;
        atts.push_back(*att);
    }

    if (atts.empty())
    {
        return tecXCHAIN_PROOF_UNKNOWN_KEY;
    }

    AccountID const otherChainSource = (*sleCID)[sfOtherChainSource];
    if (attBegin->sendingAccount != otherChainSource)
    {
        return tecXCHAIN_SENDING_ACCOUNT_MISMATCH;
    }
    if (attBegin->wasLockingChainSend == isLockingChain)
    {
        return tecXCHAIN_WRONG_CHAIN;
    }

    XChainClaimAttestations curAtts{
        sleCID->getFieldArray(sfXChainClaimAttestations)};

    auto const rewardAccounts = curAtts.onNewAttestations(
        &atts[0], &atts[0] + atts.size(), quorum, signersList);

    if (rewardAccounts && attBegin->dst)
    {
        auto const& rewardPoolSrc = (*sleCID)[sfAccount];
        auto const r = finalizeClaimHelper(
            psb,
            bridgeSpec,
            *attBegin->dst,
            /*dstTag*/ std::nullopt,
            cidOwner,
            attBegin->sendingAmount,
            rewardPoolSrc,
            (*sleCID)[sfSignatureReward],
            *rewardAccounts,
            attBegin->wasLockingChainSend,
            sleCID,
            ctx_.journal);
        if (!isTesSuccess(r))
            return r;
    }
    else
    {
        // update the claim id
        sleCID->setFieldArray(sfXChainClaimAttestations, curAtts.toSTArray());
        psb.update(sleCID);
    }

    psb.apply(ctx_.rawView());

    return tesSUCCESS;
}

TER
XChainAddAttestation::applyCreateAccountAtt(
    STXChainAttestationBatch::TCreates::const_iterator attBegin,
    STXChainAttestationBatch::TCreates::const_iterator attEnd,
    AccountID const& doorAccount,
    Keylet const& doorK,
    STXChainBridge const& bridgeSpec,
    Keylet const& bridgeK,
    bool isLockingChain,
    std::unordered_map<AccountID, std::uint32_t> const& signersList,
    std::uint32_t quorum)
{
    if (attBegin == attEnd)
        return tesSUCCESS;

    PaymentSandbox psb(&ctx_.view());

    auto const sleDoor = psb.peek(doorK);
    if (!sleDoor)
        return tecINTERNAL;

    auto const sleB = psb.peek(bridgeK);
    if (!sleB)
        return tecINTERNAL;

    std::int64_t const claimCount = (*sleB)[sfXChainAccountClaimCount];

    if (attBegin->createCount <= claimCount)
    {
        return tecXCHAIN_ACCOUNT_CREATE_PAST;
    }
    if (attBegin->createCount >= claimCount + 128)
    {
        // Limit the number of claims on the account
        return tecXCHAIN_ACCOUNT_CREATE_TOO_MANY;
    }

    if (attBegin->wasLockingChainSend == isLockingChain)
    {
        return tecXCHAIN_WRONG_CHAIN;
    }

    auto const claimKeylet =
        keylet::xChainCreateAccountClaimID(bridgeSpec, attBegin->createCount);

    // sleCID may be null. If it's null it isn't created until the end of this
    // function (if needed)
    auto const sleCID = psb.peek(claimKeylet);
    bool createCID = false;
    if (!sleCID)
    {
        createCID = true;

        // Check reserve
        auto const balance = (*sleDoor)[sfBalance];
        auto const reserve =
            psb.fees().accountReserve((*sleDoor)[sfOwnerCount] + 1);

        if (balance < reserve)
            return tecINSUFFICIENT_RESERVE;
    }

    std::vector<AttestationBatch::AttestationCreateAccount> atts;
    atts.reserve(std::distance(attBegin, attEnd));
    for (auto att = attBegin; att != attEnd; ++att)
    {
        if (!signersList.count(calcAccountID(att->publicKey)))
            continue;
        atts.push_back(*att);
    }
    if (atts.empty())
    {
        return tecXCHAIN_PROOF_UNKNOWN_KEY;
    }

    XChainCreateAccountAttestations curAtts = [&] {
        if (sleCID)
            return XChainCreateAccountAttestations{
                sleCID->getFieldArray(sfXChainCreateAccountAttestations)};
        return XChainCreateAccountAttestations{};
    }();

    auto const rewardAccounts = curAtts.onNewAttestations(
        &atts[0], &atts[0] + atts.size(), quorum, signersList);

    // Account create transactions must happen in order
    if (rewardAccounts && claimCount + 1 == attBegin->createCount)
    {
        auto const r = finalizeClaimHelper(
            psb,
            bridgeSpec,
            attBegin->toCreate,
            /*dstTag*/ std::nullopt,
            doorAccount,
            attBegin->sendingAmount,
            /*rewardPoolSrc*/ doorAccount,
            attBegin->rewardAmount,
            *rewardAccounts,
            attBegin->wasLockingChainSend,
            sleCID,
            ctx_.journal);
        if (!isTesSuccess(r))
            return r;
        (*sleB)[sfXChainAccountClaimCount] = attBegin->createCount;
        psb.update(sleB);
    }
    else
    {
        if (createCID)
        {
            if (sleCID)
                return tecINTERNAL;

            auto const sleCID = std::make_shared<SLE>(claimKeylet);
            (*sleCID)[sfAccount] = doorAccount;
            (*sleCID)[sfXChainBridge] = bridgeSpec;
            (*sleCID)[sfXChainAccountCreateCount] = attBegin->createCount;
            sleCID->setFieldArray(
                sfXChainCreateAccountAttestations, curAtts.toSTArray());

            // Add to owner directory of the door account
            auto const page = ctx_.view().dirInsert(
                keylet::ownerDir(doorAccount),
                claimKeylet,
                describeOwnerDir(doorAccount));
            if (!page)
                return tecDIR_FULL;
            (*sleCID)[sfOwnerNode] = *page;

            // Reserve was already checked
            adjustOwnerCount(psb, sleDoor, 1, ctx_.journal);
            psb.insert(sleCID);
            psb.update(sleDoor);
        }
        else
        {
            if (!sleCID)
                return tecINTERNAL;
            sleCID->setFieldArray(
                sfXChainCreateAccountAttestations, curAtts.toSTArray());
            psb.update(sleCID);
        }
    }

    psb.apply(ctx_.rawView());

    return tesSUCCESS;
}

TER
XChainAddAttestation::doApply()
{
    STXChainAttestationBatch const batch = ctx_.tx[sfXChainAttestationBatch];

    std::vector<TER> applyRestuls;
    applyRestuls.reserve(batch.numAttestations());

    auto const& bridgeSpec = batch.bridge();

    // TODO: sle's cannot overlap calls to applyCreateAccountAtt and
    // applyCreateAccountAtt because those functions create a sandbox
    // Reduce the scope of sleB
    auto const bridgeK = keylet::bridge(bridgeSpec);
    auto const sleB = ctx_.view().peek(bridgeK);
    if (!sleB)
    {
        // TODO: custom return code for no sidechain?
        return tecNO_ENTRY;
    }
    AccountID const thisDoor = (*sleB)[sfAccount];
    auto const doorK = keylet::account(thisDoor);
    bool isLockingChain = false;
    {
        if (thisDoor == bridgeSpec.lockingChainDoor())
            isLockingChain = true;
        else if (thisDoor == bridgeSpec.issuingChainDoor())
            isLockingChain = false;
        else
            return tecINTERNAL;
    }

    // signersList is a map from account id to weights
    auto const [signersList, quorum, slTer] =
        getSignersListAndQuorum(ctx_.view(), *sleB, ctx_.journal);

    if (!isTesSuccess(slTer))
        return slTer;

    {
        auto const claimResults =
            STXChainAttestationBatch::for_each_create_batch<TER>(
                batch.creates().begin(),
                batch.creates().end(),
                [&, &signersList = signersList, &quorum = quorum](
                    auto batchStart, auto batchEnd) {
                    return applyCreateAccountAtt(
                        batchStart,
                        batchEnd,
                        thisDoor,
                        doorK,
                        bridgeSpec,
                        bridgeK,
                        isLockingChain,
                        signersList,
                        quorum);
                });
        auto isTecInternal = [](auto r) { return r == tecINTERNAL; };
        if (std::any_of(
                claimResults.begin(), claimResults.end(), isTecInternal))
            return tecINTERNAL;

        applyRestuls.insert(
            applyRestuls.end(), claimResults.begin(), claimResults.end());
    }

    {
        auto const claimResults =
            STXChainAttestationBatch::for_each_claim_batch<TER>(
                batch.claims().begin(),
                batch.claims().end(),
                [&, &signersList = signersList, &quorum = quorum](
                    auto batchStart, auto batchEnd) {
                    return applyClaims(
                        batchStart,
                        batchEnd,
                        bridgeSpec,
                        isLockingChain,
                        signersList,
                        quorum);
                });
        auto isTecInternal = [](auto r) { return r == tecINTERNAL; };
        if (std::any_of(
                claimResults.begin(), claimResults.end(), isTecInternal))
            return tecINTERNAL;

        applyRestuls.insert(
            applyRestuls.end(), claimResults.begin(), claimResults.end());
    }

    if (applyRestuls.size() == 1)
        return applyRestuls[0];

    if (std::any_of(applyRestuls.begin(), applyRestuls.end(), isTesSuccess))
        return tesSUCCESS;

    // TODO: What to return here?
    return applyRestuls[0];
}

//------------------------------------------------------------------------------

NotTEC
XChainCreateAccount::preflight(PreflightContext const& ctx)
{
    if (!ctx.rules.enabled(featureXChainBridge))
        return temDISABLED;

    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    if (ctx.tx.getFlags() & tfUniversalMask)
        return temINVALID_FLAG;

    auto const amount = ctx.tx[sfAmount];

    if (amount.signum() <= 0 || !amount.native())
        return temBAD_AMOUNT;

    auto const reward = ctx.tx[sfSignatureReward];
    if (reward.signum() <= 0 || !reward.native())
        return temBAD_AMOUNT;

    if (reward.issue() != amount.issue())
        return temBAD_AMOUNT;

    return preflight2(ctx);
}

TER
XChainCreateAccount::preclaim(PreclaimContext const& ctx)
{
    STXChainBridge const bridgeSpec = ctx.tx[sfXChainBridge];
    STAmount const amount = ctx.tx[sfAmount];
    STAmount const reward = ctx.tx[sfSignatureReward];

    auto const sleB = ctx.view.read(keylet::bridge(bridgeSpec));
    if (!sleB)
    {
        // TODO: custom return code for no sidechain?
        return tecNO_ENTRY;
    }

    if (reward != (*sleB)[sfSignatureReward])
    {
        return tecXCHAIN_REWARD_MISMATCH;
    }

    std::optional<STAmount> minCreateAmount =
        (*sleB)[~sfMinAccountCreateAmount];

    if (!minCreateAmount || amount < *minCreateAmount)
    {
        return tecXCHAIN_INSUFF_CREATE_AMOUNT;
    }

    if (minCreateAmount->issue() != amount.issue())
        return tecBAD_XCHAIN_TRANSFER_ISSUE;

    AccountID const thisDoor = (*sleB)[sfAccount];
    bool isLockingChain = false;
    {
        if (thisDoor == bridgeSpec.lockingChainDoor())
            isLockingChain = true;
        else if (thisDoor == bridgeSpec.issuingChainDoor())
            isLockingChain = false;
        else
            return tecINTERNAL;
    }

    if (isLockingChain)
    {
        if (bridgeSpec.lockingChainIssue() != ctx.tx[sfAmount].issue())
            return tecBAD_XCHAIN_TRANSFER_ISSUE;

        if (!isXRP(bridgeSpec.issuingChainIssue()))
            return tecXCHAIN_CREATE_ACCOUNT_NONXRP_ISSUE;
    }
    else
    {
        if (bridgeSpec.issuingChainIssue() != ctx.tx[sfAmount].issue())
            return tecBAD_XCHAIN_TRANSFER_ISSUE;

        if (!isXRP(bridgeSpec.lockingChainIssue()))
            return tecXCHAIN_CREATE_ACCOUNT_NONXRP_ISSUE;
    }

    return tesSUCCESS;
}

TER
XChainCreateAccount::doApply()
{
    PaymentSandbox psb(&ctx_.view());

    AccountID const account = ctx_.tx[sfAccount];
    STAmount const amount = ctx_.tx[sfAmount];
    STAmount const reward = ctx_.tx[sfSignatureReward];
    STXChainBridge const bridge = ctx_.tx[sfXChainBridge];

    auto const sle = psb.peek(keylet::account(account));
    if (!sle)
        return tecINTERNAL;

    auto const sleB = psb.peek(keylet::bridge(bridge));
    if (!sleB)
        return tecINTERNAL;

    auto const dst = (*sleB)[sfAccount];

    STAmount const toTransfer = amount + reward;
    auto const thTer = transferHelper(
        psb,
        account,
        dst,
        /*dstTag*/ std::nullopt,
        /*claimOwner*/ std::nullopt,
        toTransfer,
        TransferHelperCanCreateDst::yes,
        ctx_.journal);

    if (!isTesSuccess(thTer))
        return thTer;

    (*sleB)[sfXChainAccountCreateCount] =
        (*sleB)[sfXChainAccountCreateCount] + 1;
    psb.update(sleB);

    psb.apply(ctx_.rawView());

    return tesSUCCESS;
}

//------------------------------------------------------------------------------

}  // namespace ripple
