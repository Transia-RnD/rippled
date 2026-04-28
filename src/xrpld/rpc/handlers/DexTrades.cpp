#include <xrpld/app/main/Application.h>
#include <xrpld/app/misc/DEXTimeSeriesReader.h>
#include <xrpld/rpc/Context.h>

#include <xrpl/basics/Log.h>
#include <xrpl/json/json_value.h>
#include <xrpl/protocol/ErrorCodes.h>
#include <xrpl/protocol/jss.h>

namespace xrpl {

Json::Value
doDexTrades(RPC::JsonContext& context)
{
    JLOG(context.j.debug()) << "RPC dex_trades called";

    auto const& params = context.params;

    if (!params.isMember("book") || !params["book"].isString())
        return RPC::missing_field_error("book");

    std::string const bookKey = params["book"].asString();

    uint32_t startTime = 0;
    uint32_t endTime = UINT32_MAX;
    uint32_t limit = 1000;

    if (params.isMember("start_time"))
    {
        if (!params["start_time"].isUInt() &&
            !(params["start_time"].isInt() && params["start_time"].asInt() >= 0))
            return RPC::make_error(rpcINVALID_PARAMS);
        startTime = params["start_time"].asUInt();
    }
    if (params.isMember("end_time"))
    {
        if (!params["end_time"].isUInt() &&
            !(params["end_time"].isInt() && params["end_time"].asInt() >= 0))
            return RPC::make_error(rpcINVALID_PARAMS);
        endTime = params["end_time"].asUInt();
    }
    if (params.isMember("limit"))
    {
        if (!params["limit"].isUInt() &&
            !(params["limit"].isInt() && params["limit"].asInt() >= 0))
            return RPC::make_error(rpcINVALID_PARAMS);
        limit = params["limit"].asUInt();
        if (limit == 0 || limit > 10000)
            limit = 10000;
    }

    if (startTime > endTime)
        return RPC::make_error(rpcINVALID_PARAMS);

    JLOG(context.j.debug())
        << "RPC dex_trades: book=" << bookKey
        << " start=" << startTime << " end=" << endTime
        << " limit=" << limit;

    auto& reader = context.app.getDEXTimeSeriesReader();
    auto trades = reader.getTrades(bookKey, startTime, endTime, limit);

    JLOG(context.j.debug())
        << "RPC dex_trades: returning " << trades.size() << " trades";

    Json::Value result(Json::objectValue);
    result["book"] = bookKey;

    Json::Value& arr = (result["trades"] = Json::arrayValue);
    for (auto const& t : trades)
    {
        Json::Value entry(Json::objectValue);
        entry["ledger_seq"] = t.ledgerSeq;
        entry["tx_index"] = t.txIndex;
        entry["timestamp"] = t.timestamp;
        entry["rate"] = std::to_string(t.rate);
        entry["volume_base"] = std::to_string(t.volumeBase);
        entry["volume_quote"] = std::to_string(t.volumeQuote);
        entry["side"] = t.side;
        entry["source"] = t.source;
        if (!t.taker.empty())
            entry["taker"] = t.taker;
        if (!t.txHash.empty())
            entry["tx_hash"] = t.txHash;
        arr.append(entry);
    }

    auto const lastSeq = reader.getLastIndexedSeq();
    if (lastSeq)
        result["last_indexed_seq"] = *lastSeq;

    return result;
}

}  // namespace xrpl
