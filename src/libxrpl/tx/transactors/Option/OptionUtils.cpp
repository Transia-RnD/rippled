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

#include <xrpl/tx/transactors/Option/OptionUtils.h>
#include <xrpl/tx/transactors/Option/MarginUtils.h>
#include <xrpl/ledger/Dir.h>
#include <xrpl/ledger/View.h>

#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/STArray.h>
#include <xrpl/protocol/STNumber.h>
#include <xrpl/protocol/TxFlags.h>

#include <functional>
#include <memory>

namespace xrpl {

namespace option {

/**
 * @brief Matches option offers on the ledger based on provided criteria.
 *
 * This function searches the option book for matching offers based on
 * parameters like asset type, strike price, expiration, and option
 * characteristics. For each match found, it updates the open interest of the
 * matched offer and creates a sealed option relationship between the parties.
 *
 * @param sb Sandbox ledger view to read from and apply changes to
 * @param issue The underlying asset (currency and issuer)
 * @param strike Strike price of the option in smallest units
 * @param expiration Expiration time of the option as a Unix timestamp
 * @param isPut Whether this is a put option (true) or call option (false)
 * @param isSell Whether this is a sell order (true) or buy order (false)
 * @param desiredQuantity The quantity of options to match
 * @param account The account ID of the requester
 * @param optionIndex The index of the requester's option offer
 * @param isMarketOrder Whether this is a market order (true) or limit order
 * (false)
 * @param limitPrice The price limit for a limit order (ignored for market
 * orders)
 * @return Vector of matched options with their details
 */
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
    // Get the base of the option book directory for this asset, strike, and
    // expiration
    const uint256 uBookBaseOpp =
        getOptionBookBase(issue.account, issue.currency, strike, expiration);

    // Get the end marker of the option book directory
    const uint256 uBookEndOpp = getOptionQualityNext(uBookBaseOpp);

    // Get the first entry in the book
    auto key = sb.succ(uBookBaseOpp, uBookEndOpp);

    // Return empty array if no offers in the book
    if (!key)
        return {};

    // Initialize result array and tracking variable
    std::vector<SealedOptionData> sealedOptions;
    std::uint32_t totalSealedQuantity = 0;

