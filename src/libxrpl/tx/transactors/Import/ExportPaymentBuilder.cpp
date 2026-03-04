#include <xrpl/tx/transactors/Import/ExportPaymentBuilder.h>

#include <xrpl/basics/Blob.h>
#include <xrpl/protocol/HashPrefix.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STAmount.h>
#include <xrpl/protocol/STArray.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/Sign.h>
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/protocol/digest.h>

#include <algorithm>

namespace xrpl {

STTx
buildExportPayment(ExportPaymentParams const& params)
{
    // Construct the exact same Payment on every validator node.
    // Field order and values must be 100% deterministic.
    // Note: Do NOT use STObject(paymentFormat, sfTransaction) here.
    // That pre-populates soeDEFAULT fields (like sfPaths) as nonPresent
    // entries, which STTx::applyTemplate then rejects as "explicitly set
    // to default".  Build a bare STObject and let the STTx constructor
    // apply the template on the fields we actually set.

    STObject obj(sfTransaction);

    // TransactionType = Payment
    obj.setFieldU16(sfTransactionType, ttPAYMENT);

    // Flags = tfFullyCanonicalSig (required for all transactions)
    obj.setFieldU32(sfFlags, tfFullyCanonicalSig);

    // Account = vault address (the sender on mainnet)
    obj.setAccountID(sfAccount, params.vaultAddress);

    // Destination = the export recipient on mainnet
    obj.setAccountID(sfDestination, params.destination);

    // Amount — for IOUs, map sidechain vault issuer → mainnet issuer
    auto amt = params.amount;
    if (!isXRP(amt) && params.mainnetIssuer)
    {
        amt = STAmount(
            Issue(amt.getCurrency(), *params.mainnetIssuer),
            amt.mantissa(),
            amt.exponent());
    }
    obj.setFieldAmount(sfAmount, amt);

    // SendMax for IOU payments (vault sends IOUs it holds)
    if (!isXRP(amt))
        obj.setFieldAmount(sfSendMax, amt);

    // Sequence = 0 (required when using TicketSequence)
    obj.setFieldU32(sfSequence, 0);

    // TicketSequence = the assigned mainnet ticket
    obj.setFieldU32(sfTicketSequence, params.ticketSeq);

    // Fee = (signerCount + 1) * baseFee drops
    // Multisig fee on mainnet is (1 + numSigners) * baseFee.
    // baseFee is configurable via [mainnet_base_fee] (default 15 drops).
    auto const fee = STAmount(
        (static_cast<std::uint64_t>(params.signerCount) + 1) * params.baseFee);
    obj.setFieldAmount(sfFee, fee);

    // Empty SigningPubKey (required for multi-signed transactions)
    obj.setFieldVL(sfSigningPubKey, Blob{});

    // Optional DestinationTag
    if (params.destinationTag)
        obj.setFieldU32(sfDestinationTag, *params.destinationTag);

    return STTx(std::move(obj));
}

uint256
exportPaymentMultiSignHash(STTx const& tx, AccountID const& signerID)
{
    // For XRPL multisig, each signer signs:
    //   SHA512Half(HashPrefix::txMultiSign || txFieldsWithoutSigningFields
    //              || signerAccountID)
    Serializer s = buildMultiSigningData(tx, signerID);
    return sha512Half(s.slice());
}

STTx
buildSignerListSet(SignerListSetParams const& params)
{
    STObject obj(sfTransaction);

    obj.setFieldU16(sfTransactionType, ttSIGNER_LIST_SET);
    obj.setFieldU32(sfFlags, tfFullyCanonicalSig);
    obj.setAccountID(sfAccount, params.vaultAddress);
    obj.setFieldU32(sfSequence, 0);
    obj.setFieldU32(sfTicketSequence, params.ticketSeq);
    obj.setFieldU32(sfSignerQuorum, params.quorum);

    auto const fee = STAmount(
        (static_cast<std::uint64_t>(params.signerCount) + 1) * params.baseFee);
    obj.setFieldAmount(sfFee, fee);
    obj.setFieldVL(sfSigningPubKey, Blob{});

    // Build the SignerEntries array, sorted by Account (required by XRPL)
    // First sort the signer accounts
    auto sortedAccounts = params.signerAccounts;
    std::sort(sortedAccounts.begin(), sortedAccounts.end());
    STArray entries(sfSignerEntries);
    for (auto const& acctID : sortedAccounts)
    {
        STObject entry = STObject::makeInnerObject(sfSignerEntry);
        entry.setAccountID(sfAccount, acctID);
        entry.setFieldU16(sfSignerWeight, 1);
        entries.push_back(std::move(entry));
    }
    obj.setFieldArray(sfSignerEntries, entries);

    return STTx(std::move(obj));
}

STTx
buildTicketCreate(TicketCreateParams const& params)
{
    STObject obj(sfTransaction);

    obj.setFieldU16(sfTransactionType, ttTICKET_CREATE);
    obj.setFieldU32(sfFlags, tfFullyCanonicalSig);
    obj.setAccountID(sfAccount, params.vaultAddress);
    obj.setFieldU32(sfSequence, 0);
    obj.setFieldU32(sfTicketSequence, params.ticketSeq);
    obj.setFieldU32(sfTicketCount, params.ticketCount);

    auto const fee = STAmount(
        (static_cast<std::uint64_t>(params.signerCount) + 1) * params.baseFee);
    obj.setFieldAmount(sfFee, fee);
    obj.setFieldVL(sfSigningPubKey, Blob{});

    return STTx(std::move(obj));
}

}  // namespace xrpl
