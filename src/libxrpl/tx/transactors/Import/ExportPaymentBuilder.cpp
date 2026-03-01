#include <xrpl/tx/transactors/Import/ExportPaymentBuilder.h>

#include <xrpl/basics/Blob.h>
#include <xrpl/protocol/HashPrefix.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STAmount.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/Sign.h>
#include <xrpl/protocol/TxFormats.h>

namespace xrpl {

STTx
buildExportPayment(ExportPaymentParams const& params)
{
    // Construct the exact same Payment on every validator node.
    // Field order and values must be 100% deterministic.

    SOTemplate const& paymentFormat =
        TxFormats::getInstance().findByType(ttPAYMENT)->getSOTemplate();

    STObject obj(paymentFormat, sfTransaction);

    // TransactionType = Payment
    obj.setFieldU16(sfTransactionType, ttPAYMENT);

    // Flags = tfFullyCanonicalSig (required for all transactions)
    obj.setFieldU32(sfFlags, tfFullyCanonicalSig);

    // Account = vault address (the sender on mainnet)
    obj.setAccountID(sfAccount, params.vaultAddress);

    // Destination = the export recipient on mainnet
    obj.setAccountID(sfDestination, params.destination);

    // Amount = the XRP being released
    obj.setFieldAmount(sfAmount, params.amount);

    // Sequence = 0 (required when using TicketSequence)
    obj.setFieldU32(sfSequence, 0);

    // TicketSequence = the assigned mainnet ticket
    obj.setFieldU32(sfTicketSequence, params.ticketSeq);

    // Fee = (signerCount + 1) * 15 drops
    // Multisig fee on mainnet is (1 + numSigners) * baseFee.
    // Use 15 drops as a safe base fee estimate.
    auto const fee =
        STAmount(static_cast<std::uint64_t>(params.signerCount + 1) * 15);
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
    SOTemplate const& format =
        TxFormats::getInstance()
            .findByType(ttSIGNER_LIST_SET)
            ->getSOTemplate();

    STObject obj(format, sfTransaction);

    obj.setFieldU16(sfTransactionType, ttSIGNER_LIST_SET);
    obj.setFieldU32(sfFlags, tfFullyCanonicalSig);
    obj.setAccountID(sfAccount, params.vaultAddress);
    obj.setFieldU32(sfSequence, 0);
    obj.setFieldU32(sfTicketSequence, params.ticketSeq);
    obj.setFieldU32(sfSignerQuorum, params.quorum);

    auto const fee =
        STAmount(static_cast<std::uint64_t>(params.signerCount + 1) * 15);
    obj.setFieldAmount(sfFee, fee);
    obj.setFieldVL(sfSigningPubKey, Blob{});

    // Build the SignerEntries array (sorted by Account)
    STArray entries(sfSignerEntries);
    for (auto const& acctID : params.signerAccounts)
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
    SOTemplate const& format =
        TxFormats::getInstance()
            .findByType(ttTICKET_CREATE)
            ->getSOTemplate();

    STObject obj(format, sfTransaction);

    obj.setFieldU16(sfTransactionType, ttTICKET_CREATE);
    obj.setFieldU32(sfFlags, tfFullyCanonicalSig);
    obj.setAccountID(sfAccount, params.vaultAddress);
    obj.setFieldU32(sfSequence, 0);
    obj.setFieldU32(sfTicketSequence, params.ticketSeq);
    obj.setFieldU32(sfTicketCount, params.ticketCount);

    auto const fee =
        STAmount(static_cast<std::uint64_t>(params.signerCount + 1) * 15);
    obj.setFieldAmount(sfFee, fee);
    obj.setFieldVL(sfSigningPubKey, Blob{});

    return STTx(std::move(obj));
}

}  // namespace xrpl
