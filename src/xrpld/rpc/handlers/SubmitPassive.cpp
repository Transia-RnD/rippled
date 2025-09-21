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
#include <xrpld/app/misc/Transaction.h>
#include <xrpld/app/rdb/backend/StopLossDB.h>
#include <xrpld/rpc/Context.h>

#include <xrpl/protocol/jss.h>

namespace ripple {

Json::Value
doSubmitPassive(RPC::JsonContext& context)
{
    Json::Value result;

    if (!context.params.isMember(jss::tx_blob))
    {
        return RPC::make_error(rpcINVALID_PARAMS, "Missing tx_blob");
    }

    auto const txBlob = strUnHex(context.params[jss::tx_blob].asString());
    if (!txBlob || txBlob->empty())
    {
        return RPC::make_error(rpcINVALID_PARAMS, "Invalid tx_blob");
    }

    // Parse the signed transaction
    SerialIter sit(txBlob->data(), txBlob->size());
    std::shared_ptr<STTx const> stx;
    try
    {
        stx = std::make_shared<STTx const>(sit);
    }
    catch (...)
    {
        return RPC::make_error(rpcINVALID_PARAMS, "Invalid transaction");
    }

    // Verify it's an OfferCreate transaction
    if (stx->getTxnType() != ttOFFER_CREATE)
    {
        return RPC::make_error(
            rpcINVALID_PARAMS, "Must be an OfferCreate transaction");
    }

    // Verify it uses a ticket
    if (!stx->isFieldPresent(sfTicketSequence))
    {
        return RPC::make_error(
            rpcINVALID_PARAMS, "Transaction must use ticket");
    }

    // Get stop parameters
    if (!context.params.isMember("stop_price") ||
        !context.params.isMember("stop_type"))
    {
        return RPC::make_error(
            rpcINVALID_PARAMS, "Missing stop_price or stop_type");
    }

    // Parse stop price as a numeric value
    double stopPrice;
    if (context.params["stop_price"].isString())
    {
        try
        {
            stopPrice = std::stod(context.params["stop_price"].asString());
        }
        catch (...)
        {
            return RPC::make_error(rpcINVALID_PARAMS, "Invalid stop_price");
        }
    }
    else if (context.params["stop_price"].isNumeric())
    {
        stopPrice = context.params["stop_price"].asDouble();
    }
    else
    {
        return RPC::make_error(rpcINVALID_PARAMS, "stop_price must be numeric");
    }

    if (stopPrice <= 0)
    {
        return RPC::make_error(
            rpcINVALID_PARAMS, "stop_price must be positive");
    }

    StopType stopType;
    std::string typeStr = context.params["stop_type"].asString();
    if (typeStr == "stop_loss_sell")
        stopType = StopType::STOP_LOSS_SELL;
    else if (typeStr == "stop_loss_buy")
        stopType = StopType::STOP_LOSS_BUY;
    else if (typeStr == "take_profit_sell")
        stopType = StopType::TAKE_PROFIT_SELL;
    else if (typeStr == "take_profit_buy")
        stopType = StopType::TAKE_PROFIT_BUY;
    else
        return RPC::make_error(rpcINVALID_PARAMS, "Invalid stop_type");

    // Extract market from transaction
    STAmount takerGets = stx->getFieldAmount(sfTakerGets);
    STAmount takerPays = stx->getFieldAmount(sfTakerPays);

    // Store in database
    auto& stopLossDB = context.app.getStopLossDB();
    bool success = stopLossDB.addOrder(
        stx->getTransactionID(),
        *txBlob,
        stopPrice,
        stopType,
        takerGets,
        takerPays);

    if (success)
    {
        result[jss::engine_result] = "tesSUCCESS";
        result[jss::tx_hash] = to_string(stx->getTransactionID());
    }
    else
    {
        result[jss::engine_result] = "tecDUPLICATE";
    }

    return result;
}

}  // namespace ripple