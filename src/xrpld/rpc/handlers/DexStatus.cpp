#include <xrpld/app/main/Application.h>
#include <xrpld/app/misc/DEXTimeSeriesReader.h>
#include <xrpld/app/misc/DEXTimeSeriesWriter.h>
#include <xrpld/app/misc/DEXTimeSeriesStore.h>
#include <xrpld/rpc/Context.h>

#include <xrpl/json/json_value.h>
#include <xrpl/protocol/ErrorCodes.h>
#include <xrpl/protocol/jss.h>

namespace xrpl {

Json::Value
doDexStatus(RPC::JsonContext& context)
{
    Json::Value result(Json::objectValue);

    auto& reader = context.app.getDEXTimeSeriesReader();
    auto& writer = context.app.getDEXTimeSeriesWriter();

    auto* store = writer.getStore();
    if (!store || !store->isOpen())
    {
        result["enabled"] = false;
        return result;
    }

    result["enabled"] = true;

    auto const lastSeq = reader.getLastIndexedSeq();
    if (lastSeq)
        result["last_indexed_seq"] = *lastSeq;

    auto backfillStatus = store->getMeta("backfill_status");
    if (backfillStatus)
        result["backfill_status"] = *backfillStatus;

    auto backfillStartSeq = store->getMeta("backfill_start_seq");
    if (backfillStartSeq)
        result["backfill_start_seq"] =
            static_cast<uint32_t>(std::stoul(*backfillStartSeq));

    auto backfillTargetSeq = store->getMeta("backfill_target_seq");
    if (backfillTargetSeq)
        result["backfill_target_seq"] =
            static_cast<uint32_t>(std::stoul(*backfillTargetSeq));

    return result;
}

}  // namespace xrpl
