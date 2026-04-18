#include <xrpld/app/main/Application.h>
#include <xrpld/app/misc/DEXTimeSeriesReader.h>
#include <xrpld/rpc/Context.h>

#include <xrpl/json/json_value.h>
#include <xrpl/protocol/ErrorCodes.h>
#include <xrpl/protocol/jss.h>

namespace xrpl {

Json::Value
doDexAMMHistory(RPC::JsonContext& context)
{
    auto const& params = context.params;

    if (!params.isMember("account") || !params["account"].isString())
        return RPC::missing_field_error("account");

    std::string const account = params["account"].asString();

    uint32_t startVal = 0;
    uint32_t endVal = UINT32_MAX;
    uint32_t limit = 1000;

    // Prefer time-based params; fall back to seq-based for compat
    bool const useTime = params.isMember("start_time") ||
        params.isMember("end_time");

    auto const startField = useTime ? "start_time" : "start_seq";
    auto const endField = useTime ? "end_time" : "end_seq";

    if (params.isMember(startField))
    {
        if (!params[startField].isUInt() &&
            !(params[startField].isInt() && params[startField].asInt() >= 0))
            return RPC::make_error(rpcINVALID_PARAMS);
        startVal = params[startField].asUInt();
    }
    if (params.isMember(endField))
    {
        if (!params[endField].isUInt() &&
            !(params[endField].isInt() && params[endField].asInt() >= 0))
            return RPC::make_error(rpcINVALID_PARAMS);
        endVal = params[endField].asUInt();
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

    if (startVal > endVal)
        return RPC::make_error(rpcINVALID_PARAMS);

    auto& reader = context.app.getDEXTimeSeriesReader();
    auto snapshots = reader.getAMMHistory(account, startVal, endVal, limit);

    Json::Value result(Json::objectValue);
    result["account"] = account;

    Json::Value& arr = (result["history"] = Json::arrayValue);
    for (auto const& s : snapshots)
    {
        Json::Value entry(Json::objectValue);
        entry["ledger_seq"] = s.ledgerSeq;
        entry["timestamp"] = s.timestamp;
        entry["asset1_balance"] = s.asset1Balance;
        entry["asset2_balance"] = s.asset2Balance;
        entry["lpt_balance"] = s.lptBalance;
        entry["trading_fee"] = s.tradingFee;
        entry["curve_type"] = s.curveType;
        entry["tvl_xrp"] = s.tvlXrp;
        entry["volume_24h_xrp"] = s.volume24hXrp;
        entry["fees_24h_xrp"] = s.fees24hXrp;
        arr.append(entry);
    }

    auto const lastSeq = reader.getLastIndexedSeq();
    if (lastSeq)
        result["last_indexed_seq"] = *lastSeq;

    return result;
}

}  // namespace xrpl
