#include <xrpld/app/main/Application.h>
#include <xrpld/app/misc/DEXTimeSeriesReader.h>
#include <xrpld/rpc/Context.h>

#include <xrpl/basics/Log.h>
#include <xrpl/json/json_value.h>
#include <xrpl/protocol/ErrorCodes.h>
#include <xrpl/protocol/jss.h>

#include <algorithm>

namespace xrpl {

namespace {

struct IntervalInfo
{
    DEXInterval baseInterval;
    uint32_t derivedSeconds;
    uint32_t baseSeconds;
    bool isNative;
};

std::optional<IntervalInfo>
parseInterval(std::string const& s)
{
    if (s == "1m")  return IntervalInfo{DEXInterval::OneMinute,     60,    60, true};
    if (s == "5m")  return IntervalInfo{DEXInterval::FiveMinute,   300,   300, true};
    if (s == "15m") return IntervalInfo{DEXInterval::FiveMinute,   900,   300, false};
    if (s == "30m") return IntervalInfo{DEXInterval::FiveMinute,  1800,   300, false};
    if (s == "1h")  return IntervalInfo{DEXInterval::OneHour,     3600,  3600, true};
    if (s == "4h")  return IntervalInfo{DEXInterval::OneHour,    14400,  3600, false};
    if (s == "1d")  return IntervalInfo{DEXInterval::OneDay,     86400, 86400, true};
    if (s == "1w")  return IntervalInfo{DEXInterval::OneDay,    604800, 86400, false};
    return std::nullopt;
}

std::vector<DEXCandle>
aggregateCandles(
    std::vector<DEXCandle> const& base,
    uint32_t derivedSeconds)
{
    std::vector<DEXCandle> result;
    if (base.empty())
        return result;

    DEXCandle current{};
    uint32_t currentBucket = 0;

    for (auto const& c : base)
    {
        uint32_t bucket = (c.timestamp / derivedSeconds) * derivedSeconds;
        if (bucket != currentBucket)
        {
            if (currentBucket != 0)
                result.push_back(current);

            current = c;
            current.timestamp = bucket;
            currentBucket = bucket;
        }
        else
        {
            current.high = std::max(current.high, c.high);
            current.low = std::min(current.low, c.low);
            current.close = c.close;
            current.volumeBase += c.volumeBase;
            current.volumeQuote += c.volumeQuote;
            current.txCount += c.txCount;
            current.buyVolumeBase += c.buyVolumeBase;
            current.sellVolumeBase += c.sellVolumeBase;
        }
    }
    if (currentBucket != 0)
        result.push_back(current);

    return result;
}

}  // namespace

Json::Value
doDexCandles(RPC::JsonContext& context)
{
    JLOG(context.j.debug()) << "RPC dex_candles called";

    auto const& params = context.params;

    if (!params.isMember("book") || !params["book"].isString())
        return RPC::missing_field_error("book");

    std::string const bookKey = params["book"].asString();

    std::string ivStr = "1m";
    if (params.isMember("interval"))
    {
        if (!params["interval"].isString())
            return RPC::make_error(rpcINVALID_PARAMS);
        ivStr = params["interval"].asString();
    }

    auto const ivInfo = parseInterval(ivStr);
    if (!ivInfo)
        return RPC::make_error(rpcINVALID_PARAMS);

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

    JLOG(context.j.debug())
        << "RPC dex_candles: book=" << bookKey
        << " interval=" << ivStr
        << " start=" << startTime << " end=" << endTime
        << " limit=" << limit;

    auto& reader = context.app.getDEXTimeSeriesReader();

    std::vector<DEXCandle> candles;
    if (ivInfo->isNative)
    {
        candles = reader.getCandles(
            bookKey, ivInfo->baseInterval, startTime, endTime, limit);
    }
    else
    {
        uint32_t const mult = ivInfo->derivedSeconds / ivInfo->baseSeconds;
        auto base = reader.getCandles(
            bookKey, ivInfo->baseInterval, startTime, endTime, limit * mult);
        candles = aggregateCandles(base, ivInfo->derivedSeconds);
        if (candles.size() > limit)
            candles.resize(limit);
    }

    JLOG(context.j.debug())
        << "RPC dex_candles: returning " << candles.size() << " candles";

    Json::Value result(Json::objectValue);
    result["book"] = bookKey;
    result["interval"] = ivStr;

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
