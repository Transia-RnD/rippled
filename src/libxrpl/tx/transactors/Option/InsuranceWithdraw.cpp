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

#include <xrpl/tx/transactors/Option/InsuranceWithdraw.h>
#include <xrpl/ledger/Sandbox.h>
#include <xrpl/ledger/View.h>

#include <xrpl/basics/Log.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/STNumber.h>
#include <xrpl/protocol/TxFlags.h>

namespace xrpl {

NotTEC
InsuranceWithdraw::preflight(PreflightContext const& ctx)
{
    if (ctx.tx.getFlags() & tfUniversalMask)
    {
        JLOG(ctx.j.debug()) << "InsuranceWithdraw: invalid flags.";
        return temINVALID_FLAG;
    }

    STAmount const amount = ctx.tx[sfAmount];
    if (amount <= beast::zero)
    {
        JLOG(ctx.j.debug()) << "InsuranceWithdraw: amount must be positive.";
        return temBAD_AMOUNT;
    }

    return tesSUCCESS;
}

TER
InsuranceWithdraw::preclaim(PreclaimContext const& ctx)
{
    uint256 const insuranceVaultID = ctx.tx[sfInsuranceVaultID];

    // Verify insurance vault exists
    auto const sleVault =
        ctx.view.read(Keylet{ltINSURANCE_VAULT, insuranceVaultID});
    if (!sleVault)
    {
        JLOG(ctx.j.debug())
            << "InsuranceWithdraw: insurance vault does not exist.";
        return tecNO_ENTRY;
    }

    // Verify sufficient balance
    Number const balance =
        sleVault->at(~sfInsuranceBalance).value_or(Number(0));
    STAmount const amount = ctx.tx[sfAmount];
    if (balance < Number(amount))
    {
        JLOG(ctx.j.debug())
            << "InsuranceWithdraw: insufficient insurance balance.";
        return tecUNFUNDED_PAYMENT;
    }

    return tesSUCCESS;
}

TER
InsuranceWithdraw::doApply()
{
    Sandbox sb(&ctx_.view());

    uint256 const insuranceVaultID = ctx_.tx[sfInsuranceVaultID];
    STAmount const amount = ctx_.tx[sfAmount];

    auto sleVault =
        sb.peek(Keylet{ltINSURANCE_VAULT, insuranceVaultID});
    if (!sleVault)
        return tecNO_ENTRY;

    auto const vaultAccount = sleVault->getAccountID(sfAccount);
    auto sleReceiver = sb.peek(keylet::account(account_));
    if (!sleReceiver)
        return terNO_ACCOUNT;

    // Verify caller is authorized to withdraw.
    // Restrict withdrawal to the vault's asset issuers until MPT shares
    // are fully implemented.
    {
        auto const sleOptionPair = [&]() -> std::shared_ptr<SLE const> {
            auto const asset = sleVault->getFieldIssue(sfAsset).get<Issue>();
            auto const asset2 = sleVault->getFieldIssue(sfAsset2).get<Issue>();
            return sb.read(keylet::optionPair(asset, asset2));
        }();

        // Only the OptionPair asset issuers may withdraw
        bool authorized = false;
        if (sleOptionPair)
        {
            auto const asset = sleOptionPair->getFieldIssue(sfAsset).get<Issue>();
            auto const asset2 = sleOptionPair->getFieldIssue(sfAsset2).get<Issue>();
            authorized = (account_ == asset.account ||
                          account_ == asset2.account);
        }
        if (!authorized)
        {
            JLOG(j_.debug())
                << "InsuranceWithdraw: caller not authorized to withdraw.";
            return tecNO_PERMISSION;
        }
    }

    // Transfer tokens from vault to withdrawer
    if (isXRP(amount))
    {
        // Mint XRP back (counterpart of burn on deposit)
        auto const receiverBalance = sleReceiver->getFieldAmount(sfBalance);
        sleReceiver->setFieldAmount(sfBalance, receiverBalance + amount);
        sb.rawDestroyXRP(-amount.xrp());
    }
    else
    {
        auto const ter = accountSend(sb, vaultAccount, account_, amount, j_);
        if (ter != tesSUCCESS)
        {
            JLOG(j_.debug()) << "InsuranceWithdraw: transfer failed.";
            return ter;
        }
    }

    // Update insurance balance
    Number currentBalance =
        sleVault->at(~sfInsuranceBalance).value_or(Number(0));
    currentBalance = currentBalance - Number(amount);
    sleVault->at(sfInsuranceBalance) =
        STNumber{sfInsuranceBalance, currentBalance};

    sb.update(sleVault);
    sb.update(sleReceiver);
    sb.apply(ctx_.rawView());
    return tesSUCCESS;
}

}  // namespace xrpl
