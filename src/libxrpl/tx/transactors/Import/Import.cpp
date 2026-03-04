#include <xrpl/tx/transactors/Import/Import.h>
#include <xrpl/tx/transactors/Import/ImportUtils.h>
#include <xrpl/tx/transactors/SetSignerList.h>
#include <xrpl/tx/SignerEntries.h>

#include <xrpl/basics/Log.h>
#include <xrpl/basics/StringUtilities.h>
#include <xrpl/basics/base64.h>
#include <xrpl/basics/chrono.h>
#include <xrpl/core/NetworkIDService.h>
#include <xrpl/core/ServiceRegistry.h>
#include <xrpl/json/json_reader.h>
#include <xrpl/json/json_value.h>
#include <xrpl/ledger/Sandbox.h>
#include <xrpl/ledger/View.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/STValidation.h>
#include <xrpl/protocol/TxFormats.h>
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

        // Non-Payment imports don't mint XRP
        if (inner->getTxnType() != ttPAYMENT)
            return beast::zero;

        // IOU imports don't have direct XRP consequences; XRP is
        // auto-minted as a side effect in doApply.
        return beast::zero;
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

    // Inner transaction must be a supported type
    auto const innerType = stpTrans->getTxnType();
    if (innerType != ttPAYMENT &&
        innerType != ttREGULAR_KEY_SET &&
        innerType != ttSIGNER_LIST_SET)
    {
        JLOG(ctx.j.warn()) << "Import: unsupported inner txn type "
                           << tx.getTransactionID();
        return temMALFORMED;
    }

    // Ensure the inner txn was tesSUCCESS
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
                << "Import: inner txn did not have a tesSUCCESS result "
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

    // IOU-only import: DeliveredAmount must be a positive IOU (not XRP).
    // XRP imports are blocked to prevent accounting mismatch — sidechain
    // XRP is freely minted as a gas token.
    if (innerType == ttPAYMENT)
    {
        STAmount const delivered =
            meta->isFieldPresent(sfDeliveredAmount)
            ? meta->getFieldAmount(sfDeliveredAmount)
            : stpTrans->getFieldAmount(sfAmount);
        if (isXRP(delivered) || delivered <= beast::zero)
        {
            JLOG(ctx.j.warn())
                << "Import: DeliveredAmount must be a positive IOU "
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
    if (stpTrans->getTxnType() == ttPAYMENT)
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

    // Non-Payment imports require an existing account (no XRP is minted)
    if (stpTrans->getTxnType() != ttPAYMENT && !sle)
    {
        JLOG(ctx.j.warn())
            << "Import: non-Payment import requires existing account "
            << ctx.tx.getTransactionID();
        return tecNO_DST;
    }

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

    auto const innerType = stpTrans->getTxnType();
    uint32_t importSequence = stpTrans->getFieldU32(sfSequence);
    auto const id = ctx_.tx[sfAccount];
    auto sle = view().peek(keylet::account(id));

    // Replay protection
    if (sle && sle->isFieldPresent(sfImportSequence) &&
        sle->getFieldU32(sfImportSequence) >= importSequence)
        return tefINTERNAL;

    if (innerType == ttPAYMENT)
    {
        // ── Payment Import: IOU credit + auto XRP mint ────────

        STAmount const delivered =
            meta->isFieldPresent(sfDeliveredAmount)
            ? meta->getFieldAmount(sfDeliveredAmount)
            : stpTrans->getFieldAmount(sfAmount);
        if (isXRP(delivered) || delivered <= beast::zero)
            return tefINTERNAL;

        // Get vault address (issuer on sidechain)
        auto const vaultOpt = ctx_.registry.getImportVaultAddress();
        if (!vaultOpt)
            return tefINTERNAL;
        auto const& vaultAddr = *vaultOpt;

        // Map to sidechain representation: same currency, vault as issuer
        Issue const sidechainIssue(delivered.getCurrency(), vaultAddr);
        STAmount const creditAmount(
            sidechainIssue, delivered.mantissa(), delivered.exponent());

        // Auto-mint XRP threshold (default 50 XRP)
        auto const mintThreshold =
            ctx_.registry.getImportXrpMintAmount()
                .value_or(XRPAmount{50'000'000});

        bool const create = !sle;

        // ── Account creation / XRP top-up ──────────────────────
        if (create)
        {
            std::uint32_t const seqno{view().seq()};

            sle = std::make_shared<SLE>(keylet::account(id));
            sle->setAccountID(sfAccount, id);
            sle->setFieldU32(sfSequence, seqno);
            sle->setFieldU32(sfOwnerCount, 0);
            sle->setFieldAmount(sfBalance, STAmount{mintThreshold});
            sle->setFieldU32(sfImportSequence, importSequence);
            view().insert(sle);

            // Mint the XRP
            ctx_.rawView().rawDestroyXRP(-mintThreshold);
        }
        else
        {
            // Existing account: top up XRP if below threshold
            STAmount const currentBal{mSourceBalance};
            if (currentBal.xrp() < mintThreshold)
            {
                auto const toMint = mintThreshold - currentBal.xrp();

                // Overflow check
                if (toMint >
                    std::numeric_limits<std::int64_t>::max() -
                        view().header().drops)
                {
                    JLOG(ctx_.journal.warn())
                        << "Import: ledger header overflow on XRP mint";
                    return tecINTERNAL;
                }

                sle->setFieldAmount(
                    sfBalance, STAmount{currentBal.xrp() + toMint});
                ctx_.rawView().rawDestroyXRP(-toMint);
            }

            sle->setFieldU32(sfImportSequence, importSequence);
            view().update(sle);
        }

        // ── Credit IOU via issueIOU (auto-creates trust line) ──
        {
            auto const ter = issueIOU(
                view(), id, creditAmount, sidechainIssue, ctx_.journal);
            if (!isTesSuccess(ter))
                return ter;
        }

        // Create ImportRecord for audit trail
        {
            auto const importKeylet =
                keylet::importRecord(id, importSequence);
            auto sleImport = std::make_shared<SLE>(importKeylet);

            sleImport->setAccountID(sfAccount, id);
            sleImport->setFieldAmount(sfAmount, creditAmount);
            sleImport->setFieldU32(sfImportSequence, importSequence);
            sleImport->setFieldH256(
                sfSourceTxnID, stpTrans->getTransactionID());
            sleImport->setFieldU32(
                sfLedgerSequence, ctx_.view().seq());
            sleImport->setFieldH256(
                sfPreviousTxnID, ctx_.tx.getTransactionID());
            sleImport->setFieldU32(
                sfPreviousTxnLgrSeq, ctx_.view().seq());

            auto const page = view().dirInsert(
                keylet::importDir(),
                importKeylet,
                [](std::shared_ptr<SLE> const&) {});

            if (!page)
                return tecDIR_FULL;

            sleImport->setFieldU64(sfImportDirNode, *page);

            view().insert(sleImport);
        }
    }
    else
    {
        // ── Key-only Import: SetRegularKey or SignerList ────────
        // No XRP minting, no ImportRecord — just account configuration.

        if (!sle)
            return tefINTERNAL;  // preclaim already checks

        // Only apply key changes if inner tx succeeded
        if (isTesSuccess(
                TER::fromInt(meta->getFieldU8(sfTransactionResult))))
        {
            if (innerType == ttREGULAR_KEY_SET)
                doRegularKey(sle, *stpTrans);
            else if (innerType == ttSIGNER_LIST_SET)
                doSignerList(sle, *stpTrans);
        }

        sle->setFieldU32(sfImportSequence, importSequence);
        view().update(sle);
    }

    return tesSUCCESS;
}

// ── Key Import Helpers ─────────────────────────────────────────

void
Import::doRegularKey(std::shared_ptr<SLE>& sle, STTx const& stpTrans)
{
    AccountID id = stpTrans.getAccountID(sfAccount);

    JLOG(ctx_.journal.trace()) << "Import: doRegularKey acc: " << id;

    if (stpTrans.getFieldU16(sfTransactionType) != ttREGULAR_KEY_SET)
    {
        JLOG(ctx_.journal.warn())
            << "Import: doRegularKey called on non-regular key transaction.";
        return;
    }

    if (!stpTrans.isFieldPresent(sfRegularKey))
    {
        // delete op
        JLOG(ctx_.journal.trace()) << "Import: clearing SetRegularKey "
                                   << " acc: " << id;
        if (sle->isFieldPresent(sfRegularKey))
            sle->makeFieldAbsent(sfRegularKey);
        return;
    }

    AccountID rk = stpTrans.getAccountID(sfRegularKey);
    JLOG(ctx_.journal.trace())
        << "Import: actioning SetRegularKey " << rk << " acc: " << id;
    sle->setAccountID(sfRegularKey, rk);

    // always set this flag if they have done any regular keying
    sle->setFlag(lsfPasswordSpent);

    ctx_.view().update(sle);

    return;
}

void
Import::doSignerList(std::shared_ptr<SLE>& sle, STTx const& stpTrans)
{
    AccountID id = stpTrans.getAccountID(sfAccount);

    JLOG(ctx_.journal.trace()) << "Import: doSignerList acc: " << id;

    if (!stpTrans.isFieldPresent(sfSignerQuorum))
    {
        JLOG(ctx_.journal.warn())
            << "Import: acc " << id
            << " tried to import signerlist without sfSignerQuorum, skipping";
        return;
    }

    Sandbox sb(&view());

    uint32_t quorum = stpTrans.getFieldU32(sfSignerQuorum);

    if (quorum == 0)
    {
        // delete operation
        TER result =
            SetSignerList::removeFromLedger(ctx_.registry, sb, id, ctx_.journal);
        if (isTesSuccess(result))
        {
            JLOG(ctx_.journal.warn())
                << "Import: successful destroy SignerListSet";
            sb.apply(ctx_.rawView());
        }
        else
        {
            JLOG(ctx_.journal.warn())
                << "Import: SetSignerList destroy failed with code " << result
                << " acc: " << id;
        }
        return;
    }

    if (!stpTrans.isFieldPresent(sfSignerEntries) ||
        stpTrans.getFieldArray(sfSignerEntries).empty())
    {
        JLOG(ctx_.journal.warn())
            << "Import: SetSignerList lacked populated array and quorum was "
               "non-zero. Ignoring. acc: "
            << id;
        return;
    }

    // Extract signer entries and sort them
    std::vector<SignerEntries::SignerEntry> signers;
    auto const entries = stpTrans.getFieldArray(sfSignerEntries);
    signers.reserve(entries.size());
    for (auto const& e : entries)
    {
        if (!e.isFieldPresent(sfAccount) || !e.isFieldPresent(sfSignerWeight))
        {
            JLOG(ctx_.journal.warn())
                << "Import: SignerListSet entry lacked a required field "
                   "(Account/SignerWeight). "
                << "Skipping SignerListSet.";
            return;
        }

        std::optional<uint256> tag;
        if (e.isFieldPresent(sfWalletLocator))
            tag = e.getFieldH256(sfWalletLocator);

        signers.emplace_back(
            e.getAccountID(sfAccount), e.getFieldU16(sfSignerWeight), tag);
    }
    std::sort(signers.begin(), signers.end());

    // Validate signer list
    JLOG(ctx_.journal.warn()) << "Import: actioning SignerListSet "
                              << "quorum: " << quorum << " "
                              << "size: " << signers.size();

    if (SetSignerList::validateQuorumAndSignerEntries(
            quorum, signers, id, ctx_.journal, ctx_.view().rules()) !=
        tesSUCCESS)
    {
        JLOG(ctx_.journal.warn())
            << "Import: validation of signer entries failed acc: " << id
            << ". Skipping.";
        return;
    }

    // Install signer list
    TER result = SetSignerList::replaceSignersFromLedger(
        ctx_.registry,
        sb,
        ctx_.journal,
        id,
        quorum,
        signers,
        sle->getFieldAmount(sfBalance).xrp());

    if (isTesSuccess(result))
    {
        JLOG(ctx_.journal.warn()) << "Import: successful set SignerListSet";
        sb.apply(ctx_.rawView());
    }
    else
    {
        JLOG(ctx_.journal.warn())
            << "Import: SetSignerList set failed with code " << result
            << " acc: " << id;
    }
    return;
}

// ── Fee Calculation ────────────────────────────────────────────

XRPAmount
Import::calculateBaseFee(ReadView const& view, STTx const& tx)
{
    // Zero fee for new account imports
    if (!view.exists(keylet::account(tx.getAccountID(sfAccount))))
        return XRPAmount{0};

    return Transactor::calculateBaseFee(view, tx);
}

}  // namespace xrpl
