#include <xrpld/app/misc/ExportConfirmVote.h>

#include <xrpld/app/main/Application.h>
#include <xrpld/app/misc/MainnetWatcher.h>

#include <xrpl/ledger/ReadView.h>
#include <xrpl/ledger/View.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/LedgerFormats.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/shamap/SHAMap.h>
#include <xrpl/shamap/SHAMapItem.h>
#include <xrpl/shamap/SHAMapTreeNode.h>

namespace xrpl {

ExportConfirmVote::ExportConfirmVote(Application& app, beast::Journal journal)
    : app_(app), journal_(journal)
{
}

void
ExportConfirmVote::doVoting(
    std::shared_ptr<ReadView const> const& prevLedger,
    std::shared_ptr<SHAMap> const& initialSet)
{
    auto* watcher = app_.getMainnetWatcher();
    if (!watcher)
        return;

    auto const seq = prevLedger->seq() + 1;

    auto confirmed = watcher->consumeConfirmedVaultTxs();
    if (confirmed.empty())
        return;

    for (auto& ctx : confirmed)
    {
        // Skip if already proposed
        if (proposedHashes_.count(ctx.txHash))
            continue;

        if (ctx.txType == "Payment" && ctx.ticketSequence != 0)
        {
            // Find the ExportRecord matching this ticket sequence
            AccountID owner;
            std::uint32_t exportSeq = 0;
            bool found = false;

            forEachItem(
                *prevLedger,
                keylet::exportDir(),
                [&](std::shared_ptr<SLE const> const& sle) {
                    if (found)
                        return;
                    if (!sle || !sle->isFieldPresent(sfTicketSequence))
                        return;
                    if (sle->getFieldU32(sfTicketSequence) !=
                        ctx.ticketSequence)
                        return;
                    // Already confirmed — skip
                    if (sle->isFlag(lsfExportConfirmed))
                        return;
                    owner = sle->getAccountID(sfOwner);
                    exportSeq = sle->getFieldU32(sfExportSequence);
                    found = true;
                });

            if (!found)
            {
                JLOG(journal_.info())
                    << "ExportConfirmVote: No matching ExportRecord for "
                    << "ticket=" << ctx.ticketSequence;
                proposedHashes_.insert(ctx.txHash);
                continue;
            }

            STTx exportConfirmTx(ttEXPORT_CONFIRM, [&](auto& obj) {
                obj.setFieldU32(sfTicketSequence, ctx.ticketSequence);
                obj.setAccountID(sfOwner, owner);
                obj.setFieldU32(sfExportSequence, exportSeq);
                obj.setFieldH256(sfSourceTxnID, ctx.txHash);
                obj.setFieldU32(sfLedgerSequence, seq);
            });

            uint256 const txID = exportConfirmTx.getTransactionID();
            Serializer s;
            exportConfirmTx.add(s);

            if (!initialSet->addGiveItem(
                    SHAMapNodeType::tnTRANSACTION_NM,
                    make_shamapitem(txID, s.slice())))
            {
                JLOG(journal_.warn())
                    << "ExportConfirmVote: failed to add Payment confirm, "
                    << "txID=" << txID;
            }
            else
            {
                proposedHashes_.insert(ctx.txHash);

                JLOG(journal_.info())
                    << "ExportConfirmVote: proposed confirm for Payment, "
                    << "ticket=" << ctx.ticketSequence
                    << " owner=" << owner
                    << " exportSeq=" << exportSeq
                    << " txID=" << txID;
            }

            continue;
        }

        if (ctx.txType != "TicketCreate")
        {
            proposedHashes_.insert(ctx.txHash);
            continue;
        }

        // Read ExportVaultState to get current sfMainnetSequence
        auto const sleVault =
            prevLedger->read(keylet::exportVaultState());
        if (!sleVault)
        {
            JLOG(journal_.warn())
                << "ExportConfirmVote: No ExportVaultState";
            continue;
        }

        auto const currentMax =
            sleVault->getFieldU32(sfMaxTicketSeq);

        // Compute new max ticket seq:
        // TicketCreate creates tickets starting at the pre-tx Sequence.
        // After the tx, Sequence advances by ticketCount.
        // New tickets occupy: [newSequence - ticketCount, newSequence - 1]
        // So newMaxTicketSeq = newSequence - 1
        auto const newMaxTicketSeq = ctx.newSequence - 1;

        if (newMaxTicketSeq <= currentMax)
        {
            JLOG(journal_.info())
                << "ExportConfirmVote: TicketCreate new max "
                << newMaxTicketSeq << " <= current " << currentMax
                << ", skipping";
            proposedHashes_.insert(ctx.txHash);
            continue;
        }

        // Build deterministic ttEXPORT_CONFIRM pseudo-transaction
        STTx exportConfirmTx(ttEXPORT_CONFIRM, [&](auto& obj) {
            obj.setFieldU32(sfMaxTicketSeq, newMaxTicketSeq);
            obj.setFieldU32(sfMainnetSequence, ctx.newSequence);
            obj.setFieldH256(sfSourceTxnID, ctx.txHash);
            obj.setFieldU32(sfLedgerSequence, seq);
        });

        uint256 const txID = exportConfirmTx.getTransactionID();
        Serializer s;
        exportConfirmTx.add(s);

        if (!initialSet->addGiveItem(
                SHAMapNodeType::tnTRANSACTION_NM,
                make_shamapitem(txID, s.slice())))
        {
            JLOG(journal_.warn())
                << "ExportConfirmVote: failed to add to initial set, "
                << "txID=" << txID;
        }
        else
        {
            proposedHashes_.insert(ctx.txHash);

            JLOG(journal_.info())
                << "ExportConfirmVote: proposed confirm for "
                << "TicketCreate, newMaxTicket=" << newMaxTicketSeq
                << " newSeq=" << ctx.newSequence
                << " txID=" << txID;
        }
    }

    // Prune old proposed hashes (keep last 1024)
    while (proposedHashes_.size() > 1024)
        proposedHashes_.erase(proposedHashes_.begin());
}

}  // namespace xrpl
