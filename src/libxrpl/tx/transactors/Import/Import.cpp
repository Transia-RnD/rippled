#include <xrpl/tx/transactors/Import/Import.h>
#include <xrpl/tx/transactors/Import/ImportUtils.h>

#include <xrpl/basics/Log.h>
#include <xrpl/basics/StringUtilities.h>
#include <xrpl/basics/base64.h>
#include <xrpl/basics/chrono.h>
#include <xrpl/core/NetworkIDService.h>
#include <xrpl/core/ServiceRegistry.h>
#include <xrpl/json/json_reader.h>
#include <xrpl/json/json_value.h>
#include <xrpl/ledger/View.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/STValidation.h>
#include <xrpl/protocol/digest.h>
#include <xrpl/protocol/jss.h>
#include <xrpl/protocol/st.h>
#include <xrpl/server/Manifest.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <vector>

namespace xrpl {

TxConsequences
Import::makeTxConsequences(PreflightContext const& ctx)
{
    auto calculate = [](PreflightContext const& ctx) -> XRPAmount {
        auto const [inner, meta] =
            import::getInnerTxn(ctx.tx, ctx.j);
        if (!inner || !meta)
            return beast::zero;

        if (!meta->isFieldPresent(sfTransactionResult))
            return beast::zero;

        auto const result = meta->getFieldU8(sfTransactionResult);
        TER const innerTer = TER::fromInt(result);
        if (!isTesSuccess(innerTer))
            return beast::zero;

        // Lock-and-mint: use DeliveredAmount from metadata, or fall
        // back to sfAmount on the inner Payment (standard XRPL nodes
        // omit sfDeliveredAmount for simple XRP-to-XRP payments).
        STAmount const delivered =
            meta->isFieldPresent(sfDeliveredAmount)
            ? meta->getFieldAmount(sfDeliveredAmount)
            : inner->getFieldAmount(sfAmount);
        if (!isXRP(delivered) || delivered <= beast::zero)
            return beast::zero;

        return delivered.xrp();
    };

    return TxConsequences{ctx.tx, calculate(ctx)};
}

NotTEC
Import::preflight(PreflightContext const& ctx)
{
    auto& tx = ctx.tx;

    // Validate sfBlob
    if (!tx.isFieldPresent(sfBlob))
    {
        JLOG(ctx.j.warn()) << "Import: sfBlob was missing "
                           << tx.getTransactionID();
        return temMALFORMED;
    }

    if (tx.getFieldVL(sfBlob).size() > import::kMaxXPopBlobSize)
    {
        JLOG(ctx.j.warn()) << "Import: blob was more than 512kib "
                           << tx.getTransactionID();
        return temMALFORMED;
    }

    // Parse and syntax-check the XPop
    auto const xpop = import::syntaxCheckXPOP(tx.getFieldVL(sfBlob), ctx.j);
    if (!xpop)
        return temMALFORMED;

    // Extract and validate VL master key
    std::optional<PublicKey> masterVLKey;
    {
        std::string strPk =
            (*xpop)["validation"][jss::unl][jss::public_key].asString();
        auto pkHex = strUnHex(strPk);
        if (!pkHex)
        {
            JLOG(ctx.j.warn())
                << "Import: validation.unl.public_key was not valid hex.";
            return temMALFORMED;
        }

        auto const pkType = publicKeyType(makeSlice(*pkHex));
        if (!pkType)
        {
            JLOG(ctx.j.warn()) << "Import: validation.unl.public_key was not a "
                                  "recognised public key type.";
            return temMALFORMED;
        }

        masterVLKey = PublicKey(makeSlice(*pkHex));
    }

    // Extract inner transaction
    auto const [stpTrans, meta] =
        import::getInnerTxn(tx, ctx.j, &(*xpop));

    if (!stpTrans || !meta)
        return temMALFORMED;

    if (stpTrans->isFieldPresent(sfTicketSequence))
    {
        JLOG(ctx.j.warn()) << "Import: cannot use TicketSequence XPOP.";
        return temMALFORMED;
    }

    // Reject pseudo/emitted transactions
    if (isPseudoTx(*stpTrans) || stpTrans->isFieldPresent(sfEmitDetails))
    {
        JLOG(ctx.j.warn()) << "Import: attempted to import xpop containing an "
                              "emitted or pseudo txn. "
                           << tx.getTransactionID();
        return temMALFORMED;
    }

    // Inner transaction must be a Payment (lock-and-mint model)
    if (stpTrans->getTxnType() != ttPAYMENT)
    {
        JLOG(ctx.j.warn()) << "Import: inner txn is not a Payment "
                           << tx.getTransactionID();
        return temMALFORMED;
    }

    // Ensure the inner txn was tesSUCCESS (Payment must have succeeded
    // for DeliveredAmount to be meaningful)
    if (!meta->isFieldPresent(sfTransactionResult))
    {
        JLOG(ctx.j.warn()) << "Import: inner txn lacked transaction result "
                           << tx.getTransactionID();
        return temMALFORMED;
    }

    {
        uint8_t innerResult = meta->getFieldU8(sfTransactionResult);
        TER const innerTer = TER::fromInt(innerResult);
        if (!isTesSuccess(innerTer))
        {
            JLOG(ctx.j.warn())
                << "Import: inner Payment did not have a tesSUCCESS result "
                << tx.getTransactionID();
            return temMALFORMED;
        }
    }

    // Account must match
    if (stpTrans->getAccountID(sfAccount) != tx.getAccountID(sfAccount))
    {
        JLOG(ctx.j.warn()) << "Import: import and inner xpop account mismatch "
                           << tx.getTransactionID();
        return temMALFORMED;
    }

    // Inner txn must be from network 0 (no sfNetworkID)
    if (stpTrans->isFieldPresent(sfNetworkID))
    {
        JLOG(ctx.j.warn()) << "Import: inner txn has sfNetworkID field "
                           << tx.getTransactionID();
        return temMALFORMED;
    }

    // OperationLimit must be present (value checked in preclaim where
    // network ID is available)
    if (!stpTrans->isFieldPresent(sfOperationLimit))
    {
        JLOG(ctx.j.warn()) << "Import: OperationLimit missing from inner txn "
                           << tx.getTransactionID();
        return temMALFORMED;
    }

    // The outer Import tx does not need to be signed by the inner tx's key.
    // The XPOP (inner tx signature + validator quorum) is the authorization.
    // A relayer can submit with empty SigningPubKey. The inner tx's signing
    // key is used for account derivation on first import.
    // We still verify the inner tx signature below to prove mainnet ownership.
    {
        auto inner = stpTrans->getSigningPubKey();
        if (inner.empty() && !stpTrans->isFieldPresent(sfSigners))
        {
            JLOG(ctx.j.warn())
                << "Import: inner txn has no signing key or signers "
                << tx.getTransactionID();
            return temMALFORMED;
        }
    }

    // Check inner txn signature
    {
        auto const sigVerify = stpTrans->checkSign(ctx.rules);
        if (!sigVerify)
        {
            JLOG(ctx.j.warn())
                << "Import: inner txn signature verify failed: "
                << sigVerify.error() << " " << tx.getTransactionID();
            return temMALFORMED;
        }
    }

    //
    // XPOP verify: manifest, UNL blob, proof, quorum
    //

    // Verify manifest
    auto const m = deserializeManifest(base64_decode(
        (*xpop)["validation"][jss::unl][jss::manifest].asString()));

    if (!m)
    {
        JLOG(ctx.j.warn()) << "Import: failed to deserialize manifest "
                           << tx.getTransactionID();
        return temMALFORMED;
    }

    if (m->masterKey != masterVLKey)
    {
        JLOG(ctx.j.warn()) << "Import: manifest master key mismatch "
                           << tx.getTransactionID();
        return temMALFORMED;
    }

    if (!m->verify())
    {
        JLOG(ctx.j.warn()) << "Import: manifest signature invalid "
                           << tx.getTransactionID();
        return temMALFORMED;
    }

    if (!m->signingKey)
    {
        JLOG(ctx.j.warn()) << "Import: manifest signing key revoked "
                           << tx.getTransactionID();
        return temMALFORMED;
    }

    auto const& signingKey = *m->signingKey;

    // Decode and validate UNL blob
    auto const data = base64_decode(
        (*xpop)["validation"][jss::unl][jss::blob].asString());

    Json::Reader r;
    Json::Value list;
    if (!r.parse(data, list))
    {
        JLOG(ctx.j.warn()) << "Import: unl blob was not valid json "
                           << tx.getTransactionID();
        return temMALFORMED;
    }

    if (!list.isMember(jss::sequence) || !list[jss::sequence].isInt() ||
        !list.isMember(jss::expiration) || !list[jss::expiration].isInt() ||
        !list.isMember(jss::validators) || !list[jss::validators].isArray())
    {
        JLOG(ctx.j.warn()) << "Import: unl blob missing required fields "
                           << tx.getTransactionID();
        return temMALFORMED;
    }

    if (list.isMember(jss::effective) && !list[jss::effective].isInt())
    {
        JLOG(ctx.j.warn()) << "Import: unl blob effective field wrong type "
                           << tx.getTransactionID();
        return temMALFORMED;
    }

    // Check temporal validity of the UNL blob (validFrom vs validUntil only)
    // Note: time-based checks against "now" are done in preclaim where
    // the timekeeper is available.
    auto const validFrom = NetClock::time_point{NetClock::duration{
        list.isMember(jss::effective) ? list[jss::effective].asUInt() : 0}};
    auto const validUntil = NetClock::time_point{
        NetClock::duration{list[jss::expiration].asUInt()}};

    if (validUntil <= validFrom)
    {
        JLOG(ctx.j.warn()) << "Import: unl blob validUntil <= validFrom "
                           << tx.getTransactionID();
        return temMALFORMED;
    }

    // Verify UNL blob signature
    auto const sig = strUnHex(
        (*xpop)["validation"][jss::unl][jss::signature].asString());
    if (!sig || !verify(signingKey, makeSlice(data), makeSlice(*sig)))
    {
        JLOG(ctx.j.warn()) << "Import: unl blob not signed correctly "
                           << tx.getTransactionID();
        return temMALFORMED;
    }

    // Verify transaction proof
    auto const tx_hash = stpTrans->getTransactionID();

    auto rawTx = strUnHex((*xpop)[jss::transaction][jss::blob].asString());
    auto const tx_meta =
        strUnHex((*xpop)[jss::transaction][jss::meta].asString());
    Serializer s(rawTx->size() + tx_meta->size() + 40);
    s.addVL(*rawTx);
    s.addVL(*tx_meta);
    s.addBitString(tx_hash);

    uint256 const computedTxHashAndMeta =
        sha512Half(HashPrefix::txNode, s.slice());

    if (!import::proofContainsHash(
            (*xpop)[jss::transaction][jss::proof],
            strHex(computedTxHashAndMeta)))
    {
        JLOG(ctx.j.warn())
            << "Import: xpop proof did not contain the txn hash "
            << strHex(computedTxHashAndMeta) << " txid: "
            << tx.getTransactionID();
        return temMALFORMED;
    }

    // Compute and verify merkle root
    uint256 const computedTxRoot = import::computeMerkleRoot(
        (*xpop)[jss::transaction][jss::proof]);

    auto const& lgr = (*xpop)[jss::ledger];
    if (strHex(computedTxRoot) != lgr["txroot"])
    {
        JLOG(ctx.j.warn()) << "Import: computed txroot does not match xpop "
                           << tx.getTransactionID();
        return temMALFORMED;
    }

    // Compute ledger hash
    uint256 const computedLedgerHash =
        import::computeLedgerHash(lgr, computedTxRoot);

    if (computedLedgerHash == uint256{})
    {
        JLOG(ctx.j.warn()) << "Import: error computing ledger hash "
                           << tx.getTransactionID();
        return temMALFORMED;
    }

    // If a chain array is present, walk the chain forward to get the
    // final ledger hash that validators signed. This supports recovery
    // when the original ledger's validations were missed.
    uint256 quorumLedgerHash = computedLedgerHash;
    if (xpop->isMember("chain"))
    {
        auto const& chain = (*xpop)["chain"];
        if (chain.isArray() && chain.size() > 0)
        {
            quorumLedgerHash = import::verifyLedgerChain(
                chain, computedLedgerHash, ctx.j);

            if (quorumLedgerHash == uint256{})
            {
                JLOG(ctx.j.warn())
                    << "Import: ledger chain verification failed "
                    << tx.getTransactionID();
                return temMALFORMED;
            }
        }
    }

    // Parse validators and check quorum
    auto const validatorInfo =
        import::parseValidatorList(list[jss::validators], ctx.j);

    auto const validationCount = import::countValidations(
        (*xpop)["validation"][jss::data],
        validatorInfo,
        quorumLedgerHash,
        ctx.j);

    if (!import::hasQuorum(validatorInfo.totalCount, validationCount))
    {
        JLOG(ctx.j.warn()) << "Import: xpop did not contain an 80% quorum "
                           << tx.getTransactionID();
        return temMALFORMED;
    }

    // Final sanity checks
    if (!stpTrans->isFieldPresent(sfSequence))
    {
        JLOG(ctx.j.warn())
            << "Import: xpop inner txn missing sequence "
            << tx.getTransactionID();
        return temMALFORMED;
    }

    // Lock-and-mint: use DeliveredAmount from metadata when present,
    // otherwise fall back to sfAmount on the inner Payment.  Standard
    // XRPL nodes omit sfDeliveredAmount for simple XRP-to-XRP payments.
    {
        STAmount const delivered =
            meta->isFieldPresent(sfDeliveredAmount)
            ? meta->getFieldAmount(sfDeliveredAmount)
            : stpTrans->getFieldAmount(sfAmount);
        if (!isXRP(delivered) || delivered <= beast::zero)
        {
            JLOG(ctx.j.warn())
                << "Import: DeliveredAmount must be positive XRP "
                << tx.getTransactionID();
            return temMALFORMED;
        }
    }

    return tesSUCCESS;
}

TER
Import::preclaim(PreclaimContext const& ctx)
{
    if (!ctx.tx.isFieldPresent(sfBlob))
        return tefINTERNAL;

    auto const xpop =
        import::syntaxCheckXPOP(ctx.tx.getFieldVL(sfBlob), ctx.j);
    if (!xpop)
        return tefINTERNAL;

    auto const [stpTrans, meta] =
        import::getInnerTxn(ctx.tx, ctx.j, &(*xpop));

    if (!stpTrans || !meta || !stpTrans->isFieldPresent(sfSequence))
        return tefINTERNAL;

    // OperationLimit must match our network ID (moved from preflight
    // because network ID requires app/registry access)
    if (stpTrans->isFieldPresent(sfOperationLimit))
    {
        auto const networkID =
            ctx.registry.getNetworkIDService().getNetworkID();
        if (stpTrans->getFieldU32(sfOperationLimit) != networkID)
        {
            JLOG(ctx.j.warn())
                << "Import: Wrong network ID for OperationLimit "
                << ctx.tx.getTransactionID();
            return telWRONG_NETWORK;
        }
    }

    // Check temporal validity of UNL blob against current time
    // (moved from preflight because time checks require view access)
    {
        Json::Value list;
        auto const data = base64_decode(
            (*xpop)["validation"][jss::unl][jss::blob].asString());
        Json::Reader r;
        if (r.parse(data, list) &&
            list.isMember(jss::expiration) && list[jss::expiration].isInt())
        {
            auto const validFrom = NetClock::time_point{NetClock::duration{
                list.isMember(jss::effective)
                    ? list[jss::effective].asUInt()
                    : 0}};
            auto const validUntil = NetClock::time_point{
                NetClock::duration{list[jss::expiration].asUInt()}};
            auto const now = ctx.view.parentCloseTime();

            if (validUntil <= now)
            {
                JLOG(ctx.j.warn()) << "Import: unl blob expired "
                                   << ctx.tx.getTransactionID();
                return tefINTERNAL;
            }

            if (validFrom > now)
            {
                JLOG(ctx.j.warn()) << "Import: unl blob not yet valid "
                                   << ctx.tx.getTransactionID();
                return tefINTERNAL;
            }
        }
    }

    // Lock-and-mint: Verify inner Payment destination is the vault address
    {
        auto const& vaultAddress = ctx.registry.getImportVaultAddress();
        if (!vaultAddress)
        {
            JLOG(ctx.j.warn())
                << "Import: no vault address configured "
                << ctx.tx.getTransactionID();
            return tefINTERNAL;
        }

        if (!stpTrans->isFieldPresent(sfDestination))
        {
            JLOG(ctx.j.warn())
                << "Import: inner Payment has no Destination "
                << ctx.tx.getTransactionID();
            return tefINTERNAL;
        }

        if (stpTrans->getAccountID(sfDestination) != *vaultAddress)
        {
            JLOG(ctx.j.warn())
                << "Import: inner Payment Destination is not the vault "
                << ctx.tx.getTransactionID();
            return tecNO_DST;
        }
    }

    auto const& sle = ctx.view.read(keylet::account(ctx.tx[sfAccount]));

    // Replay protection: check ImportSequence
    if (sle && sle->isFieldPresent(sfImportSequence))
    {
        uint32_t sleImportSequence = sle->getFieldU32(sfImportSequence);
        if (sleImportSequence >= stpTrans->getFieldU32(sfSequence))
            return tefPAST_IMPORT_SEQ;
    }

    // First import must have zero fee (account creation)
    if (!sle && ctx.tx.getFieldAmount(sfFee) != beast::zero)
        return temBAD_FEE;

    // Validate VL key is recognised
    auto const vlInfo = import::getVLInfo(*xpop, ctx.j);
    if (!vlInfo)
        return tefINTERNAL;

    // Check VL sequence not already used
    auto const& sleVL = ctx.view.read(keylet::importVLSeq(vlInfo->second));
    if (sleVL && sleVL->getFieldU32(sfImportSequence) > vlInfo->first)
        return tefPAST_IMPORT_VL_SEQ;

    // Check master VL key is recognised
    std::string strPk =
        (*xpop)["validation"][jss::unl][jss::public_key].asString();

    if (ctx.registry.isImportVLKeyRecognized(strPk))
        return tesSUCCESS;

    auto pkHex = strUnHex(strPk);
    if (!pkHex)
        return tefINTERNAL;

    auto const pkType = publicKeyType(makeSlice(*pkHex));
    if (!pkType)
        return tefINTERNAL;

    JLOG(ctx.j.warn()) << "Import: import vl key not recognized, bailing.";
    return telIMPORT_VL_KEY_NOT_RECOGNISED;
}

TER
Import::doApply()
{
    if (!ctx_.tx.isFieldPresent(sfBlob))
        return tefINTERNAL;

    auto const xpop =
        import::syntaxCheckXPOP(ctx_.tx.getFieldVL(sfBlob), ctx_.journal);
    if (!xpop)
        return tefINTERNAL;

    // Update VL sequence tracking
    auto const infoVL = import::getVLInfo(*xpop, ctx_.journal);
    if (!infoVL)
        return tefINTERNAL;

    auto const keyletVL = keylet::importVLSeq(infoVL->second);
    auto sleVL = view().peek(keyletVL);

    if (!sleVL)
    {
        sleVL = std::make_shared<SLE>(keyletVL);
        sleVL->setFieldU32(sfImportSequence, infoVL->first);
        sleVL->setFieldVL(sfPublicKey, infoVL->second.slice());
        view().insert(sleVL);
    }
    else
    {
        uint32_t current = sleVL->getFieldU32(sfImportSequence);
        if (current > infoVL->first)
            return tefINTERNAL;
        if (infoVL->first > current)
        {
            sleVL->setFieldU32(sfImportSequence, infoVL->first);
            view().update(sleVL);
        }
    }

    // Extract inner transaction
    auto const [stpTrans, meta] =
        import::getInnerTxn(ctx_.tx, ctx_.journal, &(*xpop));

    if (!stpTrans || !stpTrans->isFieldPresent(sfSequence) || !meta ||
        !meta->isFieldPresent(sfTransactionResult))
    {
        return tefINTERNAL;
    }

    // Lock-and-mint: use DeliveredAmount from metadata when present,
    // otherwise fall back to sfAmount on the inner Payment.
    STAmount const delivered =
        meta->isFieldPresent(sfDeliveredAmount)
        ? meta->getFieldAmount(sfDeliveredAmount)
        : stpTrans->getFieldAmount(sfAmount);
    if (!isXRP(delivered) || delivered <= beast::zero)
        return tefINTERNAL;

    // Check for overflow (safe integer check, no UB)
    if (delivered.xrp() >
        std::numeric_limits<std::int64_t>::max() - view().header().drops)
    {
        JLOG(ctx_.journal.warn()) << "Import: ledger header overflow";
        return tecINTERNAL;
    }

    uint32_t importSequence = stpTrans->getFieldU32(sfSequence);
    auto const id = ctx_.tx[sfAccount];
    auto sle = view().peek(keylet::account(id));

    if (sle && sle->getFieldU32(sfImportSequence) >= importSequence)
        return tefINTERNAL;

    bool const create = !sle;

    // Lock-and-mint: 1:1 ratio — mint equals delivered amount
    uint64_t creditDrops = delivered.xrp().drops();

    XRPAmount const bonusAmount = Import::computeStartingBonus(ctx_.view());
    STAmount startBal =
        create ? STAmount(bonusAmount) : STAmount(mSourceBalance);

    STAmount finalBal = startBal + STAmount(XRPAmount(creditDrops));

    if (finalBal < startBal)
    {
        JLOG(ctx_.journal.warn()) << "Import: overflow finalBal < startBal.";
        return tefINTERNAL;
    }

    if (create)
    {
        std::uint32_t const seqno{view().seq()};

        sle = std::make_shared<SLE>(keylet::account(id));
        sle->setAccountID(sfAccount, id);
        sle->setFieldU32(sfSequence, seqno);
        sle->setFieldU32(sfOwnerCount, 0);

        // Master key is derived from the inner tx's signing key (which
        // determines the AccountID). Leave master ENABLED — the user
        // controls this account via their mainnet key.
        // No lsfDisableMaster flag set.

        // If the inner Payment has InvoiceID, use the first 20 bytes as
        // an AccountID to set as the RegularKey. This allows users to
        // specify a passkey account for the sidechain via the mainnet
        // Payment's InvoiceID field.
        if (stpTrans->isFieldPresent(sfInvoiceID))
        {
            uint256 const invoiceID = stpTrans->getFieldH256(sfInvoiceID);
            AccountID regularKeyID;
            std::memcpy(regularKeyID.data(), invoiceID.data(), 20);
            sle->setAccountID(sfRegularKey, regularKeyID);
        }
    }

    sle->setFieldU32(sfImportSequence, importSequence);
    sle->setFieldAmount(sfBalance, finalBal);

    if (create)
        view().insert(sle);
    else
        view().update(sle);

    // Mint XRP by adjusting the ledger header
    STAmount added = create ? finalBal : finalBal - startBal;
    ctx_.rawView().rawDestroyXRP(-added.xrp());

    // Create ImportRecord for audit trail (double-entry bookkeeping)
    {
        auto const importKeylet =
            keylet::importRecord(id, importSequence);
        auto sleImport = std::make_shared<SLE>(importKeylet);

        sleImport->setAccountID(sfAccount, id);
        sleImport->setFieldAmount(sfAmount, added);
        sleImport->setFieldU32(sfImportSequence, importSequence);
        sleImport->setFieldH256(
            sfSourceTxnID, stpTrans->getTransactionID());
        sleImport->setFieldU32(sfLedgerSequence, ctx_.view().seq());
        sleImport->setFieldH256(
            sfPreviousTxnID, ctx_.tx.getTransactionID());
        sleImport->setFieldU32(sfPreviousTxnLgrSeq, ctx_.view().seq());

        // Add to global import directory (not owner directory — no reserve)
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

XRPAmount
Import::calculateBaseFee(ReadView const& view, STTx const& tx)
{
    // Zero fee for new account imports
    if (!view.exists(keylet::account(tx.getAccountID(sfAccount))))
        return XRPAmount{0};

    return Transactor::calculateBaseFee(view, tx);
}

}  // namespace xrpl
