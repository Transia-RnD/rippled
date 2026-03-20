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

#include <xrpld/app/main/Application.h>
#include <xrpld/rpc/Context.h>
#include <xrpld/rpc/detail/RPCHelpers.h>
#include <xrpld/rpc/detail/RPCLedgerHelpers.h>

#include <xrpl/ledger/Dir.h>
#include <xrpl/ledger/ReadView.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/ErrorCodes.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/RPCErr.h>
#include <xrpl/protocol/STNumber.h>
#include <xrpl/protocol/jss.h>
#include <xrpl/tx/transactors/Option/MarginUtils.h>

namespace xrpl {

Json::Value
doGetMarginStatus(RPC::JsonContext& context)
{
    auto const& params = context.params;

    // Require margin_account field (256-bit hash)
    if (!params.isMember(jss::margin_account))
        return RPC::missing_field_error(jss::margin_account);

    uint256 marginAccountID;
    if (!marginAccountID.parseHex(
            params[jss::margin_account].asString()))
    {
        return RPC::make_param_error("Invalid margin_account hash.");
    }

    std::shared_ptr<ReadView const> ledger;
    auto jvResult = RPC::lookupLedger(ledger, context);
    if (!ledger)
        return jvResult;

    // Look up the margin account
    auto const sleMarginAcct =
        ledger->read(Keylet{ltMARGIN_ACCOUNT, marginAccountID});
    if (!sleMarginAcct)
    {
        RPC::inject_error(rpcENTRY_NOT_FOUND, jvResult);
        return jvResult;
    }

    AccountID const account = sleMarginAcct->getAccountID(sfAccount);

    Number const collateralBalance =
        sleMarginAcct->at(~sfCollateralBalance).value_or(Number(0));

    jvResult[jss::collateral_balance] = to_string(collateralBalance);

    // Iterate all margin positions for this account
    Json::Value positionsArray(Json::arrayValue);
    Number totalEquity = collateralBalance;
    Number totalMaintenance(0);

    Dir const ownerDir(*ledger, keylet::ownerDir(account));
    for (auto const& sle : ownerDir)
    {
        if (sle->getType() != ltMARGIN_POSITION)
            continue;
        if (sle->getFieldH256(sfMarginAccountID) != marginAccountID)
            continue;

        Number const entryPrice =
            sle->at(~sfEntryPrice).value_or(Number(0));
        Number const positionSize =
            sle->at(~sfPositionSize).value_or(Number(0));
        [[maybe_unused]] Number const allocatedMargin =
            sle->at(~sfAllocatedMargin).value_or(Number(0));
        Number const notional =
            sle->at(~sfNotionalValue).value_or(Number(0));
        std::uint32_t const positionSide =
            sle->getFieldU32(sfPositionSide);
        std::uint32_t const leverage =
            sle->getFieldU32(sfLeverage);

        // Get mark price from the position's asset pair
        uint256 const optionPairID =
            sle->getFieldH256(sfOptionPairID);
        auto const slePair =
            ledger->read(Keylet{ltOPTION_PAIR, optionPairID});

        Number markPrice(0);
        Number unrealizedPnl(0);
        if (slePair)
        {
            markPrice = margin::getMarkPrice(*ledger, slePair);
            unrealizedPnl =
                margin::calculateUnrealizedPnl(sle, markPrice);
        }

        Number const maintenance =
            margin::calculateMaintenanceMargin(notional, 5000);
        totalMaintenance = totalMaintenance + maintenance;
        totalEquity = totalEquity + unrealizedPnl;

        Json::Value pos(Json::objectValue);
        pos[jss::index] = to_string(sle->key());
        pos[jss::position_side] = positionSide;
        pos[jss::leverage] = leverage;
        pos[jss::entry_price] = to_string(entryPrice);
        pos[jss::position_size] = to_string(positionSize);
        pos[jss::notional] = to_string(notional);
        pos[jss::mark_price] = to_string(markPrice);
        pos[jss::unrealized_pnl] = to_string(unrealizedPnl);
        pos[jss::maintenance_margin] = to_string(maintenance);

        positionsArray.append(pos);
    }

    jvResult[jss::positions] = positionsArray;
    jvResult[jss::equity] = to_string(totalEquity);
    jvResult[jss::total_maintenance] = to_string(totalMaintenance);
    jvResult[jss::healthy] = (totalEquity >= totalMaintenance);

    return jvResult;
}

}  // namespace xrpl
