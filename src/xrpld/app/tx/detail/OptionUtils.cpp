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

#include <xrpld/app/tx/detail/OptionUtils.h>
#include <xrpld/ledger/Dir.h>
#include <xrpld/ledger/View.h>

#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/STArray.h>
#include <xrpl/protocol/TxFlags.h>

#include <functional>
#include <memory>

namespace ripple {

namespace option {

std::vector<SealedOptionData>
matchOptions(
    Sandbox& sb,
    Issue issue,
    std::uint64_t strike,
    std::uint32_t expiration,
    bool isPut,
    bool isSell,
    std::uint32_t desiredQuantity,
    AccountID const& account,
    uint256 const& optionIndex,
    bool isMarketOrder,
    STAmount const& limitPrice)
{
    const uint256 uBookBaseOpp =
        getOptionBookBase(issue.account, issue.currency, strike, expiration);
    const uint256 uBookEndOpp = getOptionQualityNext(uBookBaseOpp);
    auto key = sb.succ(uBookBaseOpp, uBookEndOpp);
    if (!key)
        return {};

    std::vector<SealedOptionData> sealedOptions;
    std::uint32_t totalSealedQuantity = 0;

    while (key)
    {
        auto sleOfferDir = sb.read(keylet::page(key.value()));
        if (!sleOfferDir)
            break;

        uint256 offerIndex;
        unsigned int bookEntry;

        if (!cdirFirst(
                sb, sleOfferDir->key(), sleOfferDir, bookEntry, offerIndex))
        {
            key = sb.succ(sleOfferDir->key(), uBookEndOpp);
            continue;
        }

        do
        {
            auto sleItem = sb.peek(keylet::child(offerIndex));
            if (!sleItem)
                continue;

            // Skip Sealed Options
            if (sleItem->getFieldU32(sfOpenInterest) == 0)
                continue;

            // Looking for opposite option position.
            auto const flags = sleItem->getFlags();

            // we need to only match the same type of option
            auto const offerPut = flags & tfPut;
            if (isPut && !offerPut)
                continue;

            // we need to match the opposite side of the offer
            auto const offerSell = flags & tfSell;
            if (isSell && offerSell)
                continue;

            // Get the premium for this offer
            STAmount offerPremium = sleItem->getFieldAmount(sfPremium);

            // For limit orders, check if the price is acceptable
            if (!isMarketOrder)
            {
                // For a buy order, we want premium <= limitPrice
                // For a sell order, we want premium >= limitPrice
                if (isSell)
                {
                    if (offerPremium < limitPrice)
                        continue;  // Premium too low for seller
                }
                else
                {
                    if (offerPremium > limitPrice)
                        continue;  // Premium too high for buyer
                }
            }

            // Determine how much we can seal from this option
            uint32_t availableQuantity = sleItem->getFieldU32(sfOpenInterest);
            uint32_t quantityToSeal = std::min(
                availableQuantity, desiredQuantity - totalSealedQuantity);

            // Update the option's open interest
            sleItem->setFieldU32(
                sfOpenInterest, availableQuantity - quantityToSeal);

            // Get the counterpart account ID
            AccountID accountID = sleItem->getAccountID(sfOwner);

            // Build data for this sealed option
            sealedOptions.push_back(
                {offerIndex, accountID, quantityToSeal, offerPremium});

            STArray sealedOptionsArray =
                sleItem->isFieldPresent(sfSealedOptions)
                ? sleItem->getFieldArray(sfSealedOptions)
                : STArray();

            // Create a new sfSealedOption object
            STObject sealedOption(sfSealedOption);
            sealedOption.setAccountID(sfOwner, account);
            sealedOption.setFieldH256(sfOptionOfferID, optionIndex);
            sealedOption.setFieldU32(sfQuantity, quantityToSeal);

            // Append the new sfSealedOption object to the array
            sealedOptionsArray.push_back(std::move(sealedOption));
            sleItem->setFieldArray(
                sfSealedOptions, std::move(sealedOptionsArray));

            sb.update(sleItem);

            totalSealedQuantity += quantityToSeal;

            if (totalSealedQuantity >= desiredQuantity)
                return sealedOptions;
        } while (cdirNext(
            sb, sleOfferDir->key(), sleOfferDir, bookEntry, offerIndex));

        key = sb.succ(sleOfferDir->key(), uBookEndOpp);
    }
    return sealedOptions;
}

TER
createOffer(
    Sandbox& sb,
    AccountID const& account,
    Keylet const& optionOfferKeylet,
    std::uint32_t flags,
    std::uint32_t quantity,
    std::uint32_t openInterest,
    STAmount const& premium,
    bool isSell,
    STAmount const& lockedAmount,
    Issue const& issue,
    STAmount strikePrice,
    std::int64_t strike,
    std::uint32_t expiration,
    Keylet const& optionBookDirKeylet,
    std::vector<SealedOptionData> const& sealedOptions,
    beast::Journal j)
{
    JLOG(j.trace()) << "OptionUtils.createOffer: account=" << to_string(account)
                    << ", strikePrice=" << strikePrice
                    << ", expiration=" << expiration
                    << ", quantity=" << quantity
                    << ", openInterest=" << openInterest
                    << ", premium=" << premium << ", isSell=" << isSell
                    << ", lockedAmount=" << lockedAmount;
    // Add Option to Self 3 Times [issuer, party, counter-party])
    auto sleSrcAcc = sb.peek(keylet::account(account));
    if (!sleSrcAcc)
        return terNO_ACCOUNT;

    XRPAmount const reserve =
        sb.fees().accountReserve(sleSrcAcc->getFieldU32(sfOwnerCount) + 1);
    XRPAmount const sourceBalance = sleSrcAcc->getFieldAmount(sfBalance).xrp();
    if (sourceBalance < reserve)
        return tecINSUFFICIENT_RESERVE;

    adjustOwnerCount(sb, sleSrcAcc, 1, j);

    auto optionOffer = std::make_shared<SLE>(optionOfferKeylet);
    auto const page = sb.dirInsert(
        keylet::ownerDir(account),
        optionOfferKeylet,
        describeOwnerDir(account));
    if (!page)
    {
        JLOG(j.trace()) << "final result: failed to add offer to owner dir";
        return tecDIR_FULL;
    }

    optionOffer->setFlag(flags);
    optionOffer->setAccountID(sfOwner, account);
    optionOffer->setFieldU64(sfOwnerNode, *page);
    optionOffer->setFieldAmount(sfStrikePrice, strikePrice);
    optionOffer->setFieldIssue(sfAsset, STIssue{sfAsset, issue});
    optionOffer->setFieldU32(sfExpiration, expiration);
    optionOffer->setFieldU32(sfQuantity, quantity);
    optionOffer->setFieldU32(sfOpenInterest, openInterest);
    optionOffer->setFieldAmount(sfPremium, premium);     // Premium
    optionOffer->setFieldAmount(sfAmount, STAmount(0));  // Locked
    STArray sealedOptionsArray = optionOffer->isFieldPresent(sfSealedOptions)
        ? optionOffer->getFieldArray(sfSealedOptions)
        : STArray();

    for (const auto& sealedOption : sealedOptions)
    {
        AccountID const& accountID = sealedOption.account;
        std::uint32_t quantityToSeal = sealedOption.quantitySealed;
        // Create a new sfSealedOption object
        STObject sealedOptionObj(sfSealedOption);
        sealedOptionObj.setAccountID(sfOwner, accountID);
        sealedOptionObj.setFieldH256(sfOptionOfferID, sealedOption.offerID);
        sealedOptionObj.setFieldU32(sfQuantity, quantityToSeal);
        // Append the new sfSealedOption object to the array
        sealedOptionsArray.push_back(std::move(sealedOptionObj));
    }
    optionOffer->setFieldArray(sfSealedOptions, std::move(sealedOptionsArray));
    if (isSell)
        optionOffer->setFieldAmount(sfAmount, lockedAmount);  // Locked

    Option const option{issue, static_cast<uint64_t>(strike), expiration};
    // std::int64_t const premium64 =
    // static_cast<std::int64_t>(Number(premium));
    auto dir = keylet::optionQuality(optionBookDirKeylet, premium.mantissa());
    // auto dir = keylet::optionQuality(optionBookDirKeylet, premium64);
    // bool const bookExisted = static_cast<bool>(sb.peek(dir));
    auto const bookNode =
        sb.dirAppend(dir, optionOfferKeylet, [&](SLE::ref sle) {
            sle->setFieldH160(sfTakerPaysIssuer, issue.account);
            sle->setFieldH160(sfTakerPaysCurrency, issue.currency);
            sle->setFieldU64(sfStrike, strike);
            sle->setFieldU32(sfExpiration, expiration);
            sle->setFieldU64(sfExchangeRate, premium.mantissa());
            // sle->setFieldU64(sfExchangeRate, premium64);
        });

    if (!bookNode)
    {
        JLOG(j.trace()) << "final result: failed to add offer to book";
        return tecDIR_FULL;
    }

    // if (!bookExisted)
    //     ctx_.app.getOrderBookDB().addOptionOrderBook(option);

    optionOffer->setFieldH256(sfBookDirectory, dir.key);
    optionOffer->setFieldU64(sfBookNode, *bookNode);
    sb.insert(optionOffer);
    return tesSUCCESS;
}

TER
lockTokens(
    Sandbox& sb,
    XRPAmount const& sourceBalance,
    AccountID const& account,
    STAmount const& amount,
    beast::Journal j)
{
    auto sleSrcAcc = sb.peek(keylet::account(account));
    if (!sleSrcAcc)
        return terNO_ACCOUNT;

    if (isXRP(amount))
    {
        JLOG(j.trace()) << "OptionUtils: XRP lock: " << amount.getCurrency()
                        << ": " << amount.getIssuer() << ": " << amount;
        // subtract the quantity from the writer
        if (sourceBalance < amount.xrp())
            return tecUNFUNDED_PAYMENT;
        {
            STAmount bal = sourceBalance;
            bal -= amount.xrp();
            if (bal < beast::zero || bal > sourceBalance)
                return tecINTERNAL;

            sleSrcAcc->setFieldAmount(sfBalance, bal);
        }
    }
    else
    {
        JLOG(j.trace()) << "OptionUtils: IOU lock: " << amount.getCurrency()
                        << ": " << amount.getIssuer() << ": " << amount;
        STAmount spendableAmount{accountHolds(
            sb,
            account,
            amount.getCurrency(),
            amount.getIssuer(),
            fhZERO_IF_FROZEN,
            j)};

        if (spendableAmount < amount)
            return tecINSUFFICIENT_FUNDS;

        auto const ter =
            rippleCredit(sb, account, amount.getIssuer(), amount, true, j);
        if (ter != tesSUCCESS)
            return ter;  // LCOV_EXCL_LINE
    }
    return tesSUCCESS;
}

TER
unlockTokens(
    Sandbox& sb,
    AccountID const& receiver,
    std::shared_ptr<SLE> const& sleReceiver,
    STAmount const& amount,
    beast::Journal j)
{
    if (isXRP(amount))
    {
        JLOG(j.trace()) << "OptionSettle: XRP unlock: " << amount;
        STAmount balance = sleReceiver->getFieldAmount(sfBalance);
        STAmount bal = balance;
        bal += amount.xrp();
        if (bal < beast::zero || bal < balance)
            return tecINTERNAL;
        sleReceiver->setFieldAmount(sfBalance, bal);
    }
    else
    {
        JLOG(j.trace()) << "OptionSettle: IOU unlock: " << amount;
        auto const ter =
            rippleCredit(sb, amount.getIssuer(), receiver, amount, true, j);
        if (ter != tesSUCCESS)
            return ter;  // LCOV_EXCL_LINE
    }

    return tesSUCCESS;
}

TER
transferTokens(
    Sandbox& sb,
    AccountID const& sender,
    AccountID const& receiver,
    STAmount const& amount,
    beast::Journal j)
{
    if (isXRP(amount))
    {
        JLOG(j.trace()) << "OptionSettle: XRP transfer: " << amount;
        auto sleSender = sb.read(keylet::account(sender));
        if (!sleSender)
            return terNO_ACCOUNT;

        STAmount senderBalance = sleSender->getFieldAmount(sfBalance);
        if (senderBalance < amount.xrp())
            return tecUNFUNDED_PAYMENT;
    }
    else
    {
        JLOG(j.trace()) << "OptionSettle: IOU transfer: " << amount;
        STAmount spendableAmount{accountHolds(
            sb,
            sender,
            amount.getCurrency(),
            amount.getIssuer(),
            fhZERO_IF_FROZEN,
            j)};
        if (spendableAmount < amount)
        {
            JLOG(j.trace()) << "OptionSettle: Insufficient funds."
                            << spendableAmount << " < " << amount;
            return tecINSUFFICIENT_FUNDS;
        }
    }

    if (TER result =
            accountSend(sb, sender, receiver, amount, j, WaiveTransferFee::No);
        !isTesSuccess(result))
        return result;

    return tesSUCCESS;
}

TER
closeOffer(
    Sandbox& sb,
    AccountID const& account,
    Keylet const& offerKeylet,
    bool isPut,
    bool isSell,
    Issue const& issue,
    std::uint64_t const& strike,
    std::uint32_t const& expiration,
    beast::Journal j)
{
    // Retrieve the option offer being closed
    auto sleOffer = sb.peek(offerKeylet);
    if (!sleOffer)
    {
        JLOG(j.trace()) << "OptionUtils: Option offer does not exist.";
        return tecNO_ENTRY;
    }

    // Verify the option belongs to the account
    if (sleOffer->getAccountID(sfOwner) != account)
    {
        JLOG(j.trace()) << "OptionUtils: Not owner of option.";
        return tecNO_PERMISSION;
    }

    // For sellers, unlock collateral or assets
    if (isSell)
    {
        STAmount lockedAmount = sleOffer->getFieldAmount(sfAmount);

        if (lockedAmount.mantissa() > 0)
        {
            // Get account SLE for the owner
            auto sleSeller = sb.peek(keylet::account(account));
            if (!sleSeller)
                return terNO_ACCOUNT;

            // Unlock the collateral or assets based on option type
            auto ter = unlockTokens(sb, account, sleSeller, lockedAmount, j);
            if (ter != tesSUCCESS)
                return ter;

            sb.update(sleSeller);
            JLOG(j.trace())
                << "OptionUtils: Unlocked " << lockedAmount << " for sell "
                << (isPut ? "put" : "call") << " option.";
        }
    }

    // Check if this option has any sealed relationships
    if (!sleOffer->isFieldPresent(sfSealedOptions) ||
        sleOffer->getFieldArray(sfSealedOptions).empty())
    {
        // If no sealed options, just delete the offer and return
        if (auto ter = deleteOffer(sb, sleOffer, j); ter != tesSUCCESS)
        {
            JLOG(j.trace()) << "OptionUtils: Failed to delete offer.";
            return ter;
        }
        return tesSUCCESS;
    }

    // Continue with replacing sealed options with new counterparties

    // Map to track counterparty options and their sealed quantities
    struct CounterpartyInfo
    {
        std::shared_ptr<SLE> option;
        std::uint32_t totalQuantity = 0;
        std::vector<std::size_t>
            sealedIndices;  // Indices in the original array
    };

    std::map<uint256, CounterpartyInfo> counterpartyMap;
    STArray sealedOptionsArray = sleOffer->getFieldArray(sfSealedOptions);

    // First pass: Group by counterparty option and sum quantities
    for (std::size_t i = 0; i < sealedOptionsArray.size(); ++i)
    {
        auto& sealedOption = sealedOptionsArray[i];
        uint256 const cOfferID = sealedOption.getFieldH256(sfOptionOfferID);
        std::uint32_t sealedQuantity = sealedOption.getFieldU32(sfQuantity);

        // Find the counterparty's option
        auto cKeylet = keylet::unchecked(cOfferID);
        auto cOption = sb.peek(cKeylet);

        if (!cOption)
        {
            JLOG(j.trace()) << "OptionUtils: Counterparty option not found: "
                            << to_string(cOfferID);
            return tecNO_ENTRY;
        }

        // Add to our map
        if (counterpartyMap.find(cOfferID) == counterpartyMap.end())
        {
            counterpartyMap[cOfferID] = {cOption, sealedQuantity, {i}};
        }
        else
        {
            counterpartyMap[cOfferID].totalQuantity += sealedQuantity;
            counterpartyMap[cOfferID].sealedIndices.push_back(i);
        }
    }

    JLOG(j.trace()) << "OptionUtils: Counterparty options found: "
                    << counterpartyMap.size();

    STAmount limitPrice = sleOffer->getFieldAmount(sfPremium);
    // For each unique counterparty option, find replacement offers
    for (auto& [cOfferID, cInfo] : counterpartyMap)
    {
        auto cOption = cInfo.option;
        std::uint32_t totalQuantity = cInfo.totalQuantity;
        AccountID counterpartyAccount = cOption->getAccountID(sfOwner);

        // Create a vector of option IDs to exclude when matching
        std::vector<uint256> excludeOptionIDs;
        excludeOptionIDs.push_back(
            cOfferID);  // Exclude the counterparty option itself

        // Use updated matchOptions with market order parameter
        bool isMarketOrder = true;  // Default to market orders for now
        std::vector<SealedOptionData> newMatches;

        // Custom implementation to find matches while excluding specific offers
        const uint256 uBookBaseOpp = getOptionBookBase(
            issue.account, issue.currency, strike, expiration);
        const uint256 uBookEndOpp = getOptionQualityNext(uBookBaseOpp);
        auto key = sb.succ(uBookBaseOpp, uBookEndOpp);

        std::uint32_t totalMatchedQuantity = 0;

        // Similar to matchOptions but with exclusion logic
        while (key && totalMatchedQuantity < totalQuantity)
        {
            auto sleOfferDir = sb.read(keylet::page(key.value()));
            if (!sleOfferDir)
                break;

            uint256 offerIndex;
            unsigned int bookEntry;

            if (!cdirFirst(
                    sb, sleOfferDir->key(), sleOfferDir, bookEntry, offerIndex))
            {
                key = sb.succ(sleOfferDir->key(), uBookEndOpp);
                continue;
            }

            do
            {
                // Skip if this offer ID is in our exclusion list
                bool shouldExclude = false;
                for (const auto& excludeID : excludeOptionIDs)
                {
                    if (offerIndex == excludeID)
                    {
                        shouldExclude = true;
                        break;
                    }
                }

                if (shouldExclude)
                    continue;

                auto sleItem = sb.peek(keylet::child(offerIndex));
                if (!sleItem)
                    continue;

                // Skip if no open interest
                if (sleItem->getFieldU32(sfOpenInterest) == 0)
                {
                    JLOG(j.trace())
                        << "OptionUtils: No open interest for offer: "
                        << to_string(offerIndex);
                    continue;
                }

                // Looking for opposite option position
                auto const flags = sleItem->getFlags();

                // Match same type of option
                auto const offerPut = flags & tfPut;
                if (isPut && !offerPut)
                {
                    JLOG(j.trace()) << "OptionUtils: Option type mismatch: "
                                    << to_string(offerIndex);
                    continue;  // Option type mismatch
                }

                // Match same side of option
                auto const offerSell = flags & tfSell;
                if (isSell && !offerSell)
                {
                    JLOG(j.trace()) << "OptionUtils: Offer side mismatch: "
                                    << to_string(offerIndex);
                    continue;  // Offer is a sell offer
                }

                // Get the premium for this offer
                STAmount offerPremium = sleItem->getFieldAmount(sfPremium);

                // For limit orders, check if the price is acceptable
                if (!isMarketOrder)
                {
                    // For a buy order, we want premium <= limitPrice
                    // For a sell order, we want premium >= limitPrice
                    if (isSell)
                    {
                        if (offerPremium < limitPrice)
                        {
                            JLOG(j.trace())
                                << "OptionUtils: Premium too low for seller: "
                                << offerPremium;
                            continue;  // Premium too low for seller
                        }
                    }
                    else
                    {
                        if (offerPremium > limitPrice)
                        {
                            JLOG(j.trace())
                                << "OptionUtils: Premium too high for buyer: "
                                << offerPremium;
                            continue;  // Premium too high for buyer
                        }
                    }
                }

                // Determine how much we can seal from this option
                uint32_t availableQuantity =
                    sleItem->getFieldU32(sfOpenInterest);
                uint32_t quantityToSeal = std::min(
                    availableQuantity, totalQuantity - totalMatchedQuantity);

                // Update the option's open interest
                sleItem->setFieldU32(
                    sfOpenInterest, availableQuantity - quantityToSeal);

                // Get the counterpart account ID
                AccountID accountID = sleItem->getAccountID(sfOwner);

                // Build data for this sealed option with premium information
                newMatches.push_back(
                    {offerIndex, accountID, quantityToSeal, offerPremium});

                STArray sealedOptionsArray =
                    sleItem->isFieldPresent(sfSealedOptions)
                    ? sleItem->getFieldArray(sfSealedOptions)
                    : STArray();

                // Create a new sfSealedOption object
                STObject sealedOption(sfSealedOption);
                sealedOption.setAccountID(sfOwner, counterpartyAccount);
                sealedOption.setFieldH256(sfOptionOfferID, cOfferID);
                sealedOption.setFieldU32(sfQuantity, quantityToSeal);

                // Append the new sfSealedOption object to the array
                sealedOptionsArray.push_back(std::move(sealedOption));
                sleItem->setFieldArray(
                    sfSealedOptions, std::move(sealedOptionsArray));

                sb.update(sleItem);

                totalMatchedQuantity += quantityToSeal;

                if (totalMatchedQuantity >= totalQuantity)
                    break;

            } while (cdirNext(
                sb, sleOfferDir->key(), sleOfferDir, bookEntry, offerIndex));

            key = sb.succ(sleOfferDir->key(), uBookEndOpp);
        }

        if (totalMatchedQuantity < totalQuantity)
        {
            // If not enough matches found, fail the transaction
            JLOG(j.trace())
                << "OptionUtils: Cannot close option - not enough matching "
                   "offers found to replace sealed options for counterparty "
                << to_string(cOfferID) << ". Required: " << totalQuantity
                << ", Found: " << totalMatchedQuantity;

            // Revert any matches we've already made
            for (const auto& match : newMatches)
            {
                auto matchKeylet = keylet::unchecked(match.offerID);
                auto matchOffer = sb.peek(matchKeylet);
                if (matchOffer)
                {
                    // Restore original open interest
                    std::uint32_t currentOpenInterest =
                        matchOffer->getFieldU32(sfOpenInterest);
                    matchOffer->setFieldU32(
                        sfOpenInterest,
                        currentOpenInterest + match.quantitySealed);

                    // Remove the sealed option we just added
                    if (matchOffer->isFieldPresent(sfSealedOptions))
                    {
                        STArray mSealedOptions =
                            matchOffer->getFieldArray(sfSealedOptions);
                        STArray updatedOptions;

                        for (auto& mso : mSealedOptions)
                        {
                            if (!mso.isFieldPresent(sfOptionOfferID) ||
                                mso.getFieldH256(sfOptionOfferID) != cOfferID)
                            {
                                updatedOptions.push_back(mso);
                            }
                        }

                        matchOffer->setFieldArray(
                            sfSealedOptions, updatedOptions);
                    }

                    sb.update(matchOffer);
                }
            }
            JLOG(j.trace())
                << "OptionUtils: Failed to close option - not enough "
                   "counterparty offers found.";
            return tecFAILED_PROCESSING;
        }

        // For buyers closing positions, handle payment from new buyers
        if (!isSell)
        {
            JLOG(j.trace())
                << "OptionUtils: Closing buy position for account " << account
                << " with " << newMatches.size() << " new matches.";
            // If this is a buyer closing their position:
            // 1. New counterparties should pay the buyer (account) for taking
            // over the position
            // 2. Payment should be based on current market value, not original
            // premium

            // Process payments from each new counterparty
            for (const auto& match : newMatches)
            {
                // Calculate payment amount for this match
                STAmount matchPremium = match.premium;
                STAmount paymentAmount = mulRound(
                    matchPremium,
                    STAmount(matchPremium.issue(), match.quantitySealed),
                    matchPremium.issue(),
                    false);

                // Transfer payment from new counterparty to the account closing
                // their position
                auto ter = transferTokens(
                    sb, match.account, account, paymentAmount, j);

                if (ter != tesSUCCESS)
                    return ter;

                JLOG(j.trace())
                    << "OptionUtils: Received payment of " << paymentAmount
                    << " from " << match.account << " for closing buy position";
            }
        }

        // Update the counterparty option
        if (cOption->isFieldPresent(sfSealedOptions))
        {
            STArray cSealedOptions = cOption->getFieldArray(sfSealedOptions);
            STArray updatedSealedOptions;

            // Keep all sealed options that don't refer to the offer being
            // closed
            for (auto& cso : cSealedOptions)
            {
                if (!cso.isFieldPresent(sfOptionOfferID) ||
                    cso.getFieldH256(sfOptionOfferID) != offerKeylet.key)
                {
                    updatedSealedOptions.push_back(cso);
                }
            }

            // Add new sealed options referring to the new matching offers
            for (const auto& newMatch : newMatches)
            {
                STObject newSealedOption(sfSealedOption);
                newSealedOption.setAccountID(sfOwner, newMatch.account);
                newSealedOption.setFieldH256(sfOptionOfferID, newMatch.offerID);
                newSealedOption.setFieldU32(
                    sfQuantity, newMatch.quantitySealed);

                updatedSealedOptions.push_back(std::move(newSealedOption));
            }

            cOption->setFieldArray(sfSealedOptions, updatedSealedOptions);
            sb.update(cOption);

            JLOG(j.trace()) << "OptionUtils: Updated counterparty option "
                            << to_string(cOfferID) << " with "
                            << newMatches.size() << " new matches.";
        }
    }

    // Remove the option being closed
    if (auto ter = deleteOffer(sb, sleOffer, j); ter != tesSUCCESS)
    {
        JLOG(j.trace()) << "OptionUtils: Failed to delete offer.";
        return ter;
    }

    return tesSUCCESS;
}

TER
exerciseOffer(
    Sandbox& sb,
    bool isPut,
    STAmount const& strikePrice,
    AccountID const& buyer,
    std::shared_ptr<SLE> const& sleBuyer,
    Issue const& issue,
    STArray const& sealedOptions,
    beast::Journal j)
{
    for (const auto& sealedOption : sealedOptions)
    {
        AccountID const owner = sealedOption.getAccountID(sfOwner);
        uint256 const offerID = sealedOption.getFieldH256(sfOptionOfferID);
        auto sleSealedOffer = sb.peek(keylet::optionOffer(offerID));
        if (!sleSealedOffer)
            return tecNO_TARGET;

        std::uint32_t const quantity = sealedOption.getFieldU32(sfQuantity);
        STAmount const quantityShares = STAmount(issue, quantity);
        STAmount const totalValue = mulRound(
            strikePrice,
            STAmount(strikePrice.issue(), quantity),
            strikePrice.issue(),
            false);

        STAmount const unlockAmount = isPut ? totalValue : quantityShares;
        STAmount const transferAmount = isPut ? quantityShares : totalValue;
        auto const ter =
            option::unlockTokens(sb, buyer, sleBuyer, unlockAmount, j);
        if (ter != tesSUCCESS)
            return ter;

        auto const ter2 =
            option::transferTokens(sb, buyer, owner, transferAmount, j);
        if (ter2 != tesSUCCESS)
            return ter2;

        if (quantity != sleSealedOffer->getFieldU32(sfQuantity))
        {
            sleSealedOffer->setFieldAmount(
                sfAmount,
                sleSealedOffer->getFieldAmount(sfAmount) - unlockAmount);
            sb.update(sleSealedOffer);
        }
        else
        {
            if (auto ter = option::deleteOffer(sb, sleSealedOffer, j);
                ter != tesSUCCESS)
            {
                JLOG(j.trace())
                    << "OptionUtils: Failed to delete offer after exercise.";
                return ter;
            }
        }
    }
    return tesSUCCESS;
}

TER
expireOffer(ApplyView& view, std::shared_ptr<SLE> const& sle, beast::Journal j)
{
    AccountID const account = (*sle)[sfOwner];
    auto const sleAccount = view.peek(keylet::account(account));
    if (!sleAccount)
        return tecINTERNAL;  // LCOV_EXCL_LINE

    // Get the option flags
    auto const optionFlags = sle->getFlags();
    bool const isSell = optionFlags & tfSell;
    uint256 const offerID = sle->key();

    // For sellers, unlock and return any locked collateral or assets
    if (isSell)
    {
        STAmount lockedAmount = sle->getFieldAmount(sfAmount);

        if (lockedAmount.mantissa() > 0)
        {
            // Get account SLE for the owner
            auto sleSeller = view.peek(keylet::account(account));
            if (!sleSeller)
                return terNO_ACCOUNT;

            if (isXRP(lockedAmount))
            {
                JLOG(j.trace()) << "OptionSettle: XRP unlock: " << lockedAmount;
                STAmount balance = sleSeller->getFieldAmount(sfBalance);
                STAmount bal = balance;
                bal += lockedAmount.xrp();
                if (bal < beast::zero || bal < balance)
                    return tecINTERNAL;
                sleSeller->setFieldAmount(sfBalance, bal);
            }
            else
            {
                JLOG(j.trace()) << "OptionSettle: IOU unlock: " << lockedAmount;
                auto const ter = rippleCredit(
                    view,
                    lockedAmount.getIssuer(),
                    account,
                    lockedAmount,
                    true,
                    j);
                if (ter != tesSUCCESS)
                    return ter;  // LCOV_EXCL_LINE
            }

            view.update(sleSeller);
            JLOG(j.trace()) << "OptionUtils: Unlocked and returned "
                            << lockedAmount << " for expired sell option.";
        }
    }

    // Handle sealed options, if any
    if (sle->isFieldPresent(sfSealedOptions) &&
        !sle->getFieldArray(sfSealedOptions).empty())
    {
        STArray sealedOptions = sle->getFieldArray(sfSealedOptions);

        // For each sealed option, update the counterparty's option
        for (auto const& sealedOption : sealedOptions)
        {
            uint256 const cOfferID = sealedOption.getFieldH256(sfOptionOfferID);
            auto cKeylet = keylet::unchecked(cOfferID);
            auto cOption = view.peek(cKeylet);

            if (cOption && cOption->isFieldPresent(sfSealedOptions))
            {
                // Remove references to the expired option from counterparty's
                // sealed options
                STArray cSealedOptions =
                    cOption->getFieldArray(sfSealedOptions);
                STArray updatedCSealed;

                for (auto const& cso : cSealedOptions)
                {
                    if (!cso.isFieldPresent(sfOptionOfferID) ||
                        cso.getFieldH256(sfOptionOfferID) != offerID)
                    {
                        updatedCSealed.push_back(cso);
                    }
                }

                cOption->setFieldArray(sfSealedOptions, updatedCSealed);
                view.update(cOption);

                JLOG(j.trace()) << "OptionUtils: Updated counterparty option "
                                << to_string(cOfferID)
                                << " to remove expired option reference.";
            }
        }
    }

    // Delete the expired option offer
    if (auto ter = deleteOffer(view, sle, j); ter != tesSUCCESS)
    {
        JLOG(j.trace()) << "OptionUtils: Failed to delete expired offer.";
        return ter;
    }

    JLOG(j.trace()) << "OptionUtils: Successfully expired option offer "
                    << to_string(offerID);

    return tesSUCCESS;
}

TER
deleteOffer(ApplyView& view, std::shared_ptr<SLE> const& sle, beast::Journal j)
{
    if (!sle)
        return tecINTERNAL;  // LCOV_EXCL_LINE

    AccountID const account = (*sle)[sfOwner];
    auto const sleAccount = view.peek(keylet::account(account));
    if (!sleAccount)
        return tecINTERNAL;  // LCOV_EXCL_LINE

    // Remove from owner directory
    if (!view.dirRemove(
            keylet::ownerDir(account), (*sle)[sfOwnerNode], sle->key(), true))
    {
        // LCOV_EXCL_START
        JLOG(j.trace()) << "Unable to delete OptionOffer from owner.";
        return tefBAD_LEDGER;
        // LCOV_EXCL_STOP
    }

    // Remove from book directory
    if (sle->isFieldPresent(sfBookDirectory) && sle->isFieldPresent(sfBookNode))
    {
        if (!view.dirRemove(
                keylet::page(sle->getFieldH256(sfBookDirectory)),
                (*sle)[sfBookNode],
                sle->key(),
                true))
        {
            // LCOV_EXCL_START
            JLOG(j.trace()) << "Unable to delete OptionOffer from book.";
            return tefBAD_LEDGER;
            // LCOV_EXCL_STOP
        }
    }

    adjustOwnerCount(view, sleAccount, -1, j);
    view.erase(sle);
    return tesSUCCESS;
}

}  // namespace option
}  // namespace ripple
