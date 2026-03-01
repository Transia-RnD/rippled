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

#include <xrpl/tx/transactors/Option/LeverageTierSet.h>
#include <xrpl/ledger/Sandbox.h>
#include <xrpl/ledger/View.h>

#include <xrpl/basics/Log.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/STIssue.h>
#include <xrpl/protocol/TxFlags.h>

namespace xrpl {

NotTEC
LeverageTierSet::preflight(PreflightContext const& ctx)
{
    if (ctx.tx.getFlags() & tfUniversalMask)
    {
        JLOG(ctx.j.debug()) << "LeverageTierSet: invalid flags.";
        return temINVALID_FLAG;
    }

    // Validate assets are different
    Issue const issue = ctx.tx[sfAsset].get<Issue>();
    Issue const issue2 = ctx.tx[sfAsset2].get<Issue>();

    if (issue == issue2)
    {
        JLOG(ctx.j.debug()) << "LeverageTierSet: assets must be different.";
        return temMALFORMED;
    }

    // Validate leverage range [2..200]
    std::uint32_t const maxLeverage = ctx.tx[sfMaxLeverage];
    if (maxLeverage < 2 || maxLeverage > 200)
    {
        JLOG(ctx.j.debug())
            << "LeverageTierSet: maxLeverage must be between 2 and 200.";
        return temMALFORMED;
    }

    // Validate margin rates are positive
    std::uint32_t const initialMarginBps = ctx.tx[sfInitialMarginBps];
    std::uint32_t const maintenanceMarginBps = ctx.tx[sfMaintenanceMarginBps];
    if (initialMarginBps == 0 || maintenanceMarginBps == 0)
    {
        JLOG(ctx.j.debug())
            << "LeverageTierSet: margin rates must be positive.";
        return temMALFORMED;
    }

    // Maintenance must be less than initial
    if (maintenanceMarginBps >= initialMarginBps)
    {
        JLOG(ctx.j.debug()) << "LeverageTierSet: maintenance margin must be "
                               "less than initial margin.";
        return temMALFORMED;
    }

    // Validate liquidation bonus range [100..5000] (1% to 50%)
    std::uint32_t const liquidationBonusBps = ctx.tx[sfLiquidationBonusBps];
    if (liquidationBonusBps < 100 || liquidationBonusBps > 5000)
    {
        JLOG(ctx.j.debug()) << "LeverageTierSet: liquidation bonus must be "
                               "between 100 and 5000 (1%-50%).";
        return temMALFORMED;
    }

    return tesSUCCESS;
}

TER
LeverageTierSet::preclaim(PreclaimContext const& ctx)
{
    auto const accountID = ctx.tx[sfAccount];
    Issue const issue = ctx.tx[sfAsset].get<Issue>();
    Issue const issue2 = ctx.tx[sfAsset2].get<Issue>();

    // Verify OptionPair exists for this asset pair
    auto const optionPairKeylet = keylet::optionPair(issue, issue2);
    if (!ctx.view.read(optionPairKeylet))
    {
        JLOG(ctx.j.debug())
            << "LeverageTierSet: OptionPair does not exist for this asset pair.";
        return tecNO_ENTRY;
    }

    // Verify the submitter is the issuer of one of the assets (admin gate)
    if (accountID != issue.account && accountID != issue2.account)
    {
        JLOG(ctx.j.debug())
            << "LeverageTierSet: only asset issuers can set leverage tiers.";
        return tecNO_PERMISSION;
    }

    return tesSUCCESS;
}

TER
LeverageTierSet::doApply()
{
    Sandbox sb(&ctx_.view());

    Issue const issue = ctx_.tx[sfAsset].get<Issue>();
    Issue const issue2 = ctx_.tx[sfAsset2].get<Issue>();

    auto const tierKeylet = keylet::leverageTier(issue, issue2);

    // Check if we're creating or updating
    auto sleTier = sb.peek(tierKeylet);
    bool const isCreate = !sleTier;

    if (isCreate)
    {
        // Create new leverage tier
        sleTier = std::make_shared<SLE>(tierKeylet);
        sleTier->setAccountID(sfOwner, account_);
        sleTier->setFieldIssue(sfAsset, STIssue{sfAsset, issue});
        sleTier->setFieldIssue(sfAsset2, STIssue{sfAsset2, issue2});

        // Add to owner directory
        auto const page = sb.dirInsert(
            keylet::ownerDir(account_),
            tierKeylet,
            describeOwnerDir(account_));
        if (!page)
        {
            JLOG(j_.debug())
                << "LeverageTierSet: failed to insert owner dir.";
            return tecDIR_FULL;
        }
        sleTier->setFieldU64(sfOwnerNode, *page);
    }

    // Set/update fields
    sleTier->setFieldU32(sfMaxLeverage, ctx_.tx[sfMaxLeverage]);
    sleTier->setFieldU32(sfInitialMarginBps, ctx_.tx[sfInitialMarginBps]);
    sleTier->setFieldU32(
        sfMaintenanceMarginBps, ctx_.tx[sfMaintenanceMarginBps]);
    sleTier->setFieldU32(sfLiquidationBonusBps, ctx_.tx[sfLiquidationBonusBps]);

    // Optional: tiered leverage array for position-size-based tiers
    if (ctx_.tx.isFieldPresent(sfLeverageTiers))
    {
        sleTier->setFieldArray(sfLeverageTiers, ctx_.tx.getFieldArray(sfLeverageTiers));
    }

    if (isCreate)
    {
        sb.insert(sleTier);
        adjustOwnerCount(sb, sb.peek(keylet::account(account_)), 1, j_);
    }
    else
    {
        sb.update(sleTier);
    }

    sb.apply(ctx_.rawView());
    return tesSUCCESS;
}

}  // namespace xrpl
