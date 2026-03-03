#pragma once

#include <xrpl/basics/Log.h>
#include <xrpl/basics/base_uint.h>
#include <xrpl/json/json_value.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/STTx.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace xrpl {
namespace import {

// ============================================================
// Named constants
// ============================================================
// Quorum threshold: 80% of validators required.
// Expressed as integer numerator/denominator to avoid floating-point.
constexpr uint64_t kQuorumNumerator = 4;
constexpr uint64_t kQuorumDenominator = 5;
constexpr std::size_t kMaxXPopBlobSize = 512 * 1024;  // 512 KiB
constexpr int kMaxProofDepth = 64;
constexpr int kMaxMerkleDepth = 32;

// ============================================================
// String validation helpers
// ============================================================

bool
isHex(std::string const& str);

bool
isBase58(std::string const& str);

bool
isBase64(std::string const& str);

std::optional<uint64_t>
parseUint64(std::string const& str);

// ============================================================
// XPop syntax checking
// ============================================================

/** Validate the proof section of an XPop (recursive tree/list check) */
bool
syntaxCheckProof(
    Json::Value const& proof,
    beast::Journal const& j,
    int depth = 0);

/** Full syntax check of an XPop blob, returns parsed JSON or empty */
std::optional<Json::Value>
syntaxCheckXPOP(Blob const& blob, beast::Journal const& j);

// ============================================================
// Inner transaction extraction
// ============================================================

/** Extract the inner transaction and metadata from an XPop.
    Returns {STTx, STObject(meta)} pair, or nullptrs on failure. */
std::pair<std::unique_ptr<STTx const>, std::unique_ptr<STObject const>>
getInnerTxn(
    STTx const& outer,
    beast::Journal const& j,
    Json::Value const* xpop = nullptr);

// ============================================================
// VL (Validator List) utilities
// ============================================================

/** Extract VL sequence and master key from an XPop.
    Returns {sequence, masterPublicKey} pair. */
std::optional<std::pair<uint32_t, PublicKey>>
getVLInfo(Json::Value const& xpop, beast::Journal const& j);

// ============================================================
// Proof verification
// ============================================================

/** Check if a proof tree/list contains a given hash string. */
bool
proofContainsHash(
    Json::Value const& proof,
    std::string const& hash,
    int depth = 0);

/** Compute the Merkle root from the proof tree/list. */
uint256
computeMerkleRoot(Json::Value const& proof, int depth = 0);

/** Compute the ledger hash from XPop ledger section. */
uint256
computeLedgerHash(
    Json::Value const& ledgerSection,
    uint256 const& computedTxRoot);

// ============================================================
// Validator quorum
// ============================================================

struct ValidatorListInfo
{
    // nodepub (base58) -> hex signing key
    std::map<std::string, std::string> validators;
    // master pubkey (base58) -> nodepub (base58)
    std::map<std::string, std::string> validatorsMaster;
    uint64_t totalCount = 0;
};

/** Parse the validator list from the UNL blob's validators array. */
ValidatorListInfo
parseValidatorList(
    Json::Value const& validatorArray,
    beast::Journal const& j);

/** Count how many validations confirm a given ledger hash. */
uint64_t
countValidations(
    Json::Value const& validationData,
    ValidatorListInfo const& validators,
    uint256 const& computedLedgerHash,
    beast::Journal const& j);

/** Check if quorum is met. */
bool
hasQuorum(uint64_t totalValidators, uint64_t validationCount);

/** Verify a chain of ledger headers for recovery.
    Given the computed hash of the XPOP's primary ledger, walk the chain
    array forward verifying each entry's phash links to the previous hash.
    Returns the final ledger hash if the chain is valid, or uint256{} on
    failure.
*/
uint256
verifyLedgerChain(
    Json::Value const& chain,
    uint256 const& startingLedgerHash,
    beast::Journal const& j);

}  // namespace import
}  // namespace xrpl
