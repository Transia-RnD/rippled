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

#include <xrpld/app/tx/detail/OptionSettle.h>
#include <xrpld/app/tx/detail/OptionUtils.h>
#include <xrpld/ledger/Sandbox.h>
#include <xrpld/ledger/View.h>

#include <xrpl/basics/Log.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Option.h>
#include <xrpl/protocol/STNumber.h>
#include <xrpl/protocol/TxFlags.h>

namespace ripple {

NotTEC
OptionSettle::preflight(PreflightContext const& ctx)
{
    if (!ctx.rules.enabled(featureOptions))
        return temDISABLED;

    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    std::uint32_t const flags = ctx.tx.getFlags();
    if (flags & tfOptionSettleMask)
    {
        JLOG(ctx.j.warn()) << "OptionSettle: Invalid flags set.";
        return temINVALID_FLAG;
    }

    if (std::popcount(flags & (tfExpire | tfClose | tfExercise)) != 1)
    {
        JLOG(ctx.j.trace()) << "OptionSettle: Invalid flags set.";
        return temINVALID_FLAG;
    }

    return preflight2(ctx);
}

TER
OptionSettle::preclaim(PreclaimContext const& ctx)
{
    uint256 const optionID = ctx.tx.getFieldH256(sfOptionID);
    if (!ctx.view.exists(ripple::keylet::unchecked(optionID)))
        return tecNO_ENTRY;

    uint256 const offerID = ctx.tx.getFieldH256(sfOptionOfferID);
    auto const sleOffer = ctx.view.read(keylet::unchecked(offerID));
    if (!sleOffer)
    {
        JLOG(ctx.j.trace()) << "OptionSettle: Option offer not found.";
        return tecNO_TARGET;
    }

    auto const flags = ctx.tx.getFlags();
    if (!(flags & (tfClose | tfExpire)) && (sleOffer->getFlags() & tfSell))
    {
        JLOG(ctx.j.trace()) << "OptionSettle: Option offer is a sell offer.";
        return tecNO_PERMISSION;
    }

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
    Sandbox sb(&ctx_.view());

    auto sleAccount = sb.peek(keylet::account(account_));
    if (!sleAccount)
        return tecINTERNAL;

    auto offerKeylet =
        keylet::optionOffer(ctx_.tx.getFieldH256(sfOptionOfferID));
    auto sleOffer = sb.peek(offerKeylet);
    if (!sleOffer)
        return tecINTERNAL;

    auto sleOption =
        sb.read(keylet::unchecked(ctx_.tx.getFieldH256(sfOptionID)));
    if (!sleOption)
        return tecINTERNAL;

    // Check for expiration or explicit expire flag
    auto const flags = ctx_.tx.getFlags();
    if (hasExpired(sb, sleOffer->getFieldU32(sfExpiration)) ||
        (flags & tfExpire))
    {
        JLOG(j_.trace()) << "OptionSettle: Expire offer.";
        if (auto const ter = option::expireOffer(sb, sleOffer, j_);
            ter != tesSUCCESS)
            return ter;
        sb.apply(ctx_.rawView());
        return tecEXPIRED;
    }

    STArray const sealedOptions = sleOffer->getFieldArray(sfSealedOptions);
    if (sealedOptions.size() == 0)
    {
        if (auto const ter = option::deleteOffer(sb, sleOffer, j_);
            ter != tesSUCCESS)
            return ter;
        sb.apply(ctx_.rawView());
        return tesSUCCESS;
    }

    auto const optionFlags = sleOffer->getFlags();
    bool const isPut = optionFlags & tfPut;
    bool const isSell = optionFlags & tfSell;
    auto const issue = (*sleOption)[sfAsset].get<Issue>();
    STAmount const strikePrice = sleOption->getFieldAmount(sfStrikePrice);
    std::int64_t const strike = static_cast<std::int64_t>(Number(strikePrice));
    std::uint32_t expiration = sleOffer->getFieldU32(sfExpiration);

    // close the option
    if (flags & tfClose)
    {
        JLOG(j_.trace()) << "OptionSettle: Close offer.";
        auto const ter = option::closeOffer(
            sb,
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

        sb.update(sleAccount);
        sb.apply(ctx_.rawView());
        return tesSUCCESS;
    }

    // exercise the option
    JLOG(j_.trace()) << "OptionSettle: Exercise offer.";
    if (auto const ter = option::exerciseOffer(
            sb,
            isPut,
            strikePrice,
            account_,
            sleAccount,
            issue,
            sealedOptions,
            j_);
        ter != tesSUCCESS)
        return ter;

    if (auto const ter = option::deleteOffer(sb, sleOffer, j_);
        ter != tesSUCCESS)
        return ter;
    sb.apply(ctx_.rawView());
    return tesSUCCESS;
}

}  // namespace ripple
