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

#include <xrpl/tx/transactors/Option/InsuranceDeposit.h>
#include <xrpl/ledger/Sandbox.h>
#include <xrpl/ledger/View.h>

#include <xrpl/basics/Log.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/STNumber.h>
#include <xrpl/protocol/TxFlags.h>

namespace xrpl {

NotTEC
InsuranceDeposit::preflight(PreflightContext const& ctx)
{
    if (ctx.tx.getFlags() & tfUniversalMask)
    {
        JLOG(ctx.j.debug()) << "InsuranceDeposit: invalid flags.";
        return temINVALID_FLAG;
    }

    STAmount const amount = ctx.tx[sfAmount];
    if (amount <= beast::zero)
    {
        JLOG(ctx.j.debug()) << "InsuranceDeposit: amount must be positive.";
        return temBAD_AMOUNT;
    }

    return tesSUCCESS;
}

TER
InsuranceDeposit::preclaim(PreclaimContext const& ctx)
{
    uint256 const insuranceVaultID = ctx.tx[sfInsuranceVaultID];

    // Verify insurance vault exists
    auto const sleVault =
        ctx.view.read(Keylet{ltINSURANCE_VAULT, insuranceVaultID});
    if (!sleVault)
    {
        JLOG(ctx.j.debug())
            << "InsuranceDeposit: insurance vault does not exist.";
        return tecNO_ENTRY;
    }

    // Verify deposit asset matches one of the vault's pair assets
    STAmount const amount = ctx.tx[sfAmount];
    auto const vaultAsset =
        sleVault->getFieldIssue(sfAsset).get<Issue>();
    auto const vaultAsset2 =
        sleVault->getFieldIssue(sfAsset2).get<Issue>();

    if (isXRP(amount))
    {
        if (!isXRP(vaultAsset) && !isXRP(vaultAsset2))
        {
            JLOG(ctx.j.debug())
                << "InsuranceDeposit: XRP deposit not valid for this vault.";
            return tecNO_PERMISSION;
        }
    }
    else
    {
        if (amount.issue() != vaultAsset && amount.issue() != vaultAsset2)
        {
            JLOG(ctx.j.debug())
                << "InsuranceDeposit: deposit asset does not match vault assets.";
            return tecNO_PERMISSION;
        }
    }

    return tesSUCCESS;
}

TER
InsuranceDeposit::doApply()
{
    Sandbox sb(&ctx_.view());

    uint256 const insuranceVaultID = ctx_.tx[sfInsuranceVaultID];
    STAmount const amount = ctx_.tx[sfAmount];

    auto sleVault =
        sb.peek(Keylet{ltINSURANCE_VAULT, insuranceVaultID});
    if (!sleVault)
        return tecNO_ENTRY;

    auto const vaultAccount = sleVault->getAccountID(sfAccount);
    auto sleSource = sb.peek(keylet::account(account_));
    if (!sleSource)
        return terNO_ACCOUNT;

    // Transfer tokens from depositor to vault pseudo-account
    if (isXRP(amount))
    {
        auto const sourceBalance = sleSource->getFieldAmount(sfBalance);
        auto const reserve = sb.fees().accountReserve(
            sleSource->getFieldU32(sfOwnerCount));
        if (sourceBalance < amount + reserve)
        {
            JLOG(j_.debug()) << "InsuranceDeposit: insufficient XRP (reserve).";
            return tecUNFUNDED_PAYMENT;
        }
        // Debit depositor, burn XRP (tracked via sfInsuranceBalance)
        sleSource->setFieldAmount(sfBalance, sourceBalance - amount);
        sb.rawDestroyXRP(amount.xrp());
    }
    else
    {
        auto const ter = accountSend(sb, account_, vaultAccount, amount, j_);
        if (ter != tesSUCCESS)
        {
            JLOG(j_.debug()) << "InsuranceDeposit: transfer failed.";
            return ter;
        }
    }

    // Update insurance balance
    Number currentBalance =
        sleVault->at(~sfInsuranceBalance).value_or(Number(0));
    currentBalance = currentBalance + Number(amount);
    sleVault->at(sfInsuranceBalance) =
        STNumber{sfInsuranceBalance, currentBalance};

    // TODO: Mint proportional share tokens (MPT) to depositor
    // For now, just track the balance. Full MPT minting will follow
    // the VaultDeposit pattern from src/libxrpl/tx/transactors/Vault/

    sb.update(sleVault);
    sb.update(sleSource);
    sb.apply(ctx_.rawView());
    return tesSUCCESS;
}

}  // namespace xrpl
