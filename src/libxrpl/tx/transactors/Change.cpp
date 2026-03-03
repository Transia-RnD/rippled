#include <xrpl/basics/Log.h>
#include <xrpl/basics/StringUtilities.h>
#include <xrpl/ledger/AmendmentTable.h>
#include <xrpl/ledger/Sandbox.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/server/NetworkOPs.h>
#include <xrpl/tx/transactors/Change.h>
#include <xrpl/tx/transactors/Import/Import.h>

#include <cstring>
#include <map>
#include <string_view>

namespace xrpl {

template <>
NotTEC
Transactor::invokePreflight<Change>(PreflightContext const& ctx)
{
    // 0 means "Allow any flags"
    // The check for tfChangeMask is gated by LendingProtocol because that
    // feature introduced this parameter, and it's not worth adding another
    // amendment just for this.
    if (auto const ret =
            preflight0(ctx, ctx.rules.enabled(featureLendingProtocol) ? tfChangeMask : 0))
        return ret;

    auto account = ctx.tx.getAccountID(sfAccount);
    if (account != beast::zero)
    {
        JLOG(ctx.j.warn()) << "Change: Bad source id";
        return temBAD_SRC_ACCOUNT;
    }

    // No point in going any further if the transaction fee is malformed.
    auto const fee = ctx.tx.getFieldAmount(sfFee);
    if (!fee.native() || fee != beast::zero)
    {
        JLOG(ctx.j.warn()) << "Change: invalid fee";
        return temBAD_FEE;
    }

    if (!ctx.tx.getSigningPubKey().empty() || !ctx.tx.getSignature().empty() ||
        ctx.tx.isFieldPresent(sfSigners))
    {
        JLOG(ctx.j.warn()) << "Change: Bad signature";
        return temBAD_SIGNATURE;
    }

    if (ctx.tx.getFieldU32(sfSequence) != 0 || ctx.tx.isFieldPresent(sfPreviousTxnID))
    {
        JLOG(ctx.j.warn()) << "Change: Bad sequence";
        return temBAD_SEQUENCE;
    }

    if (ctx.tx.getTxnType() == ttUNL_REPORT)
    {
        if (!ctx.rules.enabled(featureImportExport))
        {
            JLOG(ctx.j.warn()) << "Change: UNLReport is not enabled.";
            return temDISABLED;
        }

        if (!ctx.tx.isFieldPresent(sfActiveValidator) &&
            !ctx.tx.isFieldPresent(sfImportVLKey))
        {
            JLOG(ctx.j.warn()) << "Change: UNLReport must specify at least one "
                                  "of sfImportVLKey, sfActiveValidator";
            return temMALFORMED;
        }
    }

    if (ctx.tx.getTxnType() == ttIMPORT_CREDIT)
    {
        if (!ctx.rules.enabled(featureImportExport))
        {
            JLOG(ctx.j.warn()) << "Change: ImportCredit is not enabled.";
            return temDISABLED;
        }

        if (!ctx.tx.isFieldPresent(sfDestination) ||
            !ctx.tx.isFieldPresent(sfAmount) ||
            !ctx.tx.isFieldPresent(sfSourceTxnID) ||
            !ctx.tx.isFieldPresent(sfImportSequence) ||
            !ctx.tx.isFieldPresent(sfLedgerSequence))
        {
            JLOG(ctx.j.warn()) << "Change: ImportCredit missing required fields";
            return temMALFORMED;
        }

        STAmount const amount = ctx.tx.getFieldAmount(sfAmount);
        if (!isXRP(amount) || amount <= beast::zero)
        {
            JLOG(ctx.j.warn())
                << "Change: ImportCredit amount must be positive XRP";
            return temMALFORMED;
        }
    }

    if (ctx.tx.getTxnType() == ttEXPORT_CONFIRM)
    {
        if (!ctx.rules.enabled(featureImportExport))
        {
            JLOG(ctx.j.warn()) << "Change: ExportConfirm is not enabled.";
            return temDISABLED;
        }

        if (!ctx.tx.isFieldPresent(sfSourceTxnID) ||
            !ctx.tx.isFieldPresent(sfLedgerSequence))
        {
            JLOG(ctx.j.warn())
                << "Change: ExportConfirm missing required fields";
            return temMALFORMED;
        }
    }

    return tesSUCCESS;
}

TER
Change::preclaim(PreclaimContext const& ctx)
{
    // If tapOPEN_LEDGER is resurrected into ApplyFlags,
    // this block can be moved to preflight.
    if (ctx.view.open())
    {
        JLOG(ctx.j.warn()) << "Change transaction against open ledger";
        return temINVALID;
    }

    switch (ctx.tx.getTxnType())
    {
        case ttFEE:
            if (ctx.view.rules().enabled(featureXRPFees))
            {
                // The ttFEE transaction format defines these fields as
                // optional, but once the XRPFees feature is enabled, they are
                // required.
                if (!ctx.tx.isFieldPresent(sfBaseFeeDrops) ||
                    !ctx.tx.isFieldPresent(sfReserveBaseDrops) ||
                    !ctx.tx.isFieldPresent(sfReserveIncrementDrops))
                    return temMALFORMED;
                // The ttFEE transaction format defines these fields as
                // optional, but once the XRPFees feature is enabled, they are
                // forbidden.
                if (ctx.tx.isFieldPresent(sfBaseFee) ||
                    ctx.tx.isFieldPresent(sfReferenceFeeUnits) ||
                    ctx.tx.isFieldPresent(sfReserveBase) ||
                    ctx.tx.isFieldPresent(sfReserveIncrement))
                    return temMALFORMED;
            }
            else
            {
                // The ttFEE transaction format formerly defined these fields
                // as required. When the XRPFees feature was implemented, they
                // were changed to be optional. Until the feature has been
                // enabled, they are required.
                if (!ctx.tx.isFieldPresent(sfBaseFee) ||
                    !ctx.tx.isFieldPresent(sfReferenceFeeUnits) ||
                    !ctx.tx.isFieldPresent(sfReserveBase) ||
                    !ctx.tx.isFieldPresent(sfReserveIncrement))
                    return temMALFORMED;
                // The ttFEE transaction format defines these fields as
                // optional, but without the XRPFees feature, they are
                // forbidden.
                if (ctx.tx.isFieldPresent(sfBaseFeeDrops) ||
                    ctx.tx.isFieldPresent(sfReserveBaseDrops) ||
                    ctx.tx.isFieldPresent(sfReserveIncrementDrops))
                    return temDISABLED;
            }
            return tesSUCCESS;
        case ttAMENDMENT:
        case ttUNL_MODIFY:
            return tesSUCCESS;
        case ttUNL_REPORT: {
            if (!ctx.tx.isFieldPresent(sfImportVLKey) ||
                !ctx.registry.hasImportVLKeys())
                return tesSUCCESS;

            // if we do specify import_vl_keys in config then we won't approve
            // keys that aren't on our list and/or aren't in the ledger object
            auto const& inner = const_cast<xrpl::STTx&>(ctx.tx)
                                    .getField(sfImportVLKey)
                                    .downcast<STObject>();
            auto const pkBlob = inner.getFieldVL(sfPublicKey);
            std::string const strPk = strHex(makeSlice(pkBlob));
            if (ctx.registry.isImportVLKeyRecognized(strPk))
                return tesSUCCESS;

            auto const pkType = publicKeyType(makeSlice(pkBlob));
            if (!pkType)
                return tefINTERNAL;

            PublicKey const pk(makeSlice(pkBlob));

            // check on ledger
            if (auto const unlRep = ctx.view.read(keylet::UNLReport());
                unlRep && unlRep->isFieldPresent(sfImportVLKeys))
            {
                auto const& vlKeys =
                    unlRep->getFieldArray(sfImportVLKeys);
                for (auto const& k : vlKeys)
                {
                    auto const kPkBlob = k.getFieldVL(sfPublicKey);
                    if (publicKeyType(makeSlice(kPkBlob)) &&
                        PublicKey(makeSlice(kPkBlob)) == pk)
                        return tesSUCCESS;
                }
            }

            return telIMPORT_VL_KEY_NOT_RECOGNISED;
        }
        case ttIMPORT_CREDIT:
        case ttEXPORT_CONFIRM:
            return tesSUCCESS;
        default:
            return temUNKNOWN;
    }
}

TER
Change::doApply()
{
    switch (ctx_.tx.getTxnType())
    {
        case ttAMENDMENT:
            return applyAmendment();
        case ttFEE:
            return applyFee();
        case ttUNL_MODIFY:
            return applyUNLModify();
        case ttUNL_REPORT:
            return applyUNLReport();
        case ttIMPORT_CREDIT:
            return applyImportCredit();
        case ttEXPORT_CONFIRM:
            return applyExportConfirm();
        // LCOV_EXCL_START
        default:
            UNREACHABLE("xrpl::Change::doApply : invalid transaction type");
            return tefFAILURE;
            // LCOV_EXCL_STOP
    }
}

void
Change::preCompute()
{
    XRPL_ASSERT(account_ == beast::zero, "xrpl::Change::preCompute : zero account");
}

TER
Change::applyAmendment()
{
    uint256 amendment(ctx_.tx.getFieldH256(sfAmendment));

    auto const k = keylet::amendments();

    SLE::pointer amendmentObject = view().peek(k);

    if (!amendmentObject)
    {
        amendmentObject = std::make_shared<SLE>(k);
        view().insert(amendmentObject);
    }

    STVector256 amendments = amendmentObject->getFieldV256(sfAmendments);

    if (std::find(amendments.begin(), amendments.end(), amendment) != amendments.end())
        return tefALREADY;

    auto flags = ctx_.tx.getFlags();

    bool const gotMajority = (flags & tfGotMajority) != 0;
    bool const lostMajority = (flags & tfLostMajority) != 0;

    if (gotMajority && lostMajority)
        return temINVALID_FLAG;

    STArray newMajorities(sfMajorities);

    bool found = false;
    if (amendmentObject->isFieldPresent(sfMajorities))
    {
        STArray const& oldMajorities = amendmentObject->getFieldArray(sfMajorities);
        for (auto const& majority : oldMajorities)
        {
            if (majority.getFieldH256(sfAmendment) == amendment)
            {
                if (gotMajority)
                    return tefALREADY;
                found = true;
            }
            else
            {
                // pass through
                newMajorities.push_back(majority);
            }
        }
    }

    if (!found && lostMajority)
        return tefALREADY;

    if (gotMajority)
    {
        // This amendment now has a majority
        newMajorities.push_back(STObject::makeInnerObject(sfMajority));
        auto& entry = newMajorities.back();
        entry[sfAmendment] = amendment;
        entry[sfCloseTime] = view().parentCloseTime().time_since_epoch().count();

        if (!ctx_.registry.getAmendmentTable().isSupported(amendment))
        {
            JLOG(j_.warn()) << "Unsupported amendment " << amendment << " received a majority.";
        }
    }
    else if (!lostMajority)
    {
        // No flags, enable amendment
        amendments.push_back(amendment);
        amendmentObject->setFieldV256(sfAmendments, amendments);

        ctx_.registry.getAmendmentTable().enable(amendment);

        if (!ctx_.registry.getAmendmentTable().isSupported(amendment))
        {
            JLOG(j_.error()) << "Unsupported amendment " << amendment
                             << " activated: server blocked.";
            ctx_.registry.getOPs().setAmendmentBlocked();
        }

        // Create ExportVaultState singleton when ImportExport activates.
        // Initial ticket range is pre-allocated on mainnet during sidechain
        // setup.
        if (amendment == featureImportExport)
        {
            auto const vaultKeylet = keylet::exportVaultState();
            if (!view().peek(vaultKeylet))
            {
                auto const firstTicket =
                    ctx_.registry.getImportVaultFirstTicket().value_or(1);
                auto const maxTicket =
                    ctx_.registry.getImportVaultMaxTicket().value_or(
                        firstTicket + 249);

                auto sle = std::make_shared<SLE>(vaultKeylet);
                sle->setFieldU32(sfNextTicketSeq, firstTicket);
                sle->setFieldU32(sfMaxTicketSeq, maxTicket);
                auto const mainnetSeq =
                    ctx_.registry.getImportVaultMainnetSequence();
                if (mainnetSeq)
                    sle->setFieldU32(sfMainnetSequence, *mainnetSeq);
                sle->setFieldH256(sfPreviousTxnID, uint256{});
                sle->setFieldU32(sfPreviousTxnLgrSeq, 0);
                view().insert(sle);
            }
        }
    }

    if (newMajorities.empty())
        amendmentObject->makeFieldAbsent(sfMajorities);
    else
        amendmentObject->setFieldArray(sfMajorities, newMajorities);

    view().update(amendmentObject);

    return tesSUCCESS;
}

TER
Change::applyFee()
{
    auto const k = keylet::fees();

    SLE::pointer feeObject = view().peek(k);

    if (!feeObject)
    {
        feeObject = std::make_shared<SLE>(k);
        view().insert(feeObject);
    }
    auto set = [](SLE::pointer& feeObject, STTx const& tx, auto const& field) {
        feeObject->at(field) = tx[field];
    };
    if (view().rules().enabled(featureXRPFees))
    {
        set(feeObject, ctx_.tx, sfBaseFeeDrops);
        set(feeObject, ctx_.tx, sfReserveBaseDrops);
        set(feeObject, ctx_.tx, sfReserveIncrementDrops);
        // Ensure the old fields are removed
        feeObject->makeFieldAbsent(sfBaseFee);
        feeObject->makeFieldAbsent(sfReferenceFeeUnits);
        feeObject->makeFieldAbsent(sfReserveBase);
        feeObject->makeFieldAbsent(sfReserveIncrement);
    }
    else
    {
        set(feeObject, ctx_.tx, sfBaseFee);
        set(feeObject, ctx_.tx, sfReferenceFeeUnits);
        set(feeObject, ctx_.tx, sfReserveBase);
        set(feeObject, ctx_.tx, sfReserveIncrement);
    }

    view().update(feeObject);

    JLOG(j_.warn()) << "Fees have been changed";
    return tesSUCCESS;
}

TER
Change::applyUNLModify()
{
    if (!isFlagLedger(view().seq()))
    {
        JLOG(j_.warn()) << "N-UNL: applyUNLModify, not a flag ledger, seq=" << view().seq();
        return tefFAILURE;
    }

    if (!ctx_.tx.isFieldPresent(sfUNLModifyDisabling) ||
        ctx_.tx.getFieldU8(sfUNLModifyDisabling) > 1 || !ctx_.tx.isFieldPresent(sfLedgerSequence) ||
        !ctx_.tx.isFieldPresent(sfUNLModifyValidator))
    {
        JLOG(j_.warn()) << "N-UNL: applyUNLModify, wrong Tx format.";
        return tefFAILURE;
    }

    bool const disabling = ctx_.tx.getFieldU8(sfUNLModifyDisabling);
    auto const seq = ctx_.tx.getFieldU32(sfLedgerSequence);
    if (seq != view().seq())
    {
        JLOG(j_.warn()) << "N-UNL: applyUNLModify, wrong ledger seq=" << seq;
        return tefFAILURE;
    }

    Blob const validator = ctx_.tx.getFieldVL(sfUNLModifyValidator);
    if (!publicKeyType(makeSlice(validator)))
    {
        JLOG(j_.warn()) << "N-UNL: applyUNLModify, bad validator key";
        return tefFAILURE;
    }

    JLOG(j_.info()) << "N-UNL: applyUNLModify, " << (disabling ? "ToDisable" : "ToReEnable")
                    << " seq=" << seq << " validator data:" << strHex(validator);

    auto const k = keylet::negativeUNL();
    SLE::pointer negUnlObject = view().peek(k);
    if (!negUnlObject)
    {
        negUnlObject = std::make_shared<SLE>(k);
        view().insert(negUnlObject);
    }

    bool const found = [&] {
        if (negUnlObject->isFieldPresent(sfDisabledValidators))
        {
            auto const& negUnl = negUnlObject->getFieldArray(sfDisabledValidators);
            for (auto const& v : negUnl)
            {
                if (v.isFieldPresent(sfPublicKey) && v.getFieldVL(sfPublicKey) == validator)
                    return true;
            }
        }
        return false;
    }();

    if (disabling)
    {
        // cannot have more than one toDisable
        if (negUnlObject->isFieldPresent(sfValidatorToDisable))
        {
            JLOG(j_.warn()) << "N-UNL: applyUNLModify, already has ToDisable";
            return tefFAILURE;
        }

        // cannot be the same as toReEnable
        if (negUnlObject->isFieldPresent(sfValidatorToReEnable))
        {
            if (negUnlObject->getFieldVL(sfValidatorToReEnable) == validator)
            {
                JLOG(j_.warn()) << "N-UNL: applyUNLModify, ToDisable is same as ToReEnable";
                return tefFAILURE;
            }
        }

        // cannot be in negative UNL already
        if (found)
        {
            JLOG(j_.warn()) << "N-UNL: applyUNLModify, ToDisable already in negative UNL";
            return tefFAILURE;
        }

        negUnlObject->setFieldVL(sfValidatorToDisable, validator);
    }
    else
    {
        // cannot have more than one toReEnable
        if (negUnlObject->isFieldPresent(sfValidatorToReEnable))
        {
            JLOG(j_.warn()) << "N-UNL: applyUNLModify, already has ToReEnable";
            return tefFAILURE;
        }

        // cannot be the same as toDisable
        if (negUnlObject->isFieldPresent(sfValidatorToDisable))
        {
            if (negUnlObject->getFieldVL(sfValidatorToDisable) == validator)
            {
                JLOG(j_.warn()) << "N-UNL: applyUNLModify, ToReEnable is same as ToDisable";
                return tefFAILURE;
            }
        }

        // must be in negative UNL
        if (!found)
        {
            JLOG(j_.warn()) << "N-UNL: applyUNLModify, ToReEnable is not in negative UNL";
            return tefFAILURE;
        }

        negUnlObject->setFieldVL(sfValidatorToReEnable, validator);
    }

    view().update(negUnlObject);
    return tesSUCCESS;
}

TER
Change::applyUNLReport()
{
    // Follows xahaud's applyUNLReport pattern exactly:
    // Each pseudo-tx carries a single sfActiveValidator (OBJECT).
    // Multiple pseudo-txs accumulate into the UNLReport SLE's
    // sfActiveValidators (ARRAY) over the flag ledger.

    auto sle = view().peek(keylet::UNLReport());

    auto const seq = view().seq();

    bool const created = !sle;

    if (created)
        sle = std::make_shared<SLE>(keylet::UNLReport());

    // Reset detection: if previous update was from a prior ledger,
    // start fresh (don't carry forward stale validators)
    bool const reset = sle->isFieldPresent(sfPreviousTxnLgrSeq) &&
        sle->getFieldU32(sfPreviousTxnLgrSeq) < seq;

    // Canonicalize: merge existing entries with the new one from this tx.
    // Uses std::map<PublicKey, AccountID> for deterministic ordering.
    auto canonicalize =
        [&](SField const& arrayType,
            SField const& objType) -> std::vector<STObject> {
        auto const existing = reset || !sle->isFieldPresent(arrayType)
            ? STArray(arrayType)
            : sle->getFieldArray(arrayType);

        std::map<PublicKey, AccountID> ordered;
        for (auto const& obj : existing)
        {
            auto pk = obj.getFieldVL(sfPublicKey);
            if (!publicKeyType(makeSlice(pk)))
                continue;

            PublicKey p(makeSlice(pk));
            ordered.emplace(
                p,
                obj.isFieldPresent(sfAccount)
                    ? obj.getAccountID(sfAccount)
                    : calcAccountID(p));
        };

        if (ctx_.tx.isFieldPresent(objType))
        {
            auto pk = const_cast<xrpl::STTx&>(ctx_.tx)
                          .getField(objType)
                          .downcast<STObject>()
                          .getFieldVL(sfPublicKey);

            if (publicKeyType(makeSlice(pk)))
            {
                PublicKey p(makeSlice(pk));
                ordered.emplace(p, calcAccountID(p));
            }
        }

        std::vector<STObject> out;
        out.reserve(ordered.size());
        for (auto const& [k, a] : ordered)
        {
            out.emplace_back(objType);
            out.back().setFieldVL(sfPublicKey, k);
            out.back().setAccountID(sfAccount, a);
        }

        return out;
    };

    bool const hasAV = ctx_.tx.isFieldPresent(sfActiveValidator);
    bool const hasVL = ctx_.tx.isFieldPresent(sfImportVLKey);

    if (hasAV)
    {
        auto entries =
            canonicalize(sfActiveValidators, sfActiveValidator);
        STArray arr(sfActiveValidators);
        for (auto& obj : entries)
            arr.push_back(std::move(obj));
        sle->setFieldArray(sfActiveValidators, arr);
    }

    if (hasVL)
    {
        auto entries =
            canonicalize(sfImportVLKeys, sfImportVLKey);
        STArray arr(sfImportVLKeys);
        for (auto& obj : entries)
            arr.push_back(std::move(obj));
        sle->setFieldArray(sfImportVLKeys, arr);
    }

    if (created)
        view().insert(sle);
    else
        view().update(sle);

    return tesSUCCESS;
}

TER
Change::applyImportCredit()
{
    auto const id = ctx_.tx.getAccountID(sfDestination);
    STAmount const amount = ctx_.tx.getFieldAmount(sfAmount);
    auto const importSequence = ctx_.tx.getFieldU32(sfImportSequence);
    uint256 const sourceTxnID = ctx_.tx.getFieldH256(sfSourceTxnID);

    auto sle = view().peek(keylet::account(id));
    bool const create = !sle;

    // Replay protection: check ImportSequence
    if (sle && sle->getFieldU32(sfImportSequence) >= importSequence)
    {
        JLOG(j_.warn())
            << "ImportCredit: replay detected, seq=" << importSequence;
        return tefINTERNAL;
    }

    // Check for supply overflow
    if (amount.xrp() >
        std::numeric_limits<std::int64_t>::max() - view().header().drops)
    {
        JLOG(j_.warn()) << "ImportCredit: supply overflow";
        return tecINTERNAL;
    }

    if (create)
    {
        std::uint32_t const seqno{view().seq()};

        sle = std::make_shared<SLE>(keylet::account(id));
        sle->setAccountID(sfAccount, id);
        sle->setFieldU32(sfSequence, seqno);
        sle->setFieldU32(sfOwnerCount, 0);

        // Master key enabled (derived from inner tx's signing key)

        // If InvoiceID is present, set RegularKey from first 20 bytes
        if (ctx_.tx.isFieldPresent(sfInvoiceID))
        {
            uint256 const invoiceID = ctx_.tx.getFieldH256(sfInvoiceID);
            AccountID regularKeyID;
            std::memcpy(regularKeyID.data(), invoiceID.data(), 20);
            sle->setAccountID(sfRegularKey, regularKeyID);
        }

        sle->setFieldAmount(sfBalance, amount);
        sle->setFieldU32(sfImportSequence, importSequence);
        view().insert(sle);

        // Mint XRP
        ctx_.rawView().rawDestroyXRP(-amount.xrp());
    }
    else
    {
        STAmount const startBal = sle->getFieldAmount(sfBalance);
        STAmount const finalBal = startBal + amount;

        sle->setFieldU32(sfImportSequence, importSequence);
        sle->setFieldAmount(sfBalance, finalBal);
        view().update(sle);

        // Mint XRP
        ctx_.rawView().rawDestroyXRP(-amount.xrp());
    }

    // Create ImportRecord for audit trail
    {
        auto const importKeylet = keylet::importRecord(id, importSequence);
        auto sleImport = std::make_shared<SLE>(importKeylet);

        sleImport->setAccountID(sfAccount, id);
        sleImport->setFieldAmount(sfAmount, amount);
        sleImport->setFieldU32(sfImportSequence, importSequence);
        sleImport->setFieldH256(sfSourceTxnID, sourceTxnID);
        sleImport->setFieldU32(sfLedgerSequence, ctx_.view().seq());
        sleImport->setFieldH256(
            sfPreviousTxnID, ctx_.tx.getTransactionID());
        sleImport->setFieldU32(sfPreviousTxnLgrSeq, ctx_.view().seq());

        auto const page = view().dirInsert(
            keylet::importDir(),
            importKeylet,
            [](std::shared_ptr<SLE> const&) {});

        if (!page)
            return tecDIR_FULL;

        sleImport->setFieldU64(sfImportDirNode, *page);
        view().insert(sleImport);
    }

    return tesSUCCESS;
}

TER
Change::applyExportConfirm()
{
    auto const vaultKeylet = keylet::exportVaultState();
    auto sleVault = view().peek(vaultKeylet);
    if (!sleVault)
    {
        JLOG(j_.warn()) << "ExportConfirm: No ExportVaultState";
        return tefINTERNAL;
    }

    // Update sfMaxTicketSeq if present — only advance, never go backwards
    if (ctx_.tx.isFieldPresent(sfMaxTicketSeq))
    {
        auto const newMax = ctx_.tx.getFieldU32(sfMaxTicketSeq);
        auto const currentMax = sleVault->getFieldU32(sfMaxTicketSeq);

        if (newMax > currentMax)
        {
            sleVault->setFieldU32(sfMaxTicketSeq, newMax);
            JLOG(j_.info())
                << "ExportConfirm: MaxTicketSeq " << currentMax
                << " -> " << newMax;
        }
    }

    // Update sfMainnetSequence if present
    if (ctx_.tx.isFieldPresent(sfMainnetSequence))
    {
        auto const newSeq = ctx_.tx.getFieldU32(sfMainnetSequence);
        sleVault->setFieldU32(sfMainnetSequence, newSeq);
    }

    sleVault->setFieldH256(sfPreviousTxnID, ctx_.tx.getTransactionID());
    sleVault->setFieldU32(sfPreviousTxnLgrSeq, ctx_.view().seq());
    view().update(sleVault);

    return tesSUCCESS;
}

}  // namespace xrpl
