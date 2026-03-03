#include <xrpl/tx/transactors/Import/Export.h>

#include <xrpl/basics/Log.h>
#include <xrpl/ledger/View.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/jss.h>

#include <cstdint>
#include <limits>

namespace xrpl {

NotTEC
Export::preflight(PreflightContext const& ctx)
{
    if (!ctx.rules.enabled(featureImportExport))
    {
        JLOG(ctx.j.warn()) << "Export: featureImportExport is not enabled";
        return temDISABLED;
    }

    auto& tx = ctx.tx;

    STAmount const amount = tx.getFieldAmount(sfAmount);
    if (!isXRP(amount) || amount <= beast::zero)
    {
        JLOG(ctx.j.warn()) << "Export: amount must be positive XRP";
        return temBAD_AMOUNT;
    }

    return tesSUCCESS;
}

TER
Export::preclaim(PreclaimContext const& ctx)
{
    auto const& sle = ctx.view.read(keylet::account(ctx.tx[sfAccount]));
    if (!sle)
        return terNO_ACCOUNT;

    STAmount const amount = ctx.tx.getFieldAmount(sfAmount);
    STAmount const balance = sle->getFieldAmount(sfBalance);
    STAmount const fee = ctx.tx.getFieldAmount(sfFee);

    // Must have enough to cover amount + fee + reserve
    // ExportRecord is NOT in owner directory (no +1), burn is anti-spam
    auto const reserve = ctx.view.fees().accountReserve(
        sle->getFieldU32(sfOwnerCount));

    if (balance < amount + fee + STAmount(reserve))
    {
        JLOG(ctx.j.warn()) << "Export: insufficient balance";
        return tecUNFUNDED;
    }

    // If validator-signed exports are enabled, check ticket availability.
    // The ExportVaultState singleton is created lazily in doApply() if
    // it doesn't already exist (e.g. before the amendment activation
    // pseudo-tx fires).  Only check ticket exhaustion when the vault
    // is already on ledger.
    if (ctx.view.rules().enabled(featureImportExport))
    {
        auto const sleVault =
            ctx.view.read(keylet::exportVaultState());
        if (sleVault)
        {
            auto const nextTicket =
                sleVault->getFieldU32(sfNextTicketSeq);
            auto const maxTicket =
                sleVault->getFieldU32(sfMaxTicketSeq);

            if (nextTicket > maxTicket)
            {
                JLOG(ctx.j.warn())
                    << "Export: No mainnet tickets available";
                return tecINTERNAL;
            }
        }
    }

    return tesSUCCESS;
}

TER
Export::doApply()
{
    auto const id = ctx_.tx[sfAccount];
    auto sle = view().peek(keylet::account(id));
    if (!sle)
        return tefINTERNAL;

    STAmount const amount = ctx_.tx.getFieldAmount(sfAmount);

    // Debit account balance
    STAmount const balance = sle->getFieldAmount(sfBalance);
    sle->setFieldAmount(sfBalance, balance - amount);

    // Get and increment export sequence
    uint32_t exportSeq = 0;
    if (sle->isFieldPresent(sfExportSequence))
        exportSeq = sle->getFieldU32(sfExportSequence);
    if (exportSeq == std::numeric_limits<uint32_t>::max())
        return tefINTERNAL;
    sle->setFieldU32(sfExportSequence, exportSeq + 1);

    view().update(sle);

    // Create ExportRecord ledger entry
    auto const exportKeylet = keylet::exportRecord(id, exportSeq);
    auto sleExport = std::make_shared<SLE>(exportKeylet);

    sleExport->setAccountID(sfAccount, id);
    sleExport->setAccountID(sfDestination, ctx_.tx[sfDestination]);
    sleExport->setFieldAmount(sfAmount, amount);
    sleExport->setFieldU32(sfExportSequence, exportSeq);

    if (ctx_.tx.isFieldPresent(sfDestinationTag))
        sleExport->setFieldU32(
            sfDestinationTag, ctx_.tx.getFieldU32(sfDestinationTag));

    // Assign mainnet ticket if validator-signed exports are enabled
    if (view().rules().enabled(featureImportExport))
    {
        auto const vaultKeylet = keylet::exportVaultState();
        auto sleVault = view().peek(vaultKeylet);

        // Lazily create ExportVaultState if it doesn't exist yet.
        // In production this is created during amendment activation;
        // this fallback handles the window before the activation
        // pseudo-tx fires or standalone/test environments.
        if (!sleVault)
        {
            sleVault = std::make_shared<SLE>(vaultKeylet);
            sleVault->setFieldU32(sfNextTicketSeq, 1);
            sleVault->setFieldU32(sfMaxTicketSeq, 250);
            sleVault->setFieldU32(sfExportQuorum, 0);
            sleVault->setFieldU32(sfSignerCount, 0);
            sleVault->setFieldH256(sfPreviousTxnID, uint256{});
            sleVault->setFieldU32(sfPreviousTxnLgrSeq, 0);
            view().insert(sleVault);
        }

        auto const ticketSeq = sleVault->getFieldU32(sfNextTicketSeq);
        if (ticketSeq == std::numeric_limits<uint32_t>::max())
            return tefINTERNAL;
        sleExport->setFieldU32(sfTicketSequence, ticketSeq);

        // Advance to next ticket
        sleVault->setFieldU32(sfNextTicketSeq, ticketSeq + 1);
        sleVault->setFieldH256(sfPreviousTxnID, ctx_.tx.getTransactionID());
        sleVault->setFieldU32(
            sfPreviousTxnLgrSeq, ctx_.view().seq());
        view().update(sleVault);
    }

    // Record the ledger sequence for stale-pruning
    sleExport->setFieldU32(sfLedgerSequence, ctx_.view().seq());

    sleExport->setFieldH256(sfPreviousTxnID, ctx_.tx.getTransactionID());
    sleExport->setFieldU32(sfPreviousTxnLgrSeq, ctx_.view().seq());

    // Add to global export directory (not owner directory — no reserve)
    auto const page = view().dirInsert(
        keylet::exportDir(), exportKeylet, [](std::shared_ptr<SLE> const&) {});

    if (!page)
        return tecDIR_FULL;

    sleExport->setFieldU64(sfExportDirNode, *page);

    view().insert(sleExport);

    // Destroy XRP from supply (burn)
    ctx_.rawView().rawDestroyXRP(amount.xrp());

    return tesSUCCESS;
}

}  // namespace xrpl
