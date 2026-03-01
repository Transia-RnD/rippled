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

#include <xrpl/ledger/OrderBookDB.h>
#include <xrpl/tx/transactors/Option/OptionCreate.h>
#include <xrpl/tx/transactors/Option/OptionUtils.h>
#include <xrpl/tx/transactors/Option/MarginUtils.h>
#include <xrpl/ledger/Sandbox.h>
#include <xrpl/ledger/View.h>

#include <xrpl/basics/Log.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/Option.h>
#include <xrpl/protocol/STNumber.h>
#include <xrpl/protocol/TxFlags.h>

namespace xrpl {

NotTEC
OptionCreate::preflight(PreflightContext const& ctx)
{
    // Verify quantity is valid (must be divisible by 100)
    std::uint32_t const quantity = ctx.tx[sfQuantity];
    if (quantity % 100)
    {
        JLOG(ctx.j.trace()) << "OptionCreate: Invalid quantity.";
        return temMALFORMED;
    }

    // MarginAccountID and Leverage are required for all offers
    if (!ctx.tx.isFieldPresent(sfMarginAccountID) ||
        !ctx.tx.isFieldPresent(sfLeverage))
    {
        JLOG(ctx.j.trace())
            << "OptionCreate: MarginAccountID and Leverage are required.";
        return temMALFORMED;
    }

    std::uint32_t const leverage = ctx.tx[sfLeverage];
    if (leverage < 2 || leverage > 200)
    {
        JLOG(ctx.j.trace())
            << "OptionCreate: Leverage must be between 2 and 200.";
        return temMALFORMED;
    }

    return tesSUCCESS;
}

TER
OptionCreate::preclaim(PreclaimContext const& ctx)
{
    // Validate margin account and leverage tier (required for all offers)
    auto const accountID = ctx.tx[sfAccount];
    uint256 const marginAccountID = ctx.tx[sfMarginAccountID];
    std::uint32_t const leverage = ctx.tx[sfLeverage];

    // Verify margin account exists
    auto const sleMarginAcct =
        ctx.view.read(Keylet{ltMARGIN_ACCOUNT, marginAccountID});
    if (!sleMarginAcct)
    {
        JLOG(ctx.j.debug())
            << "OptionCreate: margin account does not exist.";
        return tecNO_ENTRY;
    }

    // Verify the caller owns this margin account
    if (sleMarginAcct->getAccountID(sfAccount) != accountID)
    {
        JLOG(ctx.j.debug())
            << "OptionCreate: account does not own this margin account.";
        return tecNO_PERMISSION;
    }

    auto const flags = ctx.tx.getFlags();

    // Look up the leverage tier for this asset pair
    Asset const asset = ctx.tx[sfAsset].get<Issue>();
    Issue const issue = asset.get<Issue>();
    STAmount const strikePrice = ctx.tx[sfStrikePrice];

    auto const sleTier =
        ctx.view.read(keylet::leverageTier(issue, strikePrice.issue()));
    if (!sleTier)
    {
        JLOG(ctx.j.debug())
            << "OptionCreate: no leverage tier for this asset pair.";
        return tecNO_ENTRY;
    }

    // Verify leverage does not exceed max
    std::uint32_t const maxLeverage = sleTier->getFieldU32(sfMaxLeverage);
    if (leverage > maxLeverage)
    {
        JLOG(ctx.j.debug())
            << "OptionCreate: leverage " << leverage
            << " exceeds max " << maxLeverage;
        return tecNO_PERMISSION;
    }

    // Verify collateral asset matches margin account's collateral
    Issue const collateralIssue =
        sleMarginAcct->getFieldIssue(sfCollateralAsset).get<Issue>();
    if (collateralIssue != strikePrice.issue())
    {
        JLOG(ctx.j.debug())
            << "OptionCreate: collateral asset mismatch.";
        return temMALFORMED;
    }

    // Calculate notional value and required initial margin
    std::uint32_t const quantity = ctx.tx[sfQuantity];
    bool const isPut = flags & tfPut;

    Number notional;
    if (isPut)
    {
        // For puts, notional = strikePrice * quantity
        notional = Number(strikePrice) * Number(quantity);
    }
    else
    {
        // For calls, notional = quantity (in underlying asset terms)
        // The collateral is the underlying asset value
        notional = Number(strikePrice) * Number(quantity);
    }

    Number const initialMargin =
        margin::calculateInitialMargin(notional, leverage);

    // Verify sufficient collateral balance
    Number const collateralBalance =
        sleMarginAcct->at(~sfCollateralBalance).value_or(Number(0));
    if (collateralBalance < initialMargin)
    {
        JLOG(ctx.j.debug())
            << "OptionCreate: insufficient margin collateral.";
        return tecUNFUNDED_PAYMENT;
    }

    return tesSUCCESS;
}

TER
OptionCreate::doApply()
{
    // Create sandbox view for transaction application
    Sandbox sb(&ctx_.view());

    // Extract option parameters from transaction
    auto const flags = ctx_.tx.getFlags();
    std::uint32_t const expiration = ctx_.tx[sfExpiration];
    STAmount const strikePrice = ctx_.tx[sfStrikePrice];
    std::int64_t const strike = static_cast<std::int64_t>(Number(strikePrice));
    Asset const asset = ctx_.tx[sfAsset].get<Issue>();
    Issue const issue = asset.get<Issue>();
    STAmount const premium = ctx_.tx.getFieldAmount(sfPremium);
    std::uint32_t const quantity = ctx_.tx.getFieldU32(sfQuantity);

    // Verify source account exists
    auto sleSource = sb.peek(keylet::account(account_));
    if (!sleSource)
        return terNO_ACCOUNT;

    // Verify OptionPair exists (peek for mutable access to update fees)
    auto slePseudo = sb.peek(keylet::optionPair(issue, strikePrice.issue()));
    if (!slePseudo)
    {
        JLOG(j_.trace()) << "OptionCreate: OptionPair does not exist.";
        return tecNO_ENTRY;
    }

    auto const pseudoAccount = slePseudo->getAccountID(sfAccount);

    // Get trading fee rate from the OptionPair (0 if not set)
    std::uint32_t const tradingFeeBps =
        slePseudo->at(~sfTradingFeeBps).value_or(0);

    // Determine option type flags
    bool const isPut = flags & tfPut;
    bool const isMarket = flags & tfMarket;
    bool const isSell = flags & tfSell;

    // Generate keylets for option book and offer
    auto optionBookDirKeylet =
        keylet::optionBook(issue.account, issue.currency, strike, expiration);
    auto optionOfferKeylet =
        keylet::optionOffer(account_, ctx_.tx.getSeqProxy().value());

    // Seal option against matching orders in the book
    std::vector<option::SealedOptionData> sealedOptions = option::matchOptions(
        sb,
        issue,
        strike,
        expiration,
        isPut,
        isSell,
        quantity,
        account_,
        optionOfferKeylet.key,
        isMarket,
        premium);

    JLOG(j_.trace()) << "OptionCreate: Sealed Options: "
                     << sealedOptions.size();

    // Calculate total quantity that was matched/sealed
    std::uint32_t totalSealedQuantity = 0;
    for (const auto& sealedOption : sealedOptions)
    {
        totalSealedQuantity += sealedOption.quantitySealed;
    }

    // Calculate remaining open interest
    std::uint32_t openInterest = quantity - totalSealedQuantity;

    // Calculate premium for the open portion
    STAmount const openPremium = mulRound(
        premium,
        STAmount(premium.issue(), openInterest),
        premium.issue(),
        false);

    // Track total fees collected in this transaction
    Number totalFeesCollected(0);

    // --- Margin mode (required for all offers) ---
    {
        uint256 const marginAccountID = ctx_.tx[sfMarginAccountID];
        std::uint32_t const leverage = ctx_.tx[sfLeverage];

        auto sleMarginAcct =
            sb.peek(Keylet{ltMARGIN_ACCOUNT, marginAccountID});
        if (!sleMarginAcct)
            return tecNO_ENTRY;

        // Calculate notional and initial margin
        Number notional = Number(strikePrice) * Number(quantity);
        Number const initialMargin =
            margin::calculateInitialMargin(notional, leverage);

        // Deduct initial margin from collateral balance
        Number collateralBalance =
            sleMarginAcct->at(~sfCollateralBalance).value_or(Number(0));
        if (collateralBalance < initialMargin)
            return tecUNFUNDED_PAYMENT;

        collateralBalance = collateralBalance - initialMargin;
        sleMarginAcct->at(sfCollateralBalance) =
            STNumber{sfCollateralBalance, collateralBalance};

        // Look up leverage tier for maintenance margin rate
        auto const sleTier =
            sb.read(keylet::leverageTier(issue, strikePrice.issue()));
        std::uint32_t maintenanceMarginBps = 5000;  // default 5%
        if (sleTier)
            maintenanceMarginBps =
                sleTier->getFieldU32(sfMaintenanceMarginBps);

        // Calculate entry price and liquidation price
        Number const entryPrice = Number(strikePrice);
        // Buyers are long (0), sellers are short (1)
        std::uint32_t const positionSide = isSell ? 1 : 0;
        Number const liquidationPrice =
            margin::calculateLiquidationPrice(
                entryPrice, positionSide, leverage, maintenanceMarginBps);

        // Find the OptionPair to get the optionPairID
        auto const slePairForPos =
            sb.read(keylet::optionPair(issue, strikePrice.issue()));
        uint256 const optionPairID =
            slePairForPos ? slePairForPos->key() : uint256{};

        // Create ltMARGIN_POSITION
        auto const positionKeylet =
            keylet::marginPosition(account_, ctx_.tx.getSeqProxy().value());
        auto slePosition = std::make_shared<SLE>(positionKeylet);

        slePosition->setAccountID(sfAccount, account_);
        slePosition->setFieldH256(sfMarginAccountID, marginAccountID);
        slePosition->setFieldH256(sfOptionPairID, optionPairID);
        slePosition->setFieldIssue(sfAsset, STIssue{sfAsset, issue});
        slePosition->at(sfEntryPrice) =
            STNumber{sfEntryPrice, entryPrice};
        slePosition->at(sfPositionSize) =
            STNumber{sfPositionSize, Number(quantity)};
        slePosition->setFieldU32(sfPositionSide, positionSide);
        slePosition->setFieldU32(sfLeverage, leverage);
        slePosition->at(sfAllocatedMargin) =
            STNumber{sfAllocatedMargin, initialMargin};
        slePosition->at(sfNotionalValue) =
            STNumber{sfNotionalValue, notional};
        slePosition->at(sfLiquidationPrice) =
            STNumber{sfLiquidationPrice, liquidationPrice};

        // Add to owner directory
        auto const posPage = sb.dirInsert(
            keylet::ownerDir(account_),
            positionKeylet,
            describeOwnerDir(account_));
        if (!posPage)
            return tecDIR_FULL;

        slePosition->setFieldU64(sfOwnerNode, *posPage);

        // Link to margin account directory
        auto const marginPage = sb.dirInsert(
            keylet::ownerDir(account_),
            positionKeylet,
            describeOwnerDir(account_));
        slePosition->setFieldU64(sfMarginAccountNode,
            marginPage ? *marginPage : 0);

        sb.insert(slePosition);
        sb.update(sleMarginAcct);

        // Store position ID to link to the option offer later
        marginPositionID_ = positionKeylet.key;

        JLOG(j_.trace())
            << "OptionCreate: Margin position created with "
            << initialMargin << " initial margin at " << leverage
            << "x leverage, side=" << positionSide;
    }

    // Handle premium transfers for matched (sealed) options
    if (!sealedOptions.empty())
    {
        for (const auto& sealedOption : sealedOptions)
        {
            // Calculate premium for the sealed portion
            STAmount const sealedPremium = mulRound(
                sealedOption.premium,
                STAmount(
                    sealedOption.premium.issue(),
                    sealedOption.quantitySealed),
                sealedOption.premium.issue(),
                false);

            // Calculate and deduct trading fee from premium
            STAmount netPremium = sealedPremium;
            if (tradingFeeBps > 0)
            {
                Number const feeNum = Number(mulRound(
                    sealedPremium,
                    STAmount(sealedPremium.issue(), tradingFeeBps),
                    sealedPremium.issue(),
                    false)) / Number(100000);
                STAmount const fee{sealedPremium.issue(), feeNum};

                netPremium = sealedPremium - fee;
                totalFeesCollected =
                    totalFeesCollected + Number(fee);

                // Transfer fee to pseudo-account
                if (fee > beast::zero)
                {
                    // Fee comes from the buyer side
                    AccountID const feePayer =
                        isSell ? sealedOption.account : account_;
                    auto const fter = option::transferTokens(
                        sb, feePayer, pseudoAccount, fee, j_);
                    if (fter != tesSUCCESS)
                        return fter;
                }
            }

            // Transfer net premium: buyer pays seller
            AccountID const sender =
                isSell ? sealedOption.account : account_;
            AccountID const receiver =
                isSell ? account_ : sealedOption.account;

            JLOG(j_.trace())
                << "OptionCreate: Transfer premium: " << netPremium
                << " from " << sender << " to " << receiver;
            auto const ter = option::transferTokens(
                sb, sender, receiver, netPremium, j_);

            if (ter != tesSUCCESS)
                return ter;
        }
    }

    // Create new option offer with the matched and open quantities
    auto const ter = option::createOffer(
        sb,
        account_,
        optionOfferKeylet,
        flags,
        quantity,
        openInterest,
        premium,
        isSell,
        issue,
        strikePrice,
        strike,
        expiration,
        optionBookDirKeylet,
        sealedOptions,
        j_);

    if (ter != tesSUCCESS)
        return ter;

    // Link the option offer to the margin position
    if (marginPositionID_ != uint256{})
    {
        auto sleOffer = sb.peek(optionOfferKeylet);
        if (sleOffer)
        {
            sleOffer->setFieldH256(sfMarginPositionID, marginPositionID_);

            // Also set the option offer ID on the margin position
            auto slePos = sb.peek(
                Keylet{ltMARGIN_POSITION, marginPositionID_});
            if (slePos)
            {
                slePos->setFieldH256(
                    sfOptionOfferID, optionOfferKeylet.key);
                sb.update(slePos);
            }

            sb.update(sleOffer);
        }
    }

    // Create the option in the ledger if it doesn't exist yet
    std::optional<Keylet> const optionKeylet =
        keylet::option(issue.account, issue.currency, strike, expiration);

    if (!sb.exists(*optionKeylet))
    {
        // Create new option ledger entry
        auto const sleOption = std::make_shared<SLE>(*optionKeylet);

        // Add to owner directory
        auto const newPage = sb.dirInsert(
            keylet::ownerDir(issue.account),
            *optionKeylet,
            describeOwnerDir(issue.account));

        if (!newPage)
        {
            JLOG(j_.trace())
                << "OptionList: Failed to add list to owner directory";
            return tecDIR_FULL;
        }

        // Set option properties
        (*sleOption)[sfOwnerNode] = *newPage;
        (*sleOption)[sfStrikePrice] = strikePrice;
        (*sleOption)[sfAsset] = STIssue{sfAsset, asset};
        (*sleOption)[sfExpiration] = expiration;

        // Add option to ledger
        sb.insert(sleOption);
    }

    // Update accumulated fees on the OptionPair
    if (totalFeesCollected > Number(0))
    {
        Number currentFees =
            slePseudo->at(~sfAccumulatedFees).value_or(Number(0));
        currentFees = currentFees + totalFeesCollected;
        slePseudo->at(sfAccumulatedFees) =
            STNumber{sfAccumulatedFees, currentFees};
        sb.update(slePseudo);
    }

    // Apply all changes to the ledger
    sb.update(sleSource);
    sb.apply(ctx_.rawView());
    return tesSUCCESS;
}

}  // namespace xrpl