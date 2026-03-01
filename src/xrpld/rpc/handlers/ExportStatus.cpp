#include <xrpld/app/main/Application.h>
#include <xrpld/app/misc/ExportSignatureCollector.h>
#include <xrpld/rpc/Context.h>

#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/ErrorCodes.h>
#include <xrpl/protocol/RPCErr.h>
#include <xrpl/protocol/jss.h>

namespace xrpl {

Json::Value
doExportStatus(RPC::JsonContext& context)
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

    return context.app.getExportSignatureCollector().getExportStatus(
        *account, exportSeq);
}

}  // namespace xrpl
