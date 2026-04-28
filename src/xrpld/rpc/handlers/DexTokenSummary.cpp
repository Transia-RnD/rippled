#include <xrpld/app/main/Application.h>
#include <xrpld/app/misc/DEXTimeSeriesReader.h>
#include <xrpld/rpc/Context.h>

#include <xrpl/basics/Log.h>
#include <xrpl/json/json_value.h>
#include <xrpl/protocol/ErrorCodes.h>
#include <xrpl/protocol/jss.h>

namespace xrpl {

Json::Value
doDexTokenSummary(RPC::JsonContext& context)
{
    JLOG(context.j.debug()) << "RPC dex_token_summary called";
    auto const& params = context.params;

    bool const hasBooksArray =
        params.isMember("books") && params["books"].isArray();
    bool const hasSingleBook =
        params.isMember("book") && params["book"].isString();

    if (!hasBooksArray && !hasSingleBook)
        return RPC::missing_field_error("books");

    std::vector<std::string> bookKeys;
    if (hasBooksArray)
    {
        auto const& books = params["books"];
        if (books.size() > 25)
            return RPC::make_error(rpcINVALID_PARAMS);
        for (auto const& b : books)
        {
            if (!b.isString())
                return RPC::make_error(rpcINVALID_PARAMS);
            bookKeys.push_back(b.asString());
        }
    }
    else
    {
        bookKeys.push_back(params["book"].asString());
    }

    JLOG(context.j.debug())
        << "RPC dex_token_summary: " << bookKeys.size() << " books requested";

    auto& reader = context.app.getDEXTimeSeriesReader();

    Json::Value result(Json::objectValue);
    Json::Value& arr = (result["summaries"] = Json::arrayValue);

    for (auto const& bookKey : bookKeys)
    {
        auto summary = reader.getTokenSummary(bookKey);
        Json::Value entry(Json::objectValue);
        entry["book"] = bookKey;
        if (summary)
        {
            entry["last_price"] = summary->lastPrice;
            entry["price_change_5m"] = summary->priceChange5m;
            entry["price_change_1h"] = summary->priceChange1h;
            entry["price_change_24h"] = summary->priceChange24h;
            entry["price_change_7d"] = summary->priceChange7d;
            entry["price_change_30d"] = summary->priceChange30d;
            entry["volume_24h_base"] = summary->volume24hBase;
            entry["volume_24h_quote"] = summary->volume24hQuote;
            entry["high_24h"] = summary->high24h;
            entry["low_24h"] = summary->low24h;
            entry["trade_count_24h"] = summary->tradeCount24h;
        }
        arr.append(entry);
    }

    auto const lastSeq = reader.getLastIndexedSeq();
    if (lastSeq)
        result["last_indexed_seq"] = *lastSeq;

    return result;
}

}  // namespace xrpl
