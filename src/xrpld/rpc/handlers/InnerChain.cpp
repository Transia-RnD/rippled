#include <xrpld/app/inner/InnerChainEngine.h>
#include <xrpld/app/inner/InnerCxLedger.h>
#include <xrpld/app/inner/detail/InnerTransactor.h>
#include <xrpld/app/main/Application.h>
#include <xrpld/rpc/Context.h>

#include <xrpl/json/json_value.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/ErrorCodes.h>
#include <xrpl/protocol/jss.h>

namespace xrpl {

namespace {

inner::InnerChainEngine*
resolveEngine(RPC::JsonContext& context)
{
    auto* registry = context.app.getInnerChainRegistry();
    if (!registry || registry->empty())
        return nullptr;

    if (context.params.isMember("chain_id"))
    {
        auto const chainId =
            context.params["chain_id"].asUInt();
        return registry->get(chainId);
    }

    return registry->first();
}

json::Value
noEngineError(RPC::JsonContext& context)
{
    json::Value jvResult;
    if (context.params.isMember("chain_id"))
    {
        jvResult["error"] = "innerChainNotFound";
        jvResult["error_message"] =
            "No inner chain with chain_id=" +
            std::to_string(context.params["chain_id"].asUInt());
    }
    else
    {
        jvResult["error"] = "innerChainNotEnabled";
        jvResult["error_message"] = "No inner chains configured";
    }
    return jvResult;
}

}  // namespace

json::Value
doInnerChainInfo(RPC::JsonContext& context)
{
    // If no chain_id specified and registry exists, return all chains
    auto* registry = context.app.getInnerChainRegistry();
    if (!registry || registry->empty())
    {
        json::Value jvResult;
        jvResult["error"] = "innerChainNotEnabled";
        jvResult["error_message"] = "No inner chains configured";
        return jvResult;
    }

    if (context.params.isMember("chain_id"))
    {
        auto* engine = resolveEngine(context);
        if (!engine)
            return noEngineError(context);

        auto result = engine->getJson(true);
        result["status"] = "success";
        return result;
    }

    auto result = registry->getJson(true);
    result["status"] = "success";
    return result;
}

json::Value
doInnerAccountInfo(RPC::JsonContext& context)
{
    auto* engine = resolveEngine(context);
    if (!engine)
        return noEngineError(context);

    if (!context.params.isMember("account"))
    {
        json::Value jvResult;
        jvResult["error"] = "invalidParams";
        jvResult["error_message"] = "Missing 'account' parameter";
        return jvResult;
    }

    auto const accountStr = context.params["account"].asString();
    auto const id = parseBase58<AccountID>(accountStr);
    if (!id)
    {
        json::Value jvResult;
        jvResult["error"] = "invalidParams";
        jvResult["error_message"] = "Invalid account address";
        return jvResult;
    }

    auto const acct = engine->getAccount(*id);
    json::Value jvResult;
    if (!acct)
    {
        jvResult["error"] = "actNotFound";
        jvResult["error_message"] = "Account not found on inner chain";
        jvResult["chain_id"] = engine->networkId();
        return jvResult;
    }

    jvResult["status"] = "success";
    jvResult["account"] = toBase58(*id);
    jvResult["balance"] = std::to_string(acct->balance);
    jvResult["sequence"] = acct->sequence;
    jvResult["chain_id"] = engine->networkId();
    jvResult["chain_name"] = engine->config().name;
    return jvResult;
}

json::Value
doInnerDeposit(RPC::JsonContext& context)
{
    auto* engine = resolveEngine(context);
    if (!engine)
        return noEngineError(context);

    if (!context.params.isMember("destination") ||
        !context.params.isMember("amount"))
    {
        json::Value jvResult;
        jvResult["error"] = "invalidParams";
        jvResult["error_message"] =
            "Missing 'destination' and/or 'amount' parameters";
        return jvResult;
    }

    auto const destStr = context.params["destination"].asString();
    auto const dest = parseBase58<AccountID>(destStr);
    if (!dest)
    {
        json::Value jvResult;
        jvResult["error"] = "invalidParams";
        jvResult["error_message"] = "Invalid destination address";
        return jvResult;
    }

    std::uint64_t amount = 0;
    if (context.params["amount"].isString())
        amount = std::stoull(context.params["amount"].asString());
    else if (context.params["amount"].isIntegral())
        amount = context.params["amount"].asUInt();

    if (amount == 0)
    {
        json::Value jvResult;
        jvResult["error"] = "invalidParams";
        jvResult["error_message"] = "Amount must be positive";
        return jvResult;
    }

    if (!engine->creditAccount(*dest, amount))
    {
        json::Value jvResult;
        jvResult["error"] = "innerChainBusy";
        jvResult["error_message"] = "Inner chain transaction queue is full";
        jvResult["chain_id"] = engine->networkId();
        return jvResult;
    }

    json::Value jvResult;
    jvResult["status"] = "success";
    jvResult["destination"] = toBase58(*dest);
    jvResult["amount"] = std::to_string(amount);
    jvResult["chain_id"] = engine->networkId();
    jvResult["chain_name"] = engine->config().name;
    jvResult["message"] = "Deposit queued for next inner chain ledger";
    return jvResult;
}

json::Value
doInnerSubmit(RPC::JsonContext& context)
{
    auto* engine = resolveEngine(context);
    if (!engine)
        return noEngineError(context);

    if (!context.params.isMember("tx_type"))
    {
        json::Value jvResult;
        jvResult["error"] = "invalidParams";
        jvResult["error_message"] = "Missing 'tx_type' parameter";
        return jvResult;
    }

    auto const txTypeStr = context.params["tx_type"].asString();

    if (txTypeStr == "Credit")
    {
        json::Value jvResult;
        jvResult["error"] = "invalidParams";
        jvResult["error_message"] =
            "Credit is a system-only transaction type; use inner_deposit";
        return jvResult;
    }

    inner::InnerTxType txType;
    if (txTypeStr == "Transfer")
        txType = inner::InnerTxType::Transfer;
    else if (txTypeStr == "Withdraw")
        txType = inner::InnerTxType::Withdraw;
    else
    {
        json::Value jvResult;
        jvResult["error"] = "invalidParams";
        jvResult["error_message"] =
            "Unknown tx_type '" + txTypeStr +
            "'; supported: Transfer, Withdraw";
        return jvResult;
    }

    if (!context.params.isMember("source") ||
        !context.params.isMember("destination") ||
        !context.params.isMember("amount"))
    {
        json::Value jvResult;
        jvResult["error"] = "invalidParams";
        jvResult["error_message"] =
            "Missing required field(s): source, destination, amount";
        return jvResult;
    }

    auto const src =
        parseBase58<AccountID>(context.params["source"].asString());
    if (!src)
    {
        json::Value jvResult;
        jvResult["error"] = "invalidParams";
        jvResult["error_message"] = "Invalid source address";
        return jvResult;
    }

    auto const dst =
        parseBase58<AccountID>(context.params["destination"].asString());
    if (!dst)
    {
        json::Value jvResult;
        jvResult["error"] = "invalidParams";
        jvResult["error_message"] = "Invalid destination address";
        return jvResult;
    }

    std::uint64_t amount = 0;
    if (context.params["amount"].isString())
        amount = std::stoull(context.params["amount"].asString());
    else if (context.params["amount"].isIntegral())
        amount = context.params["amount"].asUInt();

    if (amount == 0)
    {
        json::Value jvResult;
        jvResult["error"] = "invalidParams";
        jvResult["error_message"] = "Amount must be positive";
        return jvResult;
    }

    std::uint32_t sequence = 0;
    if (context.params.isMember("sequence"))
    {
        if (context.params["sequence"].isIntegral())
            sequence = context.params["sequence"].asUInt();
    }

    inner::InnerTx tx;
    tx.type = txType;
    tx.source = *src;
    tx.destination = *dst;
    tx.amount = amount;
    tx.sequence = sequence;

    auto const txHash = tx.id();

    if (!engine->submit(std::move(tx)))
    {
        json::Value jvResult;
        jvResult["error"] = "innerChainBusy";
        jvResult["error_message"] = "Inner chain transaction queue is full";
        jvResult["chain_id"] = engine->networkId();
        return jvResult;
    }

    json::Value jvResult;
    jvResult["status"] = "success";
    jvResult["tx_type"] = txTypeStr;
    jvResult["source"] = toBase58(*src);
    jvResult["destination"] = toBase58(*dst);
    jvResult["amount"] = std::to_string(amount);
    jvResult["sequence"] = sequence;
    jvResult["chain_id"] = engine->networkId();
    jvResult["chain_name"] = engine->config().name;
    jvResult["tx_hash"] = to_string(txHash);
    jvResult["message"] = "Transaction queued for next inner chain ledger";
    return jvResult;
}

json::Value
doInnerLedger(RPC::JsonContext& context)
{
    auto* engine = resolveEngine(context);
    if (!engine)
        return noEngineError(context);

    std::shared_ptr<inner::InnerLedger const> ledger;

    if (context.params.isMember("ledger_index"))
    {
        auto const& val = context.params["ledger_index"];
        if (val.isString() && val.asString() == "validated")
        {
            ledger = engine->ledgerMaster().getValidatedLedger();
        }
        else if (val.isString() && val.asString() == "closed")
        {
            ledger = engine->ledgerMaster().getClosedLedger();
        }
        else
        {
            std::uint32_t seq = 0;
            if (val.isIntegral())
                seq = val.asUInt();
            else if (val.isString())
                seq = std::stoul(val.asString());
            ledger = engine->ledgerMaster().getLedgerBySeq(seq);
        }
    }
    else
    {
        ledger = engine->ledgerMaster().getValidatedLedger();
    }

    if (!ledger)
    {
        json::Value jvResult;
        jvResult["error"] = "lgrNotFound";
        jvResult["error_message"] = "Ledger not found";
        jvResult["chain_id"] = engine->networkId();
        return jvResult;
    }

    auto const& hdr = ledger->header();

    json::Value jvResult;
    jvResult["status"] = "success";
    jvResult["chain_id"] = engine->networkId();
    jvResult["chain_name"] = engine->config().name;

    json::Value header(json::ObjectValue);
    header["seq"] = hdr.seq;
    header["hash"] = to_string(hdr.hash);
    header["parent_hash"] = to_string(hdr.parentHash);
    header["state_hash"] = to_string(hdr.stateHash);
    header["tx_hash"] = to_string(hdr.txHash);
    header["close_time_agree"] = hdr.closeTimeAgree;

    if (hdr.outerCheckpointSeq > 0)
    {
        header["outer_checkpoint_seq"] = hdr.outerCheckpointSeq;
        header["outer_checkpoint_hash"] =
            to_string(hdr.outerCheckpointHash);
    }

    jvResult["ledger"] = std::move(header);

    json::Value txs(json::ArrayValue);
    if (auto const& txMap = ledger->txMap())
    {
        for (auto it = txMap->begin(); it != txMap->end(); ++it)
        {
            auto const& item = *it;
            auto optTx =
                inner::InnerTx::deserialize(item.slice());
            if (optTx)
            {
                json::Value jtx(json::ObjectValue);
                jtx["hash"] = to_string(optTx->id());
                jtx["type"] = static_cast<int>(optTx->type);
                jtx["source"] = toBase58(optTx->source);
                jtx["destination"] = toBase58(optTx->destination);
                jtx["amount"] = std::to_string(optTx->amount);
                jtx["sequence"] = optTx->sequence;
                txs.append(std::move(jtx));
            }
        }
    }
    auto const txCount = txs.size();
    jvResult["transactions"] = std::move(txs);
    jvResult["tx_count"] = static_cast<int>(txCount);

    return jvResult;
}

}  // namespace xrpl