    // Loop through the book directory pages
    while (key)
    {
        // Read the current directory page
        auto sleOfferDir = sb.read(keylet::page(key.value()));
        if (!sleOfferDir)
            break;

        uint256 offerIndex;
        unsigned int bookEntry;

        // Get the first offer in the directory page
        if (!cdirFirst(
                sb, sleOfferDir->key(), sleOfferDir, bookEntry, offerIndex))
        {
            // If no entries in this page, move to the next page
            key = sb.succ(sleOfferDir->key(), uBookEndOpp);
            continue;
        }

        // Loop through all offers in the current directory page
        do
        {
            // Read the option offer
            auto sleItem = sb.peek(keylet::child(offerIndex));
            if (!sleItem)
                continue;

            // Skip offers with no open interest (already fully matched)
            if (sleItem->getFieldU32(sfOpenInterest) == 0)
                continue;

            // Get the offer flags
            auto const flags = sleItem->getFlags();

            // Match option type (put/call) - we need same type
            auto const offerPut = (flags & tfPut) != 0;
            if (isPut != offerPut)
                continue;  // Skip if option type mismatch (put vs call)

            // Match offer side (buy/sell) - we need opposite side
            auto const offerSell = (flags & tfSell) != 0;
            if (isSell == offerSell)
                continue;  // Skip if same side (both sell or both buy)

            // Get the premium for this offer
            STAmount offerPremium = sleItem->getFieldAmount(sfPremium);

            // For limit orders, check price constraints
            if (!isMarketOrder)
            {
                // For sell orders (we're selling), premium must be >= limit
                // price For buy orders (we're buying), premium must be <= limit
                // price
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

            // Calculate how much of this offer we can match
            uint32_t availableQuantity = sleItem->getFieldU32(sfOpenInterest);
            uint32_t quantityToSeal = std::min(
                availableQuantity, desiredQuantity - totalSealedQuantity);

            // Update the option's open interest (reduce by matched amount)
            sleItem->setFieldU32(
                sfOpenInterest, availableQuantity - quantityToSeal);

            // Get the counterparty account
            AccountID accountID = sleItem->getAccountID(sfOwner);

            // Add to our result array
            sealedOptions.push_back(
                {offerIndex, accountID, quantityToSeal, offerPremium});

            // Get or create sealed options array for the counterparty's offer
            STArray sealedOptionsArray =
                sleItem->isFieldPresent(sfSealedOptions)
                ? sleItem->getFieldArray(sfSealedOptions)
                : STArray();

            // Create a new sealed option entry for the counterparty
            STObject sealedOption(sfSealedOption);
            sealedOption.setAccountID(
                sfOwner, account);  // Set our account as owner
            sealedOption.setFieldH256(
                sfOptionOfferID, optionIndex);  // Reference our option
            sealedOption.setFieldU32(
                sfQuantity, quantityToSeal);  // Set quantity

            // Add the sealed option to the array
            sealedOptionsArray.push_back(std::move(sealedOption));

            // Update the offer with the new sealed options array
            sleItem->setFieldArray(
                sfSealedOptions, std::move(sealedOptionsArray));

            // Apply changes to the ledger
            sb.update(sleItem);

            // Update total matched quantity
            totalSealedQuantity += quantityToSeal;

            // If we've matched all we need, return the results
            if (totalSealedQuantity >= desiredQuantity)
                return sealedOptions;

        } while (cdirNext(
            sb, sleOfferDir->key(), sleOfferDir, bookEntry, offerIndex));

        // Move to the next page in the directory
        key = sb.succ(sleOfferDir->key(), uBookEndOpp);
    }

    // Return whatever matches we found (may be less than requested)
    return sealedOptions;
}

/**
 * @brief Creates a new option offer on the ledger.
 *
 * This function sets up a new option offer with all required fields, including
 * strike price, premium, expiration date, and relationships to any sealed
 * options. It adds the option to the owner directory and the appropriate book
 * directory for other users to find.
 *
 * @param sb Sandbox ledger view
 * @param account The account creating the offer
 * @param optionOfferKeylet Keylet for the option offer to be created
 * @param flags Option flags (including tfPut, tfSell)
 * @param quantity Total quantity of the option
 * @param openInterest Available quantity not yet matched
 * @param premium The premium (price) for the option
 * @param isSell Whether this is a sell offer
 * @param issue The underlying asset (currency and issuer)
 * @param strikePrice The strike price as an STAmount
 * @param strike The strike price as an integer
 * @param expiration Expiration time of the option
 * @param optionBookDirKeylet Keylet for the option book directory
 * @param sealedOptions Vector of sealed options to include with this offer
 * @param j Journal for logging
 * @return TER Transaction result code
 */
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
    Issue const& issue,
    STAmount strikePrice,
    std::int64_t strike,
    std::uint32_t expiration,
    Keylet const& optionBookDirKeylet,
    std::vector<SealedOptionData> const& sealedOptions,
    beast::Journal j)
{
    // Log the parameters for debugging
    JLOG(j.trace()) << "OptionUtils.createOffer: account=" << to_string(account)
                    << ", strikePrice=" << strikePrice
                    << ", expiration=" << expiration
                    << ", quantity=" << quantity
                    << ", openInterest=" << openInterest
                    << ", premium=" << premium << ", isSell=" << isSell;

    // Verify the account exists
    auto sleSrcAcc = sb.peek(keylet::account(account));
    if (!sleSrcAcc)
        return terNO_ACCOUNT;

    // Check if account has sufficient reserve
    // (Account needs reserve for each ledger object they own)
    XRPAmount const reserve =
        sb.fees().accountReserve(sleSrcAcc->getFieldU32(sfOwnerCount) + 1);
    XRPAmount const sourceBalance = sleSrcAcc->getFieldAmount(sfBalance).xrp();
    if (sourceBalance < reserve)
        return tecINSUFFICIENT_RESERVE;

    // Increase owner count (since we're adding an object owned by this account)
    adjustOwnerCount(sb, sleSrcAcc, 1, j);

    // Create new option offer SLE (Serialized Ledger Entry)
    auto optionOffer = std::make_shared<SLE>(optionOfferKeylet);

    // Add the option to the owner directory
    auto const page = sb.dirInsert(
        keylet::ownerDir(account),
        optionOfferKeylet,
        describeOwnerDir(account));

    // Check if owner directory is full
    if (!page)
    {
        JLOG(j.trace()) << "final result: failed to add offer to owner dir";
        return tecDIR_FULL;
    }

    // Set all option fields
    optionOffer->setFlag(flags);  // Option flags (put/call, buy/sell)
    optionOffer->setAccountID(sfOwner, account);   // Owner account
    optionOffer->setFieldU64(sfOwnerNode, *page);  // Owner directory node
    optionOffer->setFieldAmount(sfStrikePrice, strikePrice);  // Strike price
    optionOffer->setFieldIssue(
        sfAsset, STIssue{sfAsset, issue});               // Underlying asset
    optionOffer->setFieldU32(sfExpiration, expiration);  // Expiration timestamp
    optionOffer->setFieldU32(sfQuantity, quantity);      // Total quantity
    optionOffer->setFieldU32(
        sfOpenInterest, openInterest);                // Available quantity
    optionOffer->setFieldAmount(sfPremium, premium);  // Premium (price)

    // Get or create sealed options array
    STArray sealedOptionsArray = optionOffer->isFieldPresent(sfSealedOptions)
        ? optionOffer->getFieldArray(sfSealedOptions)
        : STArray();

    // Add all sealed options to the array
    for (const auto& sealedOption : sealedOptions)
    {
        AccountID const& accountID = sealedOption.account;
        std::uint32_t quantityToSeal = sealedOption.quantitySealed;

        // Create a new sealed option object
        STObject sealedOptionObj(sfSealedOption);
        sealedOptionObj.setAccountID(
            sfOwner, accountID);  // Counterparty account
        sealedOptionObj.setFieldH256(
            sfOptionOfferID, sealedOption.offerID);  // Counterparty's offer
        sealedOptionObj.setFieldU32(
            sfQuantity, quantityToSeal);  // Sealed quantity

        // Add to the array
        sealedOptionsArray.push_back(std::move(sealedOptionObj));
    }

    // Update the offer with sealed options
    optionOffer->setFieldArray(sfSealedOptions, std::move(sealedOptionsArray));

    // Create option structure for the order book
    Option const option{issue, static_cast<uint64_t>(strike), expiration};

    // Calculate the directory path based on quality (premium)
    auto dir = keylet::optionQuality(optionBookDirKeylet, premium.mantissa());

    // Add the option to the book directory
    auto const bookNode =
        sb.dirAppend(dir, optionOfferKeylet, [&](SLE::ref sle) {
            // Set directory metadata
            sle->setFieldH160(
                sfTakerPaysIssuer, issue.account);  // Asset issuer
            sle->setFieldH160(
                sfTakerPaysCurrency, issue.currency);    // Asset currency
            sle->setFieldU64(sfStrike, strike);          // Strike price
            sle->setFieldU32(sfExpiration, expiration);  // Expiration time
            sle->setFieldU64(
                sfExchangeRate, premium.mantissa());  // Exchange rate (premium)
        });

    // Check if book directory is full
    if (!bookNode)
    {
        JLOG(j.trace()) << "final result: failed to add offer to book";
        return tecDIR_FULL;
    }

    // Set book directory references
    optionOffer->setFieldH256(sfBookDirectory, dir.key);  // Book directory
    optionOffer->setFieldU64(sfBookNode, *bookNode);      // Book node

    // Insert the new offer into the ledger
    sb.insert(optionOffer);
    return tesSUCCESS;
}

/**
 * @brief Transfers tokens from one account to another.
 *
 * Used for premium payments and settlements when options are exercised.
 * This function handles both XRP and issued currencies, verifying sufficient
 * funds before initiating the transfer.
 *
 * @param sb Sandbox ledger view
 * @param sender Account sending tokens
 * @param receiver Account receiving tokens
 * @param amount Amount to transfer
 * @param j Journal for logging
 * @return TER Transaction result code
 */
TER
transferTokens(
    Sandbox& sb,
    AccountID const& sender,
    AccountID const& receiver,
    STAmount const& amount,
    beast::Journal j)
{
    // Handle XRP transfers
    if (isXRP(amount))
    {
        // Log the operation
        JLOG(j.trace()) << "OptionSettle: XRP transfer: " << amount;

        // Load the sender's account
        auto sleSender = sb.read(keylet::account(sender));
        if (!sleSender)
            return terNO_ACCOUNT;  // Sender account not found

        // Check if sender has sufficient balance
        STAmount senderBalance = sleSender->getFieldAmount(sfBalance);
        if (senderBalance < amount.xrp())
            return tecUNFUNDED_PAYMENT;
    }
    // Handle IOU (issued currency) transfers
    else
    {
        // Log the operation
        JLOG(j.trace()) << "OptionSettle: IOU transfer: " << amount;

        // Check how much of this currency the sender can spend
        STAmount spendableAmount{accountHolds(
            sb,
            sender,
            amount.getCurrency(),
            amount.getIssuer(),
            fhZERO_IF_FROZEN,  // Return zero if the account is frozen
            j)};

        // Check if sender has sufficient balance of this currency
        if (spendableAmount < amount)
        {
            JLOG(j.trace()) << "OptionSettle: Insufficient funds."
                            << spendableAmount << " < " << amount;
            return tecINSUFFICIENT_FUNDS;
        }
    }

    // Perform the actual transfer (handles both XRP and IOUs)
    // WaiveTransferFee::No means transfer fees are not waived
    if (TER result =
            accountSend(sb, sender, receiver, amount, j, WaiveTransferFee::No);
        !isTesSuccess(result))
        return result;  // Return any error from accountSend

    return tesSUCCESS;
}

/**
 * @brief Closes an existing option offer.
 *
 * This function handles the complex process of closing an option offer, which
 * includes:
 * 1. Verifying ownership and unlocking any collateral for sell offers
 * 2. For options with sealed relationships, finding replacement counterparties
 * 3. Updating counterparty offers to maintain their positions
 * 4. For buy positions, handling payments from new buyers
 * 5. Finally removing the offer from the ledger
 *
 * @param sb Sandbox ledger view
 * @param pseudoAccount OptionPair account (where tokens are locked)
 * @param account Account closing the offer
 * @param offerKeylet Keylet of the offer to close
 * @param isPut Whether this is a put option
 * @param isSell Whether this is a sell offer
 * @param issue The underlying asset
 * @param strike The strike price
 * @param expiration The expiration time
 * @param j Journal for logging
 * @return TER Transaction result code
 */
TER
closeOffer(
    Sandbox& sb,
    AccountID const& pseudoAccount,
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

    // Release margin if a margin position is linked
    if (sleOffer->isFieldPresent(sfMarginPositionID))
    {
        uint256 const positionID = sleOffer->getFieldH256(sfMarginPositionID);
        auto slePosition = sb.peek(Keylet{ltMARGIN_POSITION, positionID});
        if (slePosition)
        {
            // Deduct accumulated funding and get net margin
            std::uint32_t const now =
                sb.parentCloseTime().time_since_epoch().count();
            Number const netMargin =
                margin::deductFundingAndRelease(sb, slePosition, now);

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

            // Remove position from owner directory and delete
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
                sb, sb.peek(keylet::account(posAccount)), -1, j);
            sb.erase(slePosition);

            JLOG(j.trace())
                << "OptionUtils: Released margin and deleted position "
                << to_string(positionID);
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

    // Define a structure to track counterparty information
    struct CounterpartyInfo
    {
        std::shared_ptr<SLE> option;  // The counterparty's option SLE
        std::uint32_t totalQuantity =
            0;  // Total quantity sealed with this counterparty
        std::vector<std::size_t>
            sealedIndices;  // Indices in the original array
    };

    // Map to track counterparty options and their sealed quantities
    std::map<uint256, CounterpartyInfo> counterpartyMap;
    STArray sealedOptionsArray = sleOffer->getFieldArray(sfSealedOptions);

    // First pass: Group by counterparty option and sum quantities
    for (std::size_t i = 0; i < sealedOptionsArray.size(); ++i)
    {
        auto& sealedOption = sealedOptionsArray[i];

        // Get the counterparty's offer ID
        uint256 const cOfferID = sealedOption.getFieldH256(sfOptionOfferID);

        // Get the sealed quantity
        std::uint32_t sealedQuantity = sealedOption.getFieldU32(sfQuantity);

        // Find the counterparty's option
        auto cKeylet = keylet::unchecked(cOfferID);
        auto cOption = sb.peek(cKeylet);

        // Make sure the counterparty option exists
        if (!cOption)
        {
            JLOG(j.trace()) << "OptionUtils: Counterparty option not found: "
                            << to_string(cOfferID);
            return tecNO_ENTRY;
        }

        // Add to our map, grouping by counterparty offer ID
        if (counterpartyMap.find(cOfferID) == counterpartyMap.end())
        {
            // First entry for this counterparty
            counterpartyMap[cOfferID] = {cOption, sealedQuantity, {i}};
        }
        else
        {
            // Add to existing entry
            counterpartyMap[cOfferID].totalQuantity += sealedQuantity;
            counterpartyMap[cOfferID].sealedIndices.push_back(i);
        }
    }

    JLOG(j.trace()) << "OptionUtils: Counterparty options found: "
                    << counterpartyMap.size();

    // Get the original premium as a limit price reference
    STAmount limitPrice = sleOffer->getFieldAmount(sfPremium);

    // For each unique counterparty option, find replacement offers
    for (auto& [cOfferID, cInfo] : counterpartyMap)
    {
        auto cOption = cInfo.option;  // Counterparty's option SLE
        std::uint32_t totalQuantity =
            cInfo.totalQuantity;  // Total quantity to replace
        AccountID counterpartyAccount =
            cOption->getAccountID(sfOwner);  // Counterparty account

        // Create a vector of option IDs to exclude when matching
        std::vector<uint256> excludeOptionIDs;
        excludeOptionIDs.push_back(
            cOfferID);  // Exclude the counterparty option itself

        // Use updated matchOptions with market order parameter
        bool isMarketOrder = true;  // Default to market orders for now
        std::vector<SealedOptionData> newMatches;

        // Custom implementation to find matches while excluding specific offers
        // Get the base of the option book directory for this asset, strike, and
        // expiration
        const uint256 uBookBaseOpp = getOptionBookBase(
            issue.account, issue.currency, strike, expiration);
        const uint256 uBookEndOpp = getOptionQualityNext(uBookBaseOpp);
        auto key = sb.succ(uBookBaseOpp, uBookEndOpp);

        std::uint32_t totalMatchedQuantity = 0;

        // Similar to matchOptions but with exclusion logic
        // Find replacement offers for this counterparty
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

                // Looking for the same option type and same side
                auto const flags = sleItem->getFlags();

                // Match same type of option (put/call)
                auto const offerPut = (flags & tfPut) != 0;
                if (isPut != offerPut)
                {
                    JLOG(j.trace()) << "OptionUtils: Option type mismatch: "
                                    << to_string(offerIndex);
                    continue;  // Option type mismatch
                }

                // For closeOffer replacement, match same side
                auto const offerSell = (flags & tfSell) != 0;
                if (isSell != offerSell)
                {
                    JLOG(j.trace()) << "OptionUtils: Offer side mismatch: "
                                    << to_string(offerIndex);
                    continue;  // Side mismatch
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

                // Calculate how much of this offer we can match
                uint32_t availableQuantity =
                    sleItem->getFieldU32(sfOpenInterest);
                uint32_t quantityToSeal = std::min(
                    availableQuantity, totalQuantity - totalMatchedQuantity);

                // Update the option's open interest
                sleItem->setFieldU32(
                    sfOpenInterest, availableQuantity - quantityToSeal);

                // Get the counterpart account ID
                AccountID accountID = sleItem->getAccountID(sfOwner);

                // Add to our new matches list
                newMatches.push_back(
                    {offerIndex, accountID, quantityToSeal, offerPremium});

                // Set up sealed option relationship with the counterparty
                STArray sealedOptionsArray =
                    sleItem->isFieldPresent(sfSealedOptions)
                    ? sleItem->getFieldArray(sfSealedOptions)
                    : STArray();

                // Create a new sealed option entry
                STObject sealedOption(sfSealedOption);
                sealedOption.setAccountID(
                    sfOwner, counterpartyAccount);  // Set owner to counterparty
                sealedOption.setFieldH256(
                    sfOptionOfferID,
                    cOfferID);  // Reference counterparty's option
                sealedOption.setFieldU32(
                    sfQuantity, quantityToSeal);  // Set quantity

                // Add to the sealed options array
                sealedOptionsArray.push_back(std::move(sealedOption));
                sleItem->setFieldArray(
                    sfSealedOptions, std::move(sealedOptionsArray));

                // Update the ledger
                sb.update(sleItem);

                // Track total matched quantity
                totalMatchedQuantity += quantityToSeal;

                // If we've matched all we need, break out
                if (totalMatchedQuantity >= totalQuantity)
                    break;

            } while (cdirNext(
                sb, sleOfferDir->key(), sleOfferDir, bookEntry, offerIndex));

            // Move to next page in the directory
            key = sb.succ(sleOfferDir->key(), uBookEndOpp);
        }

        // If we couldn't find enough matches, fail the transaction
        if (totalMatchedQuantity < totalQuantity)
        {
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

                        // Keep all sealed options except the one we just added
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

                    // Update the ledger
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

            // Update the counterparty's option with the new sealed options
            cOption->setFieldArray(sfSealedOptions, updatedSealedOptions);
            sb.update(cOption);

            JLOG(j.trace()) << "OptionUtils: Updated counterparty option "
                            << to_string(cOfferID) << " with "
                            << newMatches.size() << " new matches.";
        }
    }

    // Finally, remove the option being closed
    if (auto ter = deleteOffer(sb, sleOffer, j); ter != tesSUCCESS)
    {
        JLOG(j.trace()) << "OptionUtils: Failed to delete offer.";
        return ter;
    }

    return tesSUCCESS;
}

/**
 * @brief Exercises an option contract via cash settlement.
 *
 * Calculates settlement based on mark price vs strike price.
 * For calls: settlement = max(0, (markPrice - strikePrice) * quantity)
 * For puts:  settlement = max(0, (strikePrice - markPrice) * quantity)
 * Settlement is transferred between buyer and seller margin positions.
 *
 * @param sb Sandbox ledger view
 * @param isPut Whether this is a put option
 * @param strikePrice The strike price of the option
 * @param buyer Account exercising the option
 * @param issue The underlying (base) asset
 * @param quoteIssue The quote (settlement) asset
 * @param sealedOptions Array of sealed options to exercise
 * @param j Journal for logging
 * @return TER Transaction result code
 */
TER
exerciseOffer(
    Sandbox& sb,
    bool isPut,
    STAmount const& strikePrice,
    AccountID const& buyer,
    Issue const& issue,
    Issue const& quoteIssue,
    STArray const& sealedOptions,
    beast::Journal j)
{
    // Get mark price from oracle via OptionPair
    auto const slePair = sb.read(keylet::optionPair(issue, quoteIssue));
    Number const markPrice =
        slePair ? margin::getMarkPrice(sb, slePair) : Number(0);
    if (markPrice == Number(0))
    {
        JLOG(j.warn()) << "OptionUtils: Cannot get mark price for exercise.";
        return tecFAILED_PROCESSING;
    }

    Number const strike = Number(strikePrice);

    // Process each sealed option in the array
    for (const auto& sealedOption : sealedOptions)
    {
        // Get the counterparty (seller) account
        AccountID const seller = sealedOption.getAccountID(sfOwner);

        // Get the option offer ID
        uint256 const offerID = sealedOption.getFieldH256(sfOptionOfferID);

        // Load the counterparty's option offer
        auto sleSealedOffer = sb.peek(keylet::optionOffer(offerID));
        if (!sleSealedOffer)
            return tecNO_TARGET;

        // Get the quantity being exercised
        std::uint32_t const quantity = sealedOption.getFieldU32(sfQuantity);

        // Calculate cash settlement
        // For calls: buyer profits when price rises above strike
        // For puts:  buyer profits when price falls below strike
        Number settlement(0);
        if (isPut)
        {
            if (strike > markPrice)
                settlement = (strike - markPrice) * Number(quantity);
        }
        else
        {
            if (markPrice > strike)
                settlement = (markPrice - strike) * Number(quantity);
        }

        JLOG(j.trace()) << "OptionUtils: Exercise settlement=" << settlement
                        << " markPrice=" << markPrice
                        << " strikePrice=" << strike
                        << " quantity=" << quantity;

        // Transfer settlement via margin positions
        if (settlement > Number(0))
        {
            // Find seller's margin position (linked to their offer)
            if (sleSealedOffer->isFieldPresent(sfMarginPositionID))
            {
                uint256 const sellerPosID =
                    sleSealedOffer->getFieldH256(sfMarginPositionID);
                auto sleSellerPos =
                    sb.peek(Keylet{ltMARGIN_POSITION, sellerPosID});
                if (sleSellerPos)
                {
                    // Deduct settlement from seller's margin
                    uint256 const sellerMarginAcctID =
                        sleSellerPos->getFieldH256(sfMarginAccountID);
                    auto sleSellerMargin =
                        sb.peek(Keylet{ltMARGIN_ACCOUNT, sellerMarginAcctID});
                    if (sleSellerMargin)
                    {
                        // Deduct accumulated funding and get net margin
                        std::uint32_t const nowExercise =
                            sb.parentCloseTime().time_since_epoch().count();
                        Number const netMargin =
                            margin::deductFundingAndRelease(
                                sb, sleSellerPos, nowExercise);

                        Number sellerBalance =
                            sleSellerMargin->at(~sfCollateralBalance)
                                .value_or(Number(0));
                        sellerBalance =
                            sellerBalance + netMargin - settlement;
                        // Clamp to zero (insurance fund covers deficit)
                        if (sellerBalance < Number(0))
                            sellerBalance = Number(0);
                        sleSellerMargin->at(sfCollateralBalance) =
                            STNumber{sfCollateralBalance, sellerBalance};
                        sb.update(sleSellerMargin);
                    }

                    // Delete seller's margin position
                    auto const posAccount =
                        sleSellerPos->getAccountID(sfAccount);
                    if (sleSellerPos->isFieldPresent(sfOwnerNode))
                    {
                        sb.dirRemove(
                            keylet::ownerDir(posAccount),
                            sleSellerPos->getFieldU64(sfOwnerNode),
                            sleSellerPos->key(),
                            true);
                    }
                    adjustOwnerCount(
                        sb, sb.peek(keylet::account(posAccount)), -1, j);
                    sb.erase(sleSellerPos);
                }
            }

            // Credit settlement to buyer's margin account
            // Find buyer's margin account (look up from buyer's offers or
            // use the OptionPair to find it)
            auto const sleBuyerAcct = sb.read(keylet::account(buyer));
            if (sleBuyerAcct)
            {
                // Transfer settlement as direct token transfer to buyer
                STAmount settlementAmount(quoteIssue,
                    static_cast<std::int64_t>(settlement), 0);
                if (isXRP(quoteIssue))
                {
                    // XRP settlement: mint XRP to buyer from margin pool.
                    // The corresponding XRP was burned on margin deposit.
                    auto sleBuyerPeek = sb.peek(keylet::account(buyer));
                    if (sleBuyerPeek)
                    {
                        STAmount balance =
                            sleBuyerPeek->getFieldAmount(sfBalance);
                        balance += settlementAmount.xrp();
                        sleBuyerPeek->setFieldAmount(sfBalance, balance);
                        sb.update(sleBuyerPeek);
                        sb.rawDestroyXRP(-settlementAmount.xrp());
                    }
                }
                else
                {
                    auto const ter = accountSend(
                        sb, seller, buyer, settlementAmount, j);
                    if (!isTesSuccess(ter))
                        return ter;
                }
            }
        }

        // Delete the counterparty's offer
        if (auto ter3 = option::deleteOffer(sb, sleSealedOffer, j);
            ter3 != tesSUCCESS)
        {
            JLOG(j.trace())
                << "OptionUtils: Failed to delete offer after exercise.";
            return ter3;
        }
    }
    return tesSUCCESS;
}

/**
 * @brief Handles expiration of an option offer.
 *
 * This function is called when an option expires. It returns any locked
 * collateral to the seller, updates counterparty options to remove references
 * to the expired option, and removes the option from the ledger.
 *
 * @param view Ledger view
 * @param sle SLE of the expired option
 * @param j Journal for logging
 * @return TER Transaction result code
 */
TER
expireOffer(ApplyView& view, std::shared_ptr<SLE> const& sle, beast::Journal j)
{
    // Get the owner account
    AccountID const account = (*sle)[sfOwner];

    // Get the owner's account SLE
    auto const sleAccount = view.peek(keylet::account(account));
    if (!sleAccount)
        return tecINTERNAL;  // LCOV_EXCL_LINE

    // Get the owner's account SLE
    Issue const issue = sle->getFieldIssue(sfAsset).value().get<Issue>();
    Issue const issue2 = sle->getFieldAmount(sfStrikePrice).issue();
    auto const slePair = view.read(keylet::optionPair(issue, issue2));
    if (!slePair)
        return tecINTERNAL;  // LCOV_EXCL_LINE

    // Get the option ID
    uint256 const offerID = sle->key();

    // Release margin if a margin position is linked
    if (sle->isFieldPresent(sfMarginPositionID))
    {
        uint256 const positionID = sle->getFieldH256(sfMarginPositionID);
        auto slePosition = view.peek(Keylet{ltMARGIN_POSITION, positionID});
        if (slePosition)
        {
            // Deduct accumulated funding and get net margin
            std::uint32_t const nowExpire =
                view.parentCloseTime().time_since_epoch().count();
            Number const netMargin =
                margin::deductFundingAndRelease(view, slePosition, nowExpire);

            // Release net margin back to margin account
            uint256 const marginAccountID =
                slePosition->getFieldH256(sfMarginAccountID);
            auto sleMarginAcct =
                view.peek(Keylet{ltMARGIN_ACCOUNT, marginAccountID});
            if (sleMarginAcct)
            {
                Number collateralBalance =
                    sleMarginAcct->at(~sfCollateralBalance).value_or(Number(0));
                collateralBalance = collateralBalance + netMargin;
                sleMarginAcct->at(sfCollateralBalance) =
                    STNumber{sfCollateralBalance, collateralBalance};
                view.update(sleMarginAcct);
            }

            // Remove position from owner directory and delete
            auto const posAccount = slePosition->getAccountID(sfAccount);
            if (slePosition->isFieldPresent(sfOwnerNode))
            {
                view.dirRemove(
                    keylet::ownerDir(posAccount),
                    slePosition->getFieldU64(sfOwnerNode),
                    slePosition->key(),
                    true);
            }
            adjustOwnerCount(
                view, view.peek(keylet::account(posAccount)), -1, j);
            view.erase(slePosition);

            JLOG(j.trace())
                << "OptionUtils: Released margin for expired option "
                << to_string(positionID);
        }
    }

    // Handle sealed options, if any
    if (sle->isFieldPresent(sfSealedOptions) &&
        !sle->getFieldArray(sfSealedOptions).empty())
    {
        // Get the sealed options array
        STArray sealedOptions = sle->getFieldArray(sfSealedOptions);

        // For each sealed option, update the counterparty's option
        for (auto const& sealedOption : sealedOptions)
        {
            // Get the counterparty's offer ID
            uint256 const cOfferID = sealedOption.getFieldH256(sfOptionOfferID);

            // Get the counterparty's option
            auto cKeylet = keylet::unchecked(cOfferID);
            auto cOption = view.peek(cKeylet);

            // If counterparty option exists and has sealed options
            if (cOption && cOption->isFieldPresent(sfSealedOptions))
            {
                // Get the counterparty's sealed options array
                STArray cSealedOptions =
                    cOption->getFieldArray(sfSealedOptions);
                STArray updatedCSealed;

                // Remove references to the expired option from counterparty's
                // sealed options
                for (auto const& cso : cSealedOptions)
                {
                    // Keep all sealed options except those referencing the
                    // expired offer
                    if (!cso.isFieldPresent(sfOptionOfferID) ||
                        cso.getFieldH256(sfOptionOfferID) != offerID)
                    {
                        updatedCSealed.push_back(cso);
                    }
                }

                // Update the counterparty's option with the filtered array
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

/**
 * @brief Removes an option offer from the ledger.
 *
 * This function handles the complete deletion of an option offer, which
 * includes removing it from the owner directory, the book directory, adjusting
 * the owner count, and erasing the ledger entry.
 *
 * @param view Ledger view
 * @param sle SLE of the option to delete
 * @param j Journal for logging
 * @return TER Transaction result code
 */
TER
deleteOffer(ApplyView& view, std::shared_ptr<SLE> const& sle, beast::Journal j)
{
    // Validate that we have an SLE
    if (!sle)
        return tecINTERNAL;  // LCOV_EXCL_LINE

    // Get the owner account
    AccountID const account = (*sle)[sfOwner];

    // Get the owner's account SLE
    auto const sleAccount = view.peek(keylet::account(account));
    if (!sleAccount)
        return tecINTERNAL;  // LCOV_EXCL_LINE

    // Remove from owner directory
    // (this removes the link between the account and the option offer)
    if (!view.dirRemove(
            keylet::ownerDir(account), (*sle)[sfOwnerNode], sle->key(), true))
    {
        // LCOV_EXCL_START
        JLOG(j.trace()) << "Unable to delete OptionOffer from owner.";
        return tefBAD_LEDGER;
        // LCOV_EXCL_STOP
    }

    // If the offer is in a book directory, remove it from there as well
    if (sle->isFieldPresent(sfBookDirectory) && sle->isFieldPresent(sfBookNode))
    {
        // Remove from book directory
        // (this removes the offer from the public order book)
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

    // Decrease the owner count
    // (this adjusts the reserve requirement for the account)
    adjustOwnerCount(view, sleAccount, -1, j);

    // Finally, erase the option offer from the ledger
    view.erase(sle);

    return tesSUCCESS;
}

}  // namespace option
}  // namespace xrpl
