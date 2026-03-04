#pragma once

#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/STTx.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace xrpl {

/** Deterministic construction of a mainnet multisig Payment from an
    ExportRecord. Every validator building from the same ExportRecord MUST
    produce identical bytes, which they then individually multisign.
*/
struct ExportPaymentParams
{
    AccountID vaultAddress;      // Mainnet vault account (source of Payment)
    AccountID destination;       // Mainnet destination
    STAmount amount;             // Amount (XRP or IOU)
    std::uint32_t ticketSeq;     // Mainnet ticket for this export
    std::uint32_t signerCount;   // Number of signers (for fee calculation)
    std::optional<std::uint32_t> destinationTag;
    std::uint32_t baseFee{15};   // Per-signer base fee in drops (configurable)
    std::optional<AccountID> mainnetIssuer;  // Maps sidechain vault → mainnet IOU issuer
};

/** Build the unsigned mainnet Payment STTx from deterministic parameters.

    The returned STTx has an empty SigningPubKey (required for multisig)
    and Sequence = 0 (required when using TicketSequence).
*/
STTx
buildExportPayment(ExportPaymentParams const& params);

/** Compute the multisign hash for a specific signer.

    For XRPL multisig, each signer signs:
      SHA512Half(HashPrefix::txMultiSign || txFields || signerAccountID)

    @param tx       The unsigned Payment from buildExportPayment
    @param signerID The AccountID of the signer (derived from validator's
                    signing public key via calcAccountID)
    @return         The 256-bit hash that this signer must sign
*/
uint256
exportPaymentMultiSignHash(STTx const& tx, AccountID const& signerID);

/** Parameters for building a deterministic SignerListSet transaction
    to update the mainnet vault's signer list when the UNL changes.
*/
struct SignerListSetParams
{
    AccountID vaultAddress;
    std::uint32_t ticketSeq;
    std::uint32_t signerCount;   // Current count (for fee)
    std::uint32_t quorum;        // New quorum value
    // Sorted list of (AccountID, weight=1) for each validator
    std::vector<AccountID> signerAccounts;
    std::uint32_t baseFee{15};   // Per-signer base fee in drops (configurable)
};

/** Build an unsigned SignerListSet STTx for the mainnet vault. */
STTx
buildSignerListSet(SignerListSetParams const& params);

/** Parameters for building a deterministic TicketCreate transaction
    to replenish the mainnet vault's ticket pool.
*/
struct TicketCreateParams
{
    AccountID vaultAddress;
    std::uint32_t ticketSeq;     // Use a ticket to create more tickets
    std::uint32_t signerCount;
    std::uint32_t ticketCount;   // Number of new tickets to create
    std::uint32_t baseFee{15};   // Per-signer base fee in drops (configurable)
};

/** Build an unsigned TicketCreate STTx for the mainnet vault. */
STTx
buildTicketCreate(TicketCreateParams const& params);

}  // namespace xrpl
