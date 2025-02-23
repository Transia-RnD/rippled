//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2024 Transia, LLC.

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

#include <xrpld/app/tx/detail/Firewall.h>
#include <xrpld/ledger/Sandbox.h>
#include <xrpld/ledger/View.h>
#include <xrpl/basics/Log.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/digest.h>
#include <xrpl/protocol/st.h>

namespace ripple {

XRPAmount
FirewallSet::calculateBaseFee(ReadView const& view, STTx const& tx)
{
    // Calculate the FirewallSigners Fees
    // std::int32_t signerCount = tx.isFieldPresent(sfFirewallSigners)
    //     ? tx.getFieldArray(sfFirewallSigners).size()
    //     : 0;

    // return ((signerCount + 2) * view.fees().base);
    return view.fees().base;
}

NotTEC
FirewallSet::preflight(PreflightContext const& ctx)
{
    if (!ctx.rules.enabled(featureFirewall))
        return temDISABLED;

    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    if (!ctx.tx.isFieldPresent(sfFirewallID))
    {
        // Create Firewall
    }
    else
    {
        // Update Firewall
    }

    return preflight2(ctx);
}

TER
FirewallSet::preclaim(PreclaimContext const& ctx)
{
    if (!ctx.tx.isFieldPresent(sfFirewallID))
    {
        // Create Firewall
    }
    else
    {
        // Update Firewall
    }

    return tesSUCCESS;
}

TER
FirewallSet::doApply()
{
    Sandbox sb(&ctx_.view());

    auto const sleOwner = sb.peek(keylet::account(account_));
    if (!sleOwner)
    {
        JLOG(j_.debug()) << "FirewallSet: Owner account not found";
        return tefINTERNAL;
    }

    if (!ctx_.tx.isFieldPresent(sfFirewallID))
    {
        // Create Firewall
        ripple::Keylet const firewallKeylet = keylet::firewall(account_);
        auto const sleFirewall = std::make_shared<SLE>(firewallKeylet);
        (*sleFirewall)[sfOwner] = account_;
        sleFirewall->setAccountID(sfIssuer, ctx_.tx.getAccountID(sfIssuer));
        if (ctx_.tx.isFieldPresent(sfFirewallRules))
            sleFirewall->setFieldArray(sfFirewallRules, ctx_.tx.getFieldArray(sfFirewallRules));

        // if (ctx_.tx.isFieldPresent(sfTimePeriod))
        // {
        //     sleFirewall->setFieldU32(sfTimePeriod, ctx_.tx.getFieldU32(sfTimePeriod));
        //     sleFirewall->setFieldU32(sfTimeStart, ctx_.view().parentCloseTime().time_since_epoch().count());
        //     sleFirewall->setFieldAmount(sfTimeAmount, STAmount{0});
        // }

        if (auto const page = sb.dirInsert(
                keylet::ownerDir(account_),
                sleFirewall->key(),
                describeOwnerDir(account_)))
        {
            sleFirewall->setFieldU64(sfOwnerNode, *page);
        }
        else
        {
            JLOG(j_.debug()) << "FirewallSet: failed to insert owner dir";
            return tecDIR_FULL;
        }
        sb.insert(sleFirewall);
        adjustOwnerCount(sb, sleOwner, 1, j_);

        {
            STAmount const reserve{view().fees().accountReserve(
                sleOwner->getFieldU32(sfOwnerCount) + 1)};

            if (mPriorBalance < reserve)
                return tecINSUFFICIENT_RESERVE;
        }

        AccountID const auth = ctx_.tx.getAccountID(sfAuthorize);
        Keylet const preauthKeylet = keylet::withdrawPreauth(account_, auth);
        auto slePreauth = std::make_shared<SLE>(preauthKeylet);

        slePreauth->setAccountID(sfAccount, account_);
        slePreauth->setAccountID(sfAuthorize, auth);

        if (auto const page = sb.dirInsert(
                keylet::ownerDir(account_),
                slePreauth->key(),
                describeOwnerDir(account_)))
        {
            sleFirewall->setFieldU64(sfOwnerNode, *page);
        }
        else
        {
            JLOG(j_.debug()) << "FirewallSet: failed to insert owner dir";
            return tecDIR_FULL;
        }
        sb.insert(slePreauth);
        adjustOwnerCount(sb, sleOwner, 1, j_);
    }
    else
    {
        // Update Firewall
        uint256 const firewallID = ctx_.tx.getFieldH256(sfFirewallID);
        ripple::Keylet const firewallKeylet = keylet::firewall(firewallID);
        auto sleFirewall = sb.peek(firewallKeylet);
        if (!sleFirewall)
        {
            JLOG(j_.debug()) << "FirewallSet: Firewall not found";
            return tefINTERNAL;
        }

        if (ctx_.tx.isFieldPresent(sfIssuer))
            sleFirewall->setAccountID(
                sfIssuer, ctx_.tx.getAccountID(sfIssuer));
        // if (ctx_.tx.isFieldPresent(sfAmount))
        //     sleFirewall->setFieldAmount(
        //         sfAmount, ctx_.tx.getFieldAmount(sfAmount));

        sb.update(sleFirewall);
    }

    sb.apply(ctx_.rawView());
    return tesSUCCESS;
}

}  // namespace ripple
