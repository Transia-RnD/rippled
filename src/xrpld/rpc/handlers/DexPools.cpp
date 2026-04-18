#include <xrpld/app/main/Application.h>
#include <xrpld/app/misc/DEXTimeSeriesReader.h>
#include <xrpld/rpc/Context.h>

#include <xrpl/json/json_value.h>
#include <xrpl/protocol/ErrorCodes.h>
#include <xrpl/protocol/jss.h>

namespace xrpl {

Json::Value
doDexPools(RPC::JsonContext& context)
{
    auto const& params = context.params;

    std::string sort = "tvl";
    uint32_t limit = 100;

    if (params.isMember("sort"))
    {
        if (!params["sort"].isString())
            return RPC::make_error(rpcINVALID_PARAMS);
        sort = params["sort"].asString();
        if (sort != "tvl" && sort != "volume" && sort != "apr")
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
    auto pools = reader.getPools(sort, limit);

    Json::Value result(Json::objectValue);

    Json::Value& arr = (result["pools"] = Json::arrayValue);
    for (auto const& p : pools)
    {
        Json::Value entry(Json::objectValue);
        entry["account"] = p.account;
        if (!p.asset1.empty())
            entry["asset1"] = p.asset1;
        if (!p.asset2.empty())
            entry["asset2"] = p.asset2;
        entry["asset1_balance"] = p.asset1Balance;
        entry["asset2_balance"] = p.asset2Balance;
        entry["lpt_balance"] = p.lptBalance;
        entry["trading_fee"] = p.tradingFee;
        entry["curve_type"] = p.curveType;
        entry["tvl_xrp"] = p.tvlXrp;
        entry["volume_24h_xrp"] = p.volume24hXrp;
        entry["fees_24h_xrp"] = p.fees24hXrp;
        entry["apr"] = p.apr;
        entry["ledger_seq"] = p.ledgerSeq;
        entry["timestamp"] = p.timestamp;
        arr.append(entry);
    }

    auto const lastSeq = reader.getLastIndexedSeq();
    if (lastSeq)
        result["last_indexed_seq"] = *lastSeq;

    return result;
}

}  // namespace xrpl
