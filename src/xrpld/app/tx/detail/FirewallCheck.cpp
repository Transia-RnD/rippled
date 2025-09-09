//------------------------------------------------------------------------------
/*
  This file is part of rippled: https://github.com/ripple/rippled
  Copyright (c) 2012-2016 Ripple Labs Inc.

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

#include <xrpld/app/misc/FirewallUtils.h>
#include <xrpld/app/tx/detail/FirewallCheck.h>
#include <xrpld/app/tx/detail/NFTokenUtils.h>
#include <xrpld/ledger/ReadView.h>
#include <xrpld/ledger/View.h>

#include <xrpl/basics/Log.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/FeeUnits.h>
#include <xrpl/protocol/STArray.h>
#include <xrpl/protocol/STData.h>
#include <xrpl/protocol/SystemParameters.h>
#include <xrpl/protocol/TxFormats.h>

namespace ripple {

//------------------------------------------------------------------------------

void
AccountRootBalance::visitEntry(
    bool isDelete,
    std::shared_ptr<SLE const> const& before,
    std::shared_ptr<SLE const> const& after)
{
    if (after->getType() == ltACCOUNT_ROOT &&
        (!before || before->getType() == ltACCOUNT_ROOT))
    {
        auto const getBalance =
            [](auto const& sle, auto const& other, bool zero) {
                STAmount amt =
                    sle ? sle->at(sfBalance) : other->at(sfBalance).zeroed();
                return zero ? amt.zeroed() : amt;
            };
        STAmount const beforeField = getBalance(before, after, false);
        STAmount const afterField = getBalance(after, before, isDelete);
        STAmount const changeAmount = afterField < beforeField
            ? beforeField - afterField
            : afterField - beforeField;

        AccountID const account = isDelete ? before->getAccountID(sfAccount)
                                           : after->getAccountID(sfAccount);
        balanceChanges_.emplace_back(BalanceChange{account, changeAmount});
    }
}

bool
AccountRootBalance::finalize(
    STTx const& tx,
    TER const,
    XRPAmount const fee,
    ApplyView& view,
    beast::Journal const& j)
{
    if (tx.getTxnType() == ttFIREWALL_SET)
        return true;

    for (auto const& change : balanceChanges_)
    {
        auto const& account = change.account;
        auto const& balanceChange = change.balanceChange;
        if (!view.exists(keylet::firewall(account)))
            continue;

        auto const sleFirewall = view.peek(keylet::firewall(account));
        if (!sleFirewall->isFieldPresent(sfFirewallRules))
            continue;

        STArray const firewallRules =
            sleFirewall->getFieldArray(sfFirewallRules);
        auto const leRules = getFirewallRules(firewallRules, ltACCOUNT_ROOT);
        if (leRules.size() == 0)
            continue;

        for (auto const& rule : leRules)
        {
            auto const fieldCode = rule.getFieldU32(sfFieldCode);
            SField const& fieldType = ripple::SField::getField(fieldCode);

            if (!Firewall::getInstance().handlesField(
                    ltACCOUNT_ROOT, fieldType))
                continue;

            if (AccountRootBalance::handleRule(
                    view, sleFirewall, rule, balanceChange))
                return false;
        }
    }
    return true;
}

bool
AccountRootBalance::handleRule(
    ApplyView& view,
    SLE::pointer const& sleFirewall,
    STObject const& rule,
    STAmount const& changeAmount)
{
    ripple::STData const& value = rule.getFieldData(sfFirewallValue);
    STAmount const& ruleValue = value.getFieldAmount();
    std::uint16_t const operatorCode = rule.getFieldU16(sfComparisonOperator);

    bool const hasTimeLimit = rule.isFieldPresent(sfTimePeriod) &&
        rule.isFieldPresent(sfTimeStart) && rule.isFieldPresent(sfTimeValue);

    if (hasTimeLimit)
    {
        std::uint32_t const currentTime =
            view.parentCloseTime().time_since_epoch().count();
        std::uint32_t const startTime = rule.getFieldU32(sfTimeStart);
        std::uint32_t const timePeriod = rule.getFieldU32(sfTimePeriod);
        ripple::STData const& value = rule.getFieldData(sfTimeValue);
        STAmount total = value.getFieldAmount();

        if (startTime == 0 || (currentTime - startTime > timePeriod))
        {
            resetRuleTimer(view, sleFirewall, rule, currentTime);
            total = changeAmount;
        }
        else
        {
            total += changeAmount;
        }

        if (!evaluateComparison(total, ruleValue, operatorCode))
        {
            updateRuleTotal(view, sleFirewall, rule, total);
            return true;
        }
    }
    else
    {
        if (!evaluateComparison(changeAmount, ruleValue, operatorCode))
            return true;
    }

    return false;
}

void
AccountRootBalance::updateRuleTotal(
    ApplyView& view,
    SLE::pointer const& sleFirewall,
    STObject const& rule,
    STAmount const& totalOut)
{
    STArray firewallRules = sleFirewall->getFieldArray(sfFirewallRules);
    auto const fieldCode = rule.getFieldU32(sfFieldCode);

    auto it = std::find_if(
        firewallRules.begin(),
        firewallRules.end(),
        [&fieldCode](auto& existingRule) {
            return existingRule.getFieldU32(sfFieldCode) == fieldCode;
        });

    if (it != firewallRules.end())
        it->setFieldData(sfFirewallValue, STData{sfFirewallValue, totalOut});

    sleFirewall->setFieldArray(sfFirewallRules, firewallRules);
    view.update(sleFirewall);
}

void
AccountRootBalance::resetRuleTimer(
    ApplyView& view,
    SLE::pointer const& sleFirewall,
    STObject const& rule,
    std::uint32_t const& currentTime)
{
    STArray firewallRules = sleFirewall->getFieldArray(sfFirewallRules);
    auto const fieldCode = rule.getFieldU32(sfFieldCode);

    auto it = std::find_if(
        firewallRules.begin(),
        firewallRules.end(),
        [&fieldCode](auto& existingRule) {
            return existingRule.getFieldU32(sfFieldCode) == fieldCode;
        });

    if (it != firewallRules.end())
        it->setFieldU32(sfTimeStart, currentTime);

    sleFirewall->setFieldArray(sfFirewallRules, firewallRules);
    view.update(sleFirewall);
}

//------------------------------------------------------------------------------

bool
ValidWithdraw::isValidEntry(
    std::shared_ptr<SLE const> const& before,
    std::shared_ptr<SLE const> const& after)
{
    // `after` can never be null, even if the trust line is deleted.
    XRPL_ASSERT(
        after, "ripple::TransfersNotFrozen::isValidEntry : valid after.");
    if (!after)
        return false;

    if (after->getType() == ltACCOUNT_ROOT)
        return true;

    if (after->getType() == ltRIPPLE_STATE &&
        (!before || before->getType() == ltRIPPLE_STATE))
        return true;

    if (after->getType() == ltESCROW &&
        (!before || before->getType() == ltESCROW))
        return true;

    if (after->getType() == ltPAYCHAN &&
        (!before || before->getType() == ltPAYCHAN))
        return true;

    if (after->getType() == ltNFTOKEN_OFFER &&
        (!before || before->getType() == ltNFTOKEN_OFFER))
        return true;

    if (after->getType() == ltNFTOKEN_PAGE &&
        (!before || before->getType() == ltNFTOKEN_PAGE))
        return true;

    if (after->getType() == ltMPTOKEN &&
        (!before || before->getType() == ltMPTOKEN))
        return true;

    return false;
}

STAmount
ValidWithdraw::calculateBalanceChange(
    std::shared_ptr<SLE const> const& before,
    std::shared_ptr<SLE const> const& after,
    bool isDelete)
{
    auto const getBalance = [](auto const& sle, auto const& other, bool zero) {
        STAmount amt = sle ? sle->at(sfBalance) : other->at(sfBalance).zeroed();
        return zero ? amt.zeroed() : amt;
    };

    if (after->getType() == ltACCOUNT_ROOT ||
        after->getType() == ltRIPPLE_STATE)
    {
        auto const balanceBefore = getBalance(before, after, false);
        auto const balanceAfter = getBalance(after, before, isDelete);
        return balanceAfter - balanceBefore;
    }

    // Creating an Escrow or PayChan results in a new object with an sfAmount
    // field instead of an sfBalance field.
    auto const getObjectAmount = [](auto const& sle,
                                    auto const& other,
                                    bool zero) {
        STAmount amt = sle ? sle->at(sfAmount) : other->at(sfAmount).zeroed();
        return zero ? amt.zeroed() : amt;
    };

    if (after->getType() == ltESCROW || after->getType() == ltPAYCHAN)
    {
        auto const balanceBefore = getObjectAmount(before, after, false);
        auto const balanceAfter = getObjectAmount(after, before, isDelete);
        return balanceAfter - balanceBefore;
    }

    auto const getMPTAmount = [](auto const& sle,
                                    auto const& other,
                                    bool zero) {
        std::uint64_t amt = sle ? sle->at(sfMPTAmount) : other->at(sfMPTAmount);
        return zero ? 0 : amt;
    };

    if (after->getType() == ltMPTOKEN)
    {
        auto const balanceBefore = getMPTAmount(before, after, false);
        auto const balanceAfter = getMPTAmount(after, before, isDelete);
        
        // Get the MPTIssue from the MPToken entry
        auto const mptID = after->at(sfMPTokenIssuanceID);
        MPTIssue mptIssue(mptID);
        
        // Convert to STAmount for consistent handling
        int64_t diff = static_cast<int64_t>(balanceAfter) - static_cast<int64_t>(balanceBefore);
        // Create STAmount using the explicit constructor
        if (diff < 0)
            return STAmount(mptIssue, static_cast<std::uint64_t>(-diff), 0, true);
        else
            return STAmount(mptIssue, static_cast<std::uint64_t>(diff), 0, false);
    }

    // NFTokenPage doesn't directly track balance changes
    if (after->getType() == ltNFTOKEN_PAGE)
    {
        return STAmount{};
    }

    return STAmount{};
}

void
ValidWithdraw::recordBalance(Asset const& asset, BalanceChange change)
{
    XRPL_ASSERT(
        change.balanceChangeSign,
        "ripple::TransfersNotFrozen::recordBalance : valid trustline "
        "balance sign.");
    auto& changes = balanceChanges_[asset];
    if (change.balanceChangeSign < 0)
        changes.senders.emplace_back(std::move(change));
    else
        changes.receivers.emplace_back(std::move(change));
}

void
ValidWithdraw::recordBalanceChanges(
    std::shared_ptr<SLE const> const& after,
    STAmount const& balanceChange)
{
    if (after->getType() == ltACCOUNT_ROOT)
    {
        Asset const xrpAsset{xrpIssue()};
        recordBalance(
            xrpAsset,
            {after->at(sfAccount), balanceChange.signum()});
    }
    else if (after->getType() == ltRIPPLE_STATE)
    {
        auto const balanceChangeSign = balanceChange.signum();
        auto const currency = after->at(sfBalance).getCurrency();

        // Create Issues and convert to Assets
        Issue lowIssue{currency, after->at(sfLowLimit).getIssuer()};
        Issue highIssue{currency, after->at(sfHighLimit).getIssuer()};
        
        recordBalance(
            Asset{highIssue},
            {after->at(sfLowLimit).getIssuer(), balanceChangeSign});

        recordBalance(
            Asset{lowIssue},
            {after->at(sfHighLimit).getIssuer(), -balanceChangeSign});
    }
    else if (after->getType() == ltESCROW || after->getType() == ltPAYCHAN)
    {
        recordBalance(
            xrpIssue(),
            {after->at(sfDestination), balanceChange.signum()});
    }
    else if (after->getType() == ltMPTOKEN)
    {
        auto const mptID = after->at(sfMPTokenIssuanceID);
        MPTIssue mptIssue(mptID);
        
        recordBalance(
            Asset{mptIssue},
            {after->at(sfAccount), balanceChange.signum()});
    }
}

void
ValidWithdraw::visitEntry(
    bool isDelete,
    std::shared_ptr<SLE const> const& before,
    std::shared_ptr<SLE const> const& after)
{
    if (!isValidEntry(before, after))
        return;

    auto const balanceChange = calculateBalanceChange(before, after, isDelete);

    // Special handling for NFTokenPage changes (NFT transfers)
    if (after->getType() == ltNFTOKEN_PAGE)
    {
        // For NFT transfers, we need to track the token movement
        // Extract the account ID from the NFTokenPage ID
        AccountID const owner = nft::getAccountIDFromNFTPageID(after->key());

        // Use a special marker to track NFT transfers separately from monetary transfers
        static Currency const nftCurrency = Currency(1);
        
        // Create an Issue with the special NFT currency and use xrpAccount() as issuer
        Issue const nftIssue{nftCurrency, xrpAccount()};
        Asset const nftAsset{nftIssue};

        if (isDelete)
        {
            // NFTokenPage deleted: tokens leaving this account (sender)
            recordBalance(
                nftAsset,
                {owner, -1});  // Negative for sender
        }
        else if (!before)
        {
            // NFTokenPage created: tokens entering this account (receiver)  
            recordBalance(
                nftAsset,
                {owner, 1});  // Positive for receiver
        }
        return;
    }

    // Skip if no balance change
    if (balanceChange.signum() == 0)
        return;

    recordBalanceChanges(after, balanceChange);
}

bool
ValidWithdraw::finalize(
    STTx const& tx,
    TER const ter,
    XRPAmount const fee,
    ReadView const& view,
    beast::Journal const& j)
{
    for (auto const& [asset, changes] : balanceChanges_)
    {
        if (!validateWithdrawPreauth(view, changes, tx, j))
            return false;
    }

    return true;
}

bool
ValidWithdraw::validateWithdrawPreauth(
    ReadView const& view,
    BalanceChanges const& changes,
    STTx const& tx,
    beast::Journal const& j)
{
    // No Senders and No Receivers (e.g. EscrowCreate)
    if (changes.senders.empty() && changes.receivers.empty())
        return true;

    for (auto const& change : changes.senders)
    {
        // Get the sender's account
        AccountID const account = change.account;

        // Check if sender has a firewall
        if (!view.exists(keylet::firewall(account)))
            return true;

        // If sender has a firewall, check all receivers for valid withdraw
        // preauth
        for (auto const& receiver : changes.receivers)
        {
            // Skip if sending to self
            if (receiver.account == account)
                continue;

            if (!view.exists(
                    keylet::withdrawPreauth(
                        account,
                        receiver.account,
                        tx.isFieldPresent(sfDestinationTag)
                            ? tx.getFieldU32(sfDestinationTag)
                            : 0)))
                return false;
        }
    }
    return true;
}

}  // namespace ripple
