#include <xrpld/app/misc/ImportCreditVote.h>

#include <xrpld/app/main/Application.h>
#include <xrpld/app/misc/MainnetWatcher.h>

#include <xrpl/ledger/ReadView.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/STAmount.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/shamap/SHAMap.h>
#include <xrpl/shamap/SHAMapItem.h>
#include <xrpl/shamap/SHAMapTreeNode.h>

namespace xrpl {

ImportCreditVote::ImportCreditVote(Application& app, beast::Journal journal)
    : app_(app), journal_(journal)
{
}

void
ImportCreditVote::doVoting(
    std::shared_ptr<ReadView const> const& prevLedger,
    std::shared_ptr<SHAMap> const& initialSet)
{
    auto* watcher = app_.getMainnetWatcher();
    if (!watcher)
        return;

    auto const seq = prevLedger->seq() + 1;

    // Consume all ready imports from the MainnetWatcher
    auto imports = watcher->consumeReadyImports();
    if (imports.empty())
        return;

    for (auto& ri : imports)
    {
        // Skip if already proposed
        if (proposedHashes_.count(ri.mainnetTxHash))
            continue;

        // Skip if already imported (replay check via ImportSequence)
        auto const sle =
            prevLedger->read(keylet::account(ri.sidechainAccount));
        if (sle &&
            sle->getFieldU32(sfImportSequence) >= ri.importSequence)
        {
            proposedHashes_.insert(ri.mainnetTxHash);
            continue;
        }

        // Build deterministic ttIMPORT_CREDIT pseudo-transaction.
        // All validators with the same MainnetWatcher data will
        // produce identical bytes.
        STTx importCreditTx(ttIMPORT_CREDIT, [&](auto& obj) {
            obj.setAccountID(sfDestination, ri.sidechainAccount);
            obj.setFieldAmount(
                sfAmount,
                amountFromJson(
                    sfAmount,
                    ri.xpop["transaction"]["meta"]["DeliveredAmount"]));
            obj.setFieldH256(sfSourceTxnID, ri.mainnetTxHash);
            obj.setFieldU32(sfImportSequence, ri.importSequence);
            obj.setFieldU32(sfLedgerSequence, seq);

            // If the inner tx has InvoiceID, pass it through
            // for RegularKey derivation on first import
            auto const& innerTx = ri.xpop["transaction"]["blob"];
            if (innerTx.isObject() && innerTx.isMember("InvoiceID"))
            {
                uint256 invoiceID;
                if (invoiceID.parseHex(innerTx["InvoiceID"].asString()))
                    obj.setFieldH256(sfInvoiceID, invoiceID);
            }
        });

        uint256 const txID = importCreditTx.getTransactionID();
        Serializer s;
        importCreditTx.add(s);

        if (!initialSet->addGiveItem(
                SHAMapNodeType::tnTRANSACTION_NM,
                make_shamapitem(txID, s.slice())))
        {
            JLOG(journal_.warn())
                << "ImportCreditVote: failed to add to initial set, "
                << "txID=" << txID;
        }
        else
        {
            proposedHashes_.insert(ri.mainnetTxHash);

            JLOG(journal_.info())
                << "ImportCreditVote: proposed import credit for "
                << ri.sidechainAccount << " seq=" << ri.importSequence
                << " txID=" << txID;
        }
    }

    // Prune old proposed hashes (keep last 1024)
    while (proposedHashes_.size() > 1024)
        proposedHashes_.erase(proposedHashes_.begin());
}

}  // namespace xrpl
