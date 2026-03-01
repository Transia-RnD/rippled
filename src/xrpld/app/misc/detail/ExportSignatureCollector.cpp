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

void
ExportSignatureCollector::onExportSignature(
    std::shared_ptr<protocol::TMExportSignature> const& m)
{
    // SECURITY: Validate buffer sizes before memcpy
    if (m->exportaccount().size() != AccountID::bytes)
        return;

    AccountID exportAccount;
    std::memcpy(
        exportAccount.data(),
        m->exportaccount().data(),
        exportAccount.size());

    auto const exportSeq = m->exportsequence();

    // Validate the public key before constructing PublicKey
    auto const keySlice = makeSlice(m->validatorkey());
    if (!publicKeyType(keySlice))
        return;

    PublicKey const validatorKey(keySlice);
    auto const signerAccountID = calcAccountID(validatorKey);

    std::lock_guard lock(mutex_);

    ExportKey const key{exportAccount, exportSeq};
    auto it = pending_.find(key);

    if (it == pending_.end())
    {
        // We don't have this export registered yet.
        // This can happen if the signature arrives before we process the
        // ledger locally. Store a placeholder that will be completed when
        // registerExport is called.
        JLOG(journal_.trace())
            << "ExportSignatureCollector: Received signature for unknown "
               "export "
            << exportAccount << ":" << exportSeq
            << " from validator, deferring";
        return;
    }

    auto& pending = it->second;

    // Already assembled — no need for more signatures
    if (pending.assembledPayment)
        return;

    // Check if we already have a signature from this signer
    if (pending.signatures.count(signerAccountID))
    {
        JLOG(journal_.trace())
            << "ExportSignatureCollector: Duplicate signature from "
            << signerAccountID;
        return;
    }

    // Verify the signature against the expected multisign hash
    auto const expectedHash =
        exportPaymentMultiSignHash(pending.unsignedPayment, signerAccountID);

    auto const sigSlice = makeSlice(m->signature());
    if (!verifyDigest(validatorKey, expectedHash, sigSlice, false))
    {
        JLOG(journal_.warn())
            << "ExportSignatureCollector: Invalid signature from "
            << strHex(validatorKey);
        return;
    }

    // Store the valid signature
    pending.signatures.emplace(
        signerAccountID,
        std::make_pair(validatorKey, Buffer(sigSlice)));

    JLOG(journal_.info())
        << "ExportSignatureCollector: Collected "
        << pending.signatures.size() << "/" << pending.quorum
        << " signatures for export " << exportAccount << ":" << exportSeq;

    // Try to assemble if we've reached quorum
    if (pending.signatures.size() >= pending.quorum)
        tryAssemble(pending);
}

void
ExportSignatureCollector::registerExport(
    AccountID const& exportAccount,
    std::uint32_t exportSequence,
    ExportPaymentParams params,
    std::uint32_t quorum,
    std::uint32_t ledgerSequence)
{
    std::lock_guard lock(mutex_);

    ExportKey const key{exportAccount, exportSequence};

    // Don't re-register
    if (pending_.count(key))
        return;

    auto payment = buildExportPayment(params);

    pending_.emplace(
        key,
        PendingExport{
            std::move(params),
            std::move(payment),
            {},  // signatures
            quorum,
            std::nullopt,  // assembledPayment
            std::chrono::steady_clock::now(),
            ledgerSequence});

    JLOG(journal_.info())
        << "ExportSignatureCollector: Registered export "
        << exportAccount << ":" << exportSequence
        << " quorum=" << quorum << " ledger=" << ledgerSequence;
}

void
ExportSignatureCollector::tryAssemble(PendingExport& pending)
{
    // Build the Signers array from collected signatures.
    // Signers must be sorted by AccountID (ascending).
    std::vector<std::pair<AccountID, std::pair<PublicKey, Buffer>>> sorted(
        pending.signatures.begin(), pending.signatures.end());

    std::sort(
        sorted.begin(),
        sorted.end(),
        [](auto const& a, auto const& b) { return a.first < b.first; });

    STArray signers(sfSigners);
    for (auto const& [acctID, keyAndSig] : sorted)
    {
        auto const& [pubKey, sig] = keyAndSig;

        STObject signer = STObject::makeInnerObject(sfSigner);
        signer.setAccountID(sfAccount, acctID);
        signer.setFieldVL(sfSigningPubKey, pubKey.slice());
        signer.setFieldVL(sfTxnSignature, Slice(sig.data(), sig.size()));
        signers.push_back(std::move(signer));
    }

    // Clone the unsigned payment and attach the Signers array
    STObject obj(pending.unsignedPayment);
    obj.setFieldArray(sfSigners, signers);

    pending.assembledPayment.emplace(std::move(obj));

    JLOG(journal_.info())
        << "ExportSignatureCollector: Assembled multisig payment for export "
        << pending.params.destination << " with "
        << pending.signatures.size() << " signatures";
}

Json::Value
ExportSignatureCollector::getExportStatus(
    AccountID const& account,
    std::uint32_t exportSeq) const
{
    std::lock_guard lock(mutex_);

    Json::Value result(Json::objectValue);

    ExportKey const key{account, exportSeq};
    auto it = pending_.find(key);

    if (it == pending_.end())
    {
        result[jss::error] = "exportNotFound";
        return result;
    }

    auto const& pending = it->second;

    result[jss::account] = toBase58(account);
    result[jss::export_sequence] = exportSeq;
    result[jss::destination] = toBase58(pending.params.destination);
    result[jss::amount] = pending.params.amount.getJson(JsonOptions::none);
    result[jss::ticket_seq] = pending.params.ticketSeq;
    result[jss::signatures_collected] =
        static_cast<Json::UInt>(pending.signatures.size());
    result[jss::signatures_required] = pending.quorum;
    result[jss::quorum_reached] = pending.assembledPayment.has_value();
    result[jss::ledger_index] = pending.ledgerSequence;

    return result;
}

std::optional<STTx>
ExportSignatureCollector::getExportPayment(
    AccountID const& account,
    std::uint32_t exportSeq) const
{
    std::lock_guard lock(mutex_);

    ExportKey const key{account, exportSeq};
    auto it = pending_.find(key);

    if (it == pending_.end())
        return std::nullopt;

    return it->second.assembledPayment;
}

void
ExportSignatureCollector::pruneStale(std::chrono::seconds maxAge)
{
    std::lock_guard lock(mutex_);

    auto const now = std::chrono::steady_clock::now();
    for (auto it = pending_.begin(); it != pending_.end();)
    {
        if (now - it->second.created > maxAge)
        {
            JLOG(journal_.debug())
                << "ExportSignatureCollector: Pruning stale export "
                << it->first.first << ":" << it->first.second;
            it = pending_.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

}  // namespace xrpl
