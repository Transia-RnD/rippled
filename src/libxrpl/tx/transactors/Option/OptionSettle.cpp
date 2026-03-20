//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2025 Ripple Labs Inc.

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

#include <xrpl/tx/transactors/Option/OptionSettle.h>
#include <xrpl/tx/transactors/Option/OptionUtils.h>
#include <xrpl/tx/transactors/Option/MarginUtils.h>
#include <xrpl/ledger/Sandbox.h>
#include <xrpl/ledger/View.h>

#include <xrpl/basics/Log.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Option.h>
#include <xrpl/protocol/STNumber.h>
#include <xrpl/protocol/TxFlags.h>

namespace xrpl {

NotTEC
OptionSettle::preflight(PreflightContext const& ctx)
{
    // Verify that exactly one of the three action flags is set:
    // - tfExpire: Expire the option
    // - tfClose: Close the option
    // - tfExercise: Exercise the option
    std::uint32_t const flags = ctx.tx.getFlags();
    if (std::popcount(flags & (tfExpire | tfClose | tfExercise)) != 1)
    {
        JLOG(ctx.j.trace()) << "OptionSettle: Invalid flags set.";
        return temINVALID_FLAG;
    }

    return tesSUCCESS;
}

TER
OptionSettle::preclaim(PreclaimContext const& ctx)
{
    // Get the option ID from the transaction
    uint256 const optionID = ctx.tx.getFieldH256(sfOptionID);

    // Verify the option exists in the ledger
    if (!ctx.view.exists(keylet::unchecked(optionID)))
        return tecNO_ENTRY;

    // Get the option offer ID from the transaction
    uint256 const offerID = ctx.tx.getFieldH256(sfOptionOfferID);

    // Load the option offer from the ledger
    auto const sleOffer = ctx.view.read(keylet::unchecked(offerID));
    if (!sleOffer)
    {
        JLOG(ctx.j.trace()) << "OptionSettle: Option offer not found.";
        return tecNO_TARGET;
    }

    // Verify that the account submitting the transaction is the owner of the
    // offer
    if (sleOffer->getAccountID(sfOwner) != ctx.tx.getAccountID(sfAccount))
    {
        JLOG(ctx.j.trace())
            << "OptionSettle: Option offer not owned by account.";
        return tecNO_PERMISSION;
    }

    return tesSUCCESS;
}

TER
OptionSettle::doApply()
{
    // Create a sandbox view to apply changes
    Sandbox sb(&ctx_.view());

    // Get the account SLE of the transaction submitter
    auto sleAccount = sb.peek(keylet::account(account_));
    if (!sleAccount)
        return tecINTERNAL;

    // Get the option offer keylet and SLE
    auto offerKeylet =
        keylet::optionOffer(ctx_.tx.getFieldH256(sfOptionOfferID));
    auto sleOffer = sb.peek(offerKeylet);
    if (!sleOffer)
        return tecINTERNAL;

    // Get the option definition SLE
    auto sleOption =
        sb.read(keylet::unchecked(ctx_.tx.getFieldH256(sfOptionID)));
    if (!sleOption)
        return tecINTERNAL;

    // Get the transaction flags
    auto const flags = ctx_.tx.getFlags();

    // Handle expiration - either natural expiration or explicit expire flag
    if (hasExpired(sb, sleOffer->getFieldU32(sfExpiration)) ||
        (flags & tfExpire))
    {
        JLOG(j_.trace()) << "OptionSettle: Expire offer.";

        // Call utility function to handle the expiration
        if (auto const ter = option::expireOffer(sb, sleOffer, j_);
            ter != tesSUCCESS)
            return ter;

        // Apply the changes to the ledger
        sb.apply(ctx_.rawView());

        // Return expired status
        return tecEXPIRED;
    }

    // Get the sealed options array from the offer
    STArray const sealedOptions = sleOffer->getFieldArray(sfSealedOptions);

    // If there are no sealed options, simply delete the offer
    if (sealedOptions.size() == 0)
    {
        if (auto const ter = option::deleteOffer(sb, sleOffer, j_);
            ter != tesSUCCESS)
            return ter;

        // Apply the changes to the ledger
        sb.apply(ctx_.rawView());
        return tesSUCCESS;
    }

    // Extract option properties needed for processing
    auto const optionFlags = sleOffer->getFlags();
    bool const isPut = optionFlags & tfPut;    // Is this a put option?
    bool const isSell = optionFlags & tfSell;  // Is this a sell offer?
    auto const issue =
        (*sleOption)[sfAsset].get<Issue>();  // The underlying asset
    STAmount const strikePrice =
        sleOption->getFieldAmount(sfStrikePrice);  // Strike price
    std::int64_t const strike =
        static_cast<std::int64_t>(Number(strikePrice));  // Strike as integer
    std::uint32_t expiration =
        sleOffer->getFieldU32(sfExpiration);  // Expiration time

    // Get the option pair
    auto const optionPairKeylet =
        keylet::optionPair(issue, strikePrice.issue());
    auto const slePair = sb.peek(optionPairKeylet);
    if (!slePair)
        return tecINTERNAL;
    auto const pseudoAccount = slePair->getAccountID(sfAccount);

    // Handle option closing
    if (flags & tfClose)
    {
        JLOG(j_.trace()) << "OptionSettle: Close offer.";

        // Call utility function to handle the closing process
        auto const ter = option::closeOffer(
            sb,
            pseudoAccount,
            account_,
            offerKeylet,
            isPut,
            isSell,
            issue,
            strike,
            expiration,
            j_);

        if (ter != tesSUCCESS)
            return ter;

        // Update the account in the ledger
        sb.update(sleAccount);

        // Apply the changes to the ledger
        sb.apply(ctx_.rawView());
        return tesSUCCESS;
    }

    // If not closing or expiring, we're exercising the option
    JLOG(j_.trace()) << "OptionSettle: Exercise offer.";

    // Call utility function to handle cash-settled exercise
    if (auto const ter = option::exerciseOffer(
            sb,
            isPut,
            strikePrice,
            account_,
            issue,
            strikePrice.issue(),  // quote (settlement) asset
            sealedOptions,
            j_);
        ter != tesSUCCESS)
        return ter;

    // Release buyer's margin position if linked
    if (sleOffer->isFieldPresent(sfMarginPositionID))
    {
        uint256 const positionID = sleOffer->getFieldH256(sfMarginPositionID);
        auto slePosition = sb.peek(Keylet{ltMARGIN_POSITION, positionID});
        if (slePosition)
        {
            // Deduct accumulated funding and get net margin
            std::uint32_t const nowSettle =
                sb.parentCloseTime().time_since_epoch().count();
            Number const netMargin =
                margin::deductFundingAndRelease(sb, slePosition, nowSettle);

            // Release net margin back to margin account
            uint256 const marginAccountID =
                slePosition->getFieldH256(sfMarginAccountID);
            auto sleMarginAcct =
                sb.peek(Keylet{ltMARGIN_ACCOUNT, marginAccountID});
            if (sleMarginAcct)
            {
                Number collateralBalance =
                    sleMarginAcct->at(~sfCollateralBalance).value_or(Number(0));
                collateralBalance = collateralBalance + netMargin;
                sleMarginAcct->at(sfCollateralBalance) =
                    STNumber{sfCollateralBalance, collateralBalance};
                sb.update(sleMarginAcct);
            }

            // Delete the margin position
            auto const posAccount = slePosition->getAccountID(sfAccount);
            if (slePosition->isFieldPresent(sfOwnerNode))
            {
                sb.dirRemove(
                    keylet::ownerDir(posAccount),
                    slePosition->getFieldU64(sfOwnerNode),
                    slePosition->key(),
                    true);
            }
            adjustOwnerCount(
                sb, sb.peek(keylet::account(posAccount)), -1, j_);
            sb.erase(slePosition);
        }
    }

    // Delete the offer after successful exercise
    if (auto const ter = option::deleteOffer(sb, sleOffer, j_);
        ter != tesSUCCESS)
        return ter;

    // Apply the changes to the ledger
    sb.apply(ctx_.rawView());
    return tesSUCCESS;
}

}  // namespace xrpl