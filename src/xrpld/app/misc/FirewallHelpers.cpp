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

#include <xrpld/app/misc/FirewallHelpers.h>

#include <xrpl/protocol/TER.h>

namespace ripple {
namespace firewall {

NotTEC
validateFirewallRules(STArray const& rules, beast::Journal const& j)
{
    if (rules.empty())
    {
        JLOG(j.error())
            << "FirewallSet: sfFirewallRules must not be empty if present";
        return temMALFORMED;
    }

    if (rules.size() > 8)
    {
        JLOG(j.error())
            << "FirewallSet: sfFirewallRules must not be larger than 8";
        return temMALFORMED;
    }

    for (auto const& rule : rules)
    {
        LedgerEntryType ledgerEntryType =
            static_cast<LedgerEntryType>(rule.getFieldU16(sfLedgerEntryType));
        SField const& sField = SField::getField(rule.getFieldU32(sfFieldCode));
        auto const& operatorCode = rule.getFieldU16(sfComparisonOperator);

        if (!hasFirewallProtection(ledgerEntryType))
        {
            JLOG(j.error()) << "FirewallSet: sfLedgerEntryType "
                            << ledgerEntryType << " is not supported";
            return temMALFORMED;
        }

        if (!isFieldProtected(ledgerEntryType, sField))
        {
            JLOG(j.error()) << "FirewallSet: sfFieldCode " << sField.getName()
                            << " is not supported";
            return temMALFORMED;
        }

        if (operatorCode < 1 || operatorCode > 5)
        {
            JLOG(j.error()) << "FirewallSet: sfComparisonOperator "
                            << operatorCode << " is not supported";
            return temMALFORMED;
        }
    }

    return tesSUCCESS;
}

NotTEC
checkFirewallSigners(PreflightContext const& ctx)
{
    if (!ctx.tx.isFieldPresent(sfFirewallSigners))
    {
        JLOG(ctx.j.trace())
            << "checkFirewallSigners: sfFirewallSigners required";
        return temMALFORMED;
    }
    // Validate signers structure - similar to Batch validation
    auto const& signers = ctx.tx.getFieldArray(sfFirewallSigners);
    if (signers.empty())
    {
        JLOG(ctx.j.trace())
            << "checkFirewallSigners: sfFirewallSigners cannot be empty";
        return temMALFORMED;
    }

    // None of the signers can be the outer account
    for (auto const& signer : signers)
    {
        if (signer.getAccountID(sfAccount) == ctx.tx.getAccountID(sfAccount))
        {
            JLOG(ctx.j.trace())
                << "checkFirewallSigners: sfFirewallSigners cannot include the "
                   "outer account";
            return temMALFORMED;
        }
    }

    auto const sigResult = ctx.tx.checkFirewallSign(
        STTx::RequireFullyCanonicalSig::yes, ctx.rules);
    if (!sigResult)
    {
        JLOG(ctx.j.trace())
            << "checkFirewallSigners: invalid firewall signature: "
            << sigResult.error();
        return temBAD_SIGNATURE;
    }

    return tesSUCCESS;
}

bool
hasFirewallProtection(LedgerEntryType const& type)
{
    return Firewall::getInstance().hasFirewall(type);
}

std::vector<SField const*>
getProtectedFields(LedgerEntryType const& type)
{
    return Firewall::getInstance().getFieldsForLedgerType(type);
}

bool
isFieldProtected(LedgerEntryType const& type, SField const& field)
{
    return Firewall::getInstance().handlesField(type, field);
}

std::vector<STObject>
getFirewallRules(STArray const& rules, LedgerEntryType const& type)
{
    std::vector<STObject> matchingRules;
    for (auto const& rule : rules)
    {
        if (rule.getFieldU16(sfLedgerEntryType) == type)
            matchingRules.push_back(rule);
    }
    return matchingRules;
}

}  // namespace firewall
}  // namespace ripple
