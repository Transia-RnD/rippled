#include <xrpld/app/main/Application.h>
#include <xrpld/app/ledger/LedgerMaster.h>
#include <xrpld/app/misc/DEXTimeSeriesReader.h>
#include <xrpld/app/misc/DEXTimeSeriesWriter.h>
#include <xrpld/app/misc/DEXTimeSeriesStore.h>
#include <xrpld/rpc/Context.h>

#include <xrpl/basics/Log.h>
#include <xrpl/json/json_value.h>
#include <xrpl/protocol/ErrorCodes.h>
#include <xrpl/protocol/jss.h>

namespace xrpl {

Json::Value
doDexStatus(RPC::JsonContext& context)
{
    JLOG(context.j.debug()) << "RPC dex_status called";

    Json::Value result(Json::objectValue);

    auto& reader = context.app.getDEXTimeSeriesReader();
    auto& writer = context.app.getDEXTimeSeriesWriter();

    auto* store = writer.getStore();
    if (!store || !store->isOpen())
    {
        JLOG(context.j.debug()) << "RPC dex_status: timeseries not enabled";
        result["enabled"] = false;
        return result;
    }

    result["enabled"] = true;

    auto const lastSeq = reader.getLastIndexedSeq();
    if (lastSeq)
        result["last_indexed_seq"] = *lastSeq;

    auto const validated =
        context.app.getLedgerMaster().getValidatedLedger();
    if (validated)
    {
        uint32_t const validatedSeq = validated->header().seq;
        result["validated_ledger_seq"] = validatedSeq;
        if (lastSeq)
        {
            int64_t backlog =
                static_cast<int64_t>(validatedSeq) - static_cast<int64_t>(*lastSeq);
            result["backlog"] = static_cast<uint32_t>(
                backlog > 0 ? backlog : 0);
        }
    }

    auto backfillStatus = store->getMeta("backfill_status");
    if (backfillStatus)
        result["backfill_status"] = *backfillStatus;

    auto backfillCursor = store->getMeta("backfill_cursor");
    if (backfillCursor)
    {
        try
        {
            result["backfill_cursor"] =
                static_cast<uint32_t>(std::stoul(*backfillCursor));
        }
        catch (...)
        {
            result["backfill_cursor"] = *backfillCursor;
        }
    }

    result["complete_ledgers"] =
        context.app.getLedgerMaster().getCompleteLedgers();

    return result;
}

}  // namespace xrpl
