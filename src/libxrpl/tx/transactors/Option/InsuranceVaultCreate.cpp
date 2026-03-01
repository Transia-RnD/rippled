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

#include <xrpl/tx/transactors/Option/InsuranceVaultCreate.h>
#include <xrpl/ledger/Sandbox.h>
#include <xrpl/ledger/View.h>

#include <xrpl/basics/Log.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/STNumber.h>
#include <xrpl/protocol/TxFlags.h>

namespace xrpl {

NotTEC
InsuranceVaultCreate::preflight(PreflightContext const& ctx)
{
    if (ctx.tx.getFlags() & tfUniversalMask)
    {
        JLOG(ctx.j.debug()) << "InsuranceVaultCreate: invalid flags.";
        return temINVALID_FLAG;
    }

    Issue const issue = ctx.tx[sfAsset].get<Issue>();
    Issue const issue2 = ctx.tx[sfAsset2].get<Issue>();

    if (issue == issue2)
    {
        JLOG(ctx.j.debug())
            << "InsuranceVaultCreate: assets must be different.";
        return temMALFORMED;
    }

    return tesSUCCESS;
}

XRPAmount
InsuranceVaultCreate::calculateBaseFee(ReadView const& view, STTx const& tx)
{
    return view.fees().increment;
}

TER
InsuranceVaultCreate::preclaim(PreclaimContext const& ctx)
{
    Issue const issue = ctx.tx[sfAsset].get<Issue>();
    Issue const issue2 = ctx.tx[sfAsset2].get<Issue>();

    // Verify OptionPair exists for this asset pair
    auto const slePair = ctx.view.read(keylet::optionPair(issue, issue2));
    if (!slePair)
    {
        JLOG(ctx.j.debug())
            << "InsuranceVaultCreate: OptionPair does not exist.";
        return tecNO_ENTRY;
    }

    // Verify insurance vault doesn't already exist
    auto const sleVault = ctx.view.read(keylet::insuranceVault(issue, issue2));
    if (sleVault)
    {
        JLOG(ctx.j.debug())
            << "InsuranceVaultCreate: insurance vault already exists.";
        return tecDUPLICATE;
    }

    return tesSUCCESS;
}

TER
InsuranceVaultCreate::doApply()
{
    Sandbox sb(&ctx_.view());

    Issue const issue = ctx_.tx[sfAsset].get<Issue>();
    Issue const issue2 = ctx_.tx[sfAsset2].get<Issue>();

    auto const vaultKeylet = keylet::insuranceVault(issue, issue2);

    // Create pseudo-account for the insurance vault
    auto const maybeAccount =
        createPseudoAccount(sb, vaultKeylet.key, sfInsuranceVaultID);
    if (!maybeAccount)
    {
        JLOG(j_.error())
            << "InsuranceVaultCreate: failed to create pseudo account.";
        return maybeAccount.error();
    }
    auto const vaultAccount = (*maybeAccount)->getAccountID(sfAccount);

    // Create MPTokenIssuance for share tokens
    auto const mptKeylet =
        keylet::mptIssuance(ctx_.tx.getSeqProxy().value(), account_);
    auto const sleMPT = std::make_shared<SLE>(mptKeylet);
    sleMPT->setAccountID(sfIssuer, vaultAccount);
    sleMPT->setFieldU64(sfMaximumAmount, 0xFFFFFFFFFFFFFFFF);
    sb.insert(sleMPT);

    // Create the insurance vault ledger entry
    auto sleVault = std::make_shared<SLE>(vaultKeylet);
    sleVault->setAccountID(sfAccount, vaultAccount);
    auto const& [_issue1, _issue2] = std::minmax(issue, issue2);
    sleVault->setFieldIssue(sfAsset, STIssue{sfAsset, _issue1});
    sleVault->setFieldIssue(sfAsset2, STIssue{sfAsset2, _issue2});
    sleVault->at(sfInsuranceBalance) =
        STNumber{sfInsuranceBalance, Number(0)};
    sleVault->setFieldH256(sfShareMPTID, mptKeylet.key);

    // Add to owner directory
    auto const page = sb.dirInsert(
        keylet::ownerDir(vaultAccount),
        vaultKeylet,
        describeOwnerDir(vaultAccount));
    if (!page)
    {
        JLOG(j_.debug())
            << "InsuranceVaultCreate: failed to insert owner dir.";
        return tecDIR_FULL;
    }
    sleVault->setFieldU64(sfOwnerNode, *page);

    sb.insert(sleVault);

    sb.apply(ctx_.rawView());
    return tesSUCCESS;
}

}  // namespace xrpl
