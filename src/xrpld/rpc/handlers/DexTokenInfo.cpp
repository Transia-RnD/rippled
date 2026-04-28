#include <xrpld/app/main/Application.h>
#include <xrpld/app/ledger/LedgerMaster.h>
#include <xrpld/app/misc/DEXTimeSeriesReader.h>
#include <xrpld/app/misc/DEXTimeSeriesWriter.h>
#include <xrpld/rpc/Context.h>

#include <xrpl/basics/Log.h>
#include <xrpl/basics/StringUtilities.h>
#include <xrpl/json/json_value.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/ErrorCodes.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/jss.h>

#include <boost/algorithm/string.hpp>

namespace xrpl {

Json::Value
doDexTokenInfo(RPC::JsonContext& context)
{
    JLOG(context.j.debug()) << "RPC dex_token_info called";

    auto const& params = context.params;

    if (!params.isMember("issuer") || !params["issuer"].isString())
        return RPC::missing_field_error("issuer");
    if (!params.isMember("currency") || !params["currency"].isString())
        return RPC::missing_field_error("currency");

    std::string const issuerStr = params["issuer"].asString();
    std::string const currencyStr = params["currency"].asString();

    auto const issuerID = parseBase58<AccountID>(issuerStr);
    if (!issuerID)
        return RPC::make_error(rpcACT_MALFORMED);

    Currency currency;
    if (!to_currency(currency, currencyStr))
        return RPC::make_error(rpcINVALID_PARAMS);

    if (isXRP(currency))
        return RPC::make_error(rpcINVALID_PARAMS);

    Json::Value result(Json::objectValue);
    result["issuer"] = issuerStr;
    result["currency"] = currencyStr;

    auto& reader = context.app.getDEXTimeSeriesReader();

    std::string const tokenKey = issuerStr + "/" + currencyStr;
    std::string const bookKey = "XRP_drops|" + tokenKey;

    auto tokenInfo = reader.getTokenInfo(tokenKey);
    if (tokenInfo)
    {
        result["supply"] = tokenInfo->supply;
        result["frozen_supply"] = tokenInfo->frozenSupply;
        result["locked_supply"] = tokenInfo->lockedSupply;
        result["holders"] = tokenInfo->holders;
        result["trust_lines"] = tokenInfo->trustLines;
        result["token_info_seq"] = tokenInfo->ledgerSeq;
    }

    auto summary = reader.getTokenSummary(bookKey);
    if (summary)
    {
        result["last_price"] = summary->lastPrice;
        if (tokenInfo)
        {
            result["market_cap"] =
                (tokenInfo->supply + tokenInfo->frozenSupply) *
                summary->lastPrice;
        }
    }

    // email_hash and domain are cheap single-SLE reads
    auto const ledger =
        context.app.getLedgerMaster().getValidatedLedger();
    if (ledger)
    {
        result["ledger_seq"] = ledger->header().seq;

        auto const accountSLE =
            ledger->read(keylet::account(*issuerID));
        if (accountSLE)
        {
            if (accountSLE->isFieldPresent(sfEmailHash))
            {
                auto const& hash = accountSLE->getFieldH128(sfEmailHash);
                Blob const b(hash.begin(), hash.end());
                std::string md5 = strHex(makeSlice(b));
                boost::to_lower(md5);
                result["email_hash"] = md5;
                result["gravatar"] =
                    "https://www.gravatar.com/avatar/" + md5;
            }

            if (accountSLE->isFieldPresent(sfDomain))
            {
                auto const domain = accountSLE->getFieldVL(sfDomain);
                result["domain"] =
                    std::string(domain.begin(), domain.end());
            }
        }
    }

    auto const lastSeq = reader.getLastIndexedSeq();
    if (lastSeq)
        result["last_indexed_seq"] = *lastSeq;

    return result;
}

}  // namespace xrpl
