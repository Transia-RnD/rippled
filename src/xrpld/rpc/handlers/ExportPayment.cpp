#include <xrpld/app/main/Application.h>
#include <xrpld/app/misc/ExportSignatureCollector.h>
#include <xrpld/rpc/Context.h>

#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/ErrorCodes.h>
#include <xrpl/protocol/RPCErr.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/jss.h>

#include <xrpl/basics/StringUtilities.h>

namespace xrpl {

Json::Value
doExportPayment(RPC::JsonContext& context)
{
    auto const& params = context.params;

    if (!params.isMember(jss::account))
        return RPC::missing_field_error(jss::account);

    if (!params.isMember(jss::export_sequence))
        return RPC::missing_field_error(jss::export_sequence);

    auto const account =
        parseBase58<AccountID>(params[jss::account].asString());
    if (!account)
        return RPC::make_param_error("Invalid account.");

    auto const exportSeq = params[jss::export_sequence].asUInt();

    auto const payment =
        context.app.getExportSignatureCollector().getExportPayment(
            *account, exportSeq);

    Json::Value result(Json::objectValue);
    result[jss::account] = toBase58(*account);
    result[jss::export_sequence] = exportSeq;

    if (payment)
    {
        Serializer s;
        payment->add(s);
        result[jss::mainnet_payment_blob] = strHex(s.peekData());
        result[jss::submit_ready] = true;
    }
    else
    {
        result[jss::mainnet_payment_blob] = Json::nullValue;
        result[jss::submit_ready] = false;
    }

    return result;
}

}  // namespace xrpl
