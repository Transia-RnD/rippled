#include <xrpld/app/misc/ExportSignatureCollector.h>

#include <xrpld/app/main/Application.h>
#include <xrpld/app/misc/ValidatorList.h>

#include <xrpl/basics/Slice.h>
#include <xrpl/basics/StringUtilities.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/STArray.h>
#include <xrpl/protocol/STObject.h>
#include <xrpl/protocol/Sign.h>
#include <xrpl/protocol/digest.h>
#include <xrpl/protocol/jss.h>

#include <algorithm>

namespace xrpl {

ExportSignatureCollector::ExportSignatureCollector(
    Application& app,
    beast::Journal journal)
    : journal_(journal)
{
}

bool
ExportSignatureCollector::verifySig(
    TxnData const& data,
    AccountID const& signerAccountID,
    PublicKey const& validatorKey,
    Slice const& sig) const
{
    // Identity binding: the signer AccountID must match the
    // validator's public key.
    if (calcAccountID(validatorKey) != signerAccountID)
    {
        JLOG(journal_.warn())
            << "ExportSigCollector: Identity mismatch for "
            << signerAccountID;
        return false;
    }

    // Verify the multisig signature against the expected hash
    auto const expectedHash =
        exportPaymentMultiSignHash(data.unsignedPayment, signerAccountID);

    if (!verifyDigest(validatorKey, expectedHash, sig, false))
    {
        JLOG(journal_.warn())
            << "ExportSigCollector: Invalid signature from "
            << strHex(validatorKey);
        return false;
    }

    return true;
}

void
ExportSignatureCollector::onExportSignatureFromValidation(
    uint256 const& txnHash,
    Slice const& signerSlice,
    PublicKey const& validatorKey)
{
    // Parse the sfSigner STObject from the slice
    SerialIter sit(signerSlice);
    STObject signerObj(sit, sfSigner);

    if (!signerObj.isFieldPresent(sfAccount) ||
        !signerObj.isFieldPresent(sfSigningPubKey) ||
        !signerObj.isFieldPresent(sfTxnSignature))
    {
        JLOG(journal_.warn())
            << "ExportSigCollector: Malformed signer object";
        return;
    }

    auto const signerAccountID = signerObj.getAccountID(sfAccount);

    // Identity check: validator key must produce this signer AccountID
    if (calcAccountID(validatorKey) != signerAccountID)
    {
        JLOG(journal_.warn())
            << "ExportSigCollector: Identity mismatch, validator "
            << strHex(validatorKey) << " != signer " << signerAccountID;
        return;
    }

    std::lock_guard lock(mutex_);

    // Already assembled?
    if (assembled_.count(txnHash))
        return;

    // Already have a verified signature from this signer?
    if (auto it = signatures_.find(txnHash); it != signatures_.end())
    {
        if (it->second.count(signerAccountID))
            return;
    }

    // Do we have txnData cached? If so, verify immediately (phase 2)
    if (auto dit = txnData_.find(txnHash); dit != txnData_.end())
    {
        auto const sigSlice = signerObj.getFieldVL(sfTxnSignature);
        if (!verifySig(
                dit->second,
                signerAccountID,
                validatorKey,
                makeSlice(sigSlice)))
        {
            return;  // bad signature, drop it
        }

        // Store verified signature
        signatures_[txnHash].emplace(signerAccountID, std::move(signerObj));

        JLOG(journal_.info())
            << "ExportSigCollector: Verified sig for " << txnHash
            << " from " << signerAccountID << " ("
            << signatures_[txnHash].size() << "/"
            << dit->second.quorum << ")";

        // Check quorum
        if (signatures_[txnHash].size() >= dit->second.quorum)
            tryAssemble(txnHash);
    }
    else
    {
        // Phase 1: No txnData yet. Store unverified.
        auto const sigSlice = signerObj.getFieldVL(sfTxnSignature);
        unverified_[txnHash].push_back(
            {validatorKey, Buffer(makeSlice(sigSlice))});

        JLOG(journal_.trace())
            << "ExportSigCollector: Stored unverified sig for "
            << txnHash << " from " << signerAccountID;
    }
}

void
ExportSignatureCollector::stashTxnData(
    uint256 const& txnHash,
    AccountID const& account,
    std::uint32_t exportSeq,
    ExportPaymentParams params,
    std::uint32_t quorum,
    std::uint32_t ledgerSeq)
{
    std::lock_guard lock(mutex_);

    // Don't re-stash
    if (txnData_.count(txnHash))
        return;

    auto payment = buildExportPayment(params);

    txnData_.emplace(
        txnHash,
        TxnData{
            account,
            exportSeq,
            std::move(params),
            std::move(payment),
            quorum,
            ledgerSeq});

    // Set up reverse lookup
    exportKeyLookup_[{account, exportSeq}] = txnHash;

    JLOG(journal_.info())
        << "ExportSigCollector: Stashed txnData for " << txnHash
        << " (" << account << ":" << exportSeq
        << " quorum=" << quorum << ")";

    // Phase 2: Retroactively verify all pending unverified signatures
    auto uit = unverified_.find(txnHash);
    if (uit != unverified_.end())
    {
        auto const& data = txnData_.at(txnHash);
        for (auto const& unv : uit->second)
        {
            auto const signerAccountID = calcAccountID(unv.validatorKey);

            // Skip if already have a verified sig from this signer
            if (signatures_[txnHash].count(signerAccountID))
                continue;

            if (verifySig(
                    data,
                    signerAccountID,
                    unv.validatorKey,
                    Slice(unv.signature.data(), unv.signature.size())))
            {
                // Build sfSigner object for the verified sig
                STObject signerObj = STObject::makeInnerObject(sfSigner);
                signerObj.setAccountID(sfAccount, signerAccountID);
                signerObj.setFieldVL(
                    sfSigningPubKey, unv.validatorKey.slice());
                signerObj.setFieldVL(
                    sfTxnSignature,
                    Slice(unv.signature.data(), unv.signature.size()));

                signatures_[txnHash].emplace(
                    signerAccountID, std::move(signerObj));

                JLOG(journal_.info())
                    << "ExportSigCollector: Retroactively verified "
                    << signerAccountID << " for " << txnHash;
            }
            else
            {
                JLOG(journal_.debug())
                    << "ExportSigCollector: Retroactive verify failed "
                    << signerAccountID << " for " << txnHash;
            }
        }

        // Clear unverified since we've processed them all
        unverified_.erase(uit);
    }

    // Check quorum after retroactive verification
    auto const& data = txnData_.at(txnHash);
    if (signatures_[txnHash].size() >= data.quorum)
        tryAssemble(txnHash);
}

bool
ExportSignatureCollector::hasQuorum(uint256 const& txnHash) const
{
    std::lock_guard lock(mutex_);
    return assembled_.count(txnHash) > 0;
}

void
ExportSignatureCollector::tryAssemble(uint256 const& txnHash)
{
    // Already assembled?
    if (assembled_.count(txnHash))
        return;

    auto dit = txnData_.find(txnHash);
    auto sit = signatures_.find(txnHash);
    if (dit == txnData_.end() || sit == signatures_.end())
        return;

    auto const& data = dit->second;
    if (sit->second.size() < data.quorum)
        return;

    // Build the Signers array sorted by AccountID (ascending)
    STArray signers(sfSigners);
    for (auto const& [acctID, signerObj] : sit->second)
        signers.push_back(signerObj);

    // Clone the unsigned payment and attach the Signers array
    STObject obj(data.unsignedPayment);
    obj.setFieldArray(sfSigners, signers);

    assembled_.emplace(txnHash, STTx(std::move(obj)));

    JLOG(journal_.info())
        << "ExportSigCollector: Assembled multisig for " << txnHash
        << " with " << sit->second.size() << " sigs";
}

Json::Value
ExportSignatureCollector::getExportStatus(
    AccountID const& account,
    std::uint32_t exportSeq) const
{
    std::lock_guard lock(mutex_);

    Json::Value result(Json::objectValue);

    auto lit = exportKeyLookup_.find({account, exportSeq});
    if (lit == exportKeyLookup_.end())
    {
        result[jss::error] = "exportNotFound";
        return result;
    }

    auto const& txnHash = lit->second;

    result[jss::account] = toBase58(account);
    result[jss::export_sequence] = exportSeq;
    result["txn_hash"] = to_string(txnHash);

    if (auto dit = txnData_.find(txnHash); dit != txnData_.end())
    {
        auto const& data = dit->second;
        result[jss::destination] = toBase58(data.params.destination);
        result[jss::amount] =
            data.params.amount.getJson(JsonOptions::none);
        result[jss::ticket_seq] = data.params.ticketSeq;
        result["signatures_required"] = data.quorum;
        result[jss::ledger_index] = data.ledgerSequence;
    }

    if (auto sit = signatures_.find(txnHash); sit != signatures_.end())
        result["signatures_collected"] =
            static_cast<Json::UInt>(sit->second.size());
    else
        result["signatures_collected"] = 0u;

    // Count unverified waiting signatures
    if (auto uit = unverified_.find(txnHash); uit != unverified_.end())
        result["signatures_unverified"] =
            static_cast<Json::UInt>(uit->second.size());

    result["quorum_reached"] = assembled_.count(txnHash) > 0;

    return result;
}

std::optional<STTx>
ExportSignatureCollector::getExportPayment(
    AccountID const& account,
    std::uint32_t exportSeq) const
{
    std::lock_guard lock(mutex_);

    auto lit = exportKeyLookup_.find({account, exportSeq});
    if (lit == exportKeyLookup_.end())
        return std::nullopt;

    auto ait = assembled_.find(lit->second);
    if (ait == assembled_.end())
        return std::nullopt;

    return ait->second;
}

void
ExportSignatureCollector::pruneStale(
    std::uint32_t currentLedger,
    std::uint32_t maxAge)
{
    std::lock_guard lock(mutex_);

    // Collect txnHashes to prune based on ledger age
    std::vector<uint256> toErase;
    for (auto const& [hash, data] : txnData_)
    {
        if (currentLedger > data.ledgerSequence + maxAge)
            toErase.push_back(hash);
    }

    for (auto const& hash : toErase)
    {
        // Remove reverse lookup
        auto dit = txnData_.find(hash);
        if (dit != txnData_.end())
        {
            exportKeyLookup_.erase(
                {dit->second.exportAccount, dit->second.exportSequence});
        }

        txnData_.erase(hash);
        signatures_.erase(hash);
        unverified_.erase(hash);
        assembled_.erase(hash);

        JLOG(journal_.debug())
            << "ExportSigCollector: Pruned stale " << hash;
    }

    // Also prune unverified entries that have no txnData
    // (signatures for exports we never learned about)
    std::vector<uint256> orphanedUnverified;
    for (auto const& [hash, _] : unverified_)
    {
        if (!txnData_.count(hash) &&
            !signatures_.count(hash))
        {
            orphanedUnverified.push_back(hash);
        }
    }
    for (auto const& hash : orphanedUnverified)
        unverified_.erase(hash);
}

}  // namespace xrpl
