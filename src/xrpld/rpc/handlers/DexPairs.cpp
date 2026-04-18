#include <xrpld/app/main/Application.h>
#include <xrpld/app/misc/DEXTimeSeriesReader.h>
#include <xrpld/rpc/Context.h>

#include <xrpl/json/json_value.h>
#include <xrpl/protocol/ErrorCodes.h>
#include <xrpl/protocol/jss.h>

namespace xrpl {

Json::Value
doDexPairs(RPC::JsonContext& context)
{
    auto const& params = context.params;

    std::string sort = "volume";
    uint32_t limit = 100;

    if (params.isMember("sort"))
    {
        if (!params["sort"].isString())
            return RPC::make_error(rpcINVALID_PARAMS);
        sort = params["sort"].asString();
        if (sort != "volume" && sort != "trades" && sort != "change")
            return RPC::make_error(rpcINVALID_PARAMS);
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

    auto& reader = context.app.getDEXTimeSeriesReader();
    auto pairs = reader.getPairs(sort, limit);

    Json::Value result(Json::objectValue);

    Json::Value& arr = (result["pairs"] = Json::arrayValue);
    for (auto const& p : pairs)
    {
        Json::Value entry(Json::objectValue);
        entry["book"] = p.bookKey;
        entry["last_price"] = p.lastPrice;
        entry["price_change_24h"] = p.priceChange24h;
        entry["volume_24h_base"] = p.volume24hBase;
        entry["volume_24h_quote"] = p.volume24hQuote;
        entry["high_24h"] = p.high24h;
        entry["low_24h"] = p.low24h;
        entry["trade_count_24h"] = p.tradeCount24h;
        arr.append(entry);
    }

    auto const lastSeq = reader.getLastIndexedSeq();
    if (lastSeq)
        result["last_indexed_seq"] = *lastSeq;

    return result;
}

}  // namespace xrpl
