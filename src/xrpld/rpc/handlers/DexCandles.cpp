#include <xrpld/app/main/Application.h>
#include <xrpld/app/misc/DEXTimeSeriesReader.h>
#include <xrpld/rpc/Context.h>

#include <xrpl/json/json_value.h>
#include <xrpl/protocol/ErrorCodes.h>
#include <xrpl/protocol/jss.h>

namespace xrpl {

Json::Value
doDexCandles(RPC::JsonContext& context)
{
    auto const& params = context.params;

    if (!params.isMember("book") || !params["book"].isString())
        return RPC::missing_field_error("book");

    std::string const bookKey = params["book"].asString();

    DEXInterval iv = DEXInterval::OneMinute;
    if (params.isMember("interval"))
    {
        if (!params["interval"].isString())
            return RPC::make_error(rpcINVALID_PARAMS);
        auto const ivStr = params["interval"].asString();
        if (ivStr == "5m")
            iv = DEXInterval::FiveMinute;
        else if (ivStr == "1h")
            iv = DEXInterval::OneHour;
        else if (ivStr == "1d")
            iv = DEXInterval::OneDay;
        else if (ivStr != "1m")
            return RPC::make_error(rpcINVALID_PARAMS);
    }

    uint32_t startTime = 0;
    uint32_t endTime = UINT32_MAX;
    uint32_t limit = 1000;

    if (params.isMember("start_time"))
    {
        if (!params["start_time"].isUInt() &&
            !(params["start_time"].isInt() &&
              params["start_time"].asInt() >= 0))
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

    auto& reader = context.app.getDEXTimeSeriesReader();
    auto candles = reader.getCandles(bookKey, iv, startTime, endTime, limit);

    Json::Value result(Json::objectValue);
    result["book"] = bookKey;
    result["interval"] = params.isMember("interval")
        ? params["interval"].asString()
        : "1m";

    Json::Value& arr = (result["candles"] = Json::arrayValue);
    for (auto const& c : candles)
    {
        Json::Value entry(Json::objectValue);
        entry["timestamp"] = c.timestamp;
        entry["open"] = std::to_string(c.open);
        entry["high"] = std::to_string(c.high);
        entry["low"] = std::to_string(c.low);
        entry["close"] = std::to_string(c.close);
        entry["volume_base"] = std::to_string(c.volumeBase);
        entry["volume_quote"] = std::to_string(c.volumeQuote);
        entry["tx_count"] = c.txCount;
        entry["buy_volume_base"] = std::to_string(c.buyVolumeBase);
        entry["sell_volume_base"] = std::to_string(c.sellVolumeBase);
        arr.append(entry);
    }

    auto const lastSeq = reader.getLastIndexedSeq();
    if (lastSeq)
        result["last_indexed_seq"] = *lastSeq;

    return result;
}

}  // namespace xrpl
