#include <xrpl/tx/transactors/Import/ImportUtils.h>

#include <xrpl/server/Manifest.h>
#include <xrpl/basics/StringUtilities.h>
#include <xrpl/basics/base64.h>
#include <xrpl/json/json_reader.h>
#include <xrpl/json/to_string.h>
#include <xrpl/protocol/HashPrefix.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STValidation.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/UintTypes.h>
#include <xrpl/protocol/digest.h>
#include <xrpl/protocol/jss.h>

#include <charconv>
#include <set>

namespace xrpl {
namespace import {

// ============================================================
// String validation helpers
// ============================================================

bool
isHex(std::string const& str)
{
    return !str.empty() &&
        str.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
}

bool
isBase58(std::string const& str)
{
    return !str.empty() &&
        str.find_first_not_of(
            "rpshnaf39wBUDNEGHJKLM4PQRST7VWXYZ2bcdeCg65jkm8oFqi1tuvAxyz") ==
        std::string::npos;
}

bool
isBase64(std::string const& str)
{
    return !str.empty() &&
        str.find_first_not_of(
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+"
            "/=") == std::string::npos;
}

std::optional<uint64_t>
parseUint64(std::string const& str)
{
    uint64_t result;
    auto [ptr, ec] =
        std::from_chars(str.data(), str.data() + str.size(), result);
    if (ec == std::errc())
        return result;
    return {};
}

// ============================================================
// XPop syntax checking
// ============================================================

bool
syntaxCheckProof(
    Json::Value const& proof,
    beast::Journal const& j,
    int depth)
{
    if (depth > kMaxProofDepth)
    {
        JLOG(j.warn())
            << "XPOP.transaction.proof list should be less than "
            << kMaxProofDepth << " entries";
        return false;
    }

    if (proof.isArray())
    {
        if (proof.size() != 16)
        {
            JLOG(j.warn())
                << "XPOP.transaction.proof list should be exactly 16 entries";
            return false;
        }
        for (const auto& entry : proof)
        {
            if (entry.isString())
            {
                if (!isHex(entry.asString()) || entry.asString().size() != 64)
                {
                    JLOG(j.warn())
                        << "XPOP.transaction.proof list entry missing "
                           "or wrong format "
                        << "(should be hex string with 64 characters)";
                    return false;
                }
            }
            else if (entry.isArray())
            {
                if (!syntaxCheckProof(entry, j, depth + 1))
                    return false;
            }
            else
            {
                JLOG(j.warn())
                    << "XPOP.transaction.proof list entry has wrong format";
                return false;
            }
        }
    }
    else if (proof.isObject())
    {
        if (depth == 0)
        {
            if (!proof["hash"].isString() ||
                proof["hash"].asString().size() != 64 ||
                !proof["key"].isString() ||
                proof["key"].asString().size() != 64 ||
                !proof["children"].isObject())
            {
                JLOG(j.warn()) << "XPOP.transaction.proof tree node has wrong "
                                  "format (root)";
                return false;
            }
            return syntaxCheckProof(proof["children"], j, depth + 1);
        }

        for (const auto& branch : proof.getMemberNames())
        {
            if (branch.size() != 1 || !isHex(branch))
            {
                JLOG(j.warn())
                    << "XPOP.transaction.proof child node was not 0-F "
                       "hex nibble";
                return false;
            }

            const auto& node = proof[branch];
            if (!node.isObject() || !node["hash"].isString() ||
                node["hash"].asString().size() != 64 ||
                !node["key"].isString() ||
                node["key"].asString().size() != 64 ||
                !node["children"].isObject())
            {
                JLOG(j.warn())
                    << "XPOP.transaction.proof tree node has wrong format";
                return false;
            }
            if (!syntaxCheckProof(node["children"], j, depth + 1))
            {
                JLOG(j.warn())
                    << "XPOP.transaction.proof bad children format";
                return false;
            }
        }
    }
    else
    {
        JLOG(j.warn()) << "XPOP.transaction.proof has wrong format (should be "
                          "array or object)";
        return false;
    }

    return true;
}

std::optional<Json::Value>
syntaxCheckXPOP(Blob const& blob, beast::Journal const& j)
{
    if (blob.empty())
        return {};

    std::string strJson(blob.begin(), blob.end());
    if (strJson.empty())
        return {};

    try
    {
        Json::Value xpop;
        Json::Reader reader;

        if (!reader.parse(strJson, xpop))
        {
            JLOG(j.warn()) << "XPOP failed to parse string json";
            return {};
        }

        if (!xpop.isObject())
        {
            JLOG(j.warn()) << "XPOP is not a JSON object";
            return {};
        }

        if (!xpop["ledger"].isObject() || !xpop["transaction"].isObject() ||
            !xpop["validation"].isObject())
        {
            JLOG(j.warn()) << "XPOP missing required top-level sections";
            return {};
        }

        // Validate ledger section
        auto const& lgr = xpop["ledger"];

        auto checkHex64 = [&](char const* field) -> bool {
            return lgr[field].isString() && lgr[field].asString().size() == 64 &&
                isHex(lgr[field].asString());
        };

        if (!checkHex64("acroot") || !checkHex64("txroot") ||
            !checkHex64("phash"))
        {
            JLOG(j.warn()) << "XPOP.ledger hash fields missing or wrong format";
            return {};
        }

        if (!lgr["close"].isInt() || !lgr["cres"].isInt() ||
            !lgr["index"].isInt() || !lgr["flags"].isInt() ||
            !lgr["pclose"].isInt())
        {
            JLOG(j.warn()) << "XPOP.ledger integer fields missing or wrong format";
            return {};
        }

        // coins can be int or string
        if (lgr["coins"].isInt())
        {
            // ok
        }
        else if (lgr["coins"].isString())
        {
            if (!parseUint64(lgr["coins"].asString()))
            {
                JLOG(j.warn())
                    << "XPOP.ledger.coins wrong format (not parseable uint64)";
                return {};
            }
        }
        else
        {
            JLOG(j.warn()) << "XPOP.ledger.coins missing or wrong type";
            return {};
        }

        // Validate transaction section
        auto const& txn = xpop["transaction"];
        if (!txn["blob"].isString() || !isHex(txn["blob"].asString()))
        {
            JLOG(j.warn()) << "XPOP.transaction.blob missing or wrong format";
            return {};
        }

        if (!txn["meta"].isString() || !isHex(txn["meta"].asString()))
        {
            JLOG(j.warn()) << "XPOP.transaction.meta missing or wrong format";
            return {};
        }

        if (!syntaxCheckProof(txn["proof"], j))
        {
            JLOG(j.warn()) << "XPOP.transaction.proof failed syntax check";
            return {};
        }

        // Validate validation section
        auto const& val = xpop["validation"];
        if (!val["data"].isObject() || !val["unl"].isObject())
        {
            JLOG(j.warn()) << "XPOP.validation.data or .unl missing";
            return {};
        }

        for (const auto& key : val["data"].getMemberNames())
        {
            const auto& value = val["data"][key];
            if (!isBase58(key) || !value.isString() ||
                !isHex(value.asString()))
            {
                JLOG(j.warn())
                    << "XPOP.validation.data entry has wrong format";
                return {};
            }
        }

        // Check UNL required fields
        uint32_t found = 0;
        for (const auto& key : val["unl"].getMemberNames())
        {
            const auto& value = val["unl"][key];
            if (key == "public_key")
            {
                if (!value.isString() || !isHex(value.asString()))
                {
                    JLOG(j.warn())
                        << "XPOP.validation.unl.public_key wrong format";
                    return {};
                }
                auto pk = strUnHex(value.asString());
                if (!publicKeyType(makeSlice(*pk)))
                {
                    JLOG(j.warn())
                        << "XPOP.validation.unl.public_key invalid key type";
                    return {};
                }
                found |= 1;
            }
            else if (key == "manifest")
            {
                if (!value.isString() || !isBase64(value.asString()))
                {
                    JLOG(j.warn())
                        << "XPOP.validation.unl.manifest wrong format";
                    return {};
                }
                found |= 2;
            }
            else if (key == "blob")
            {
                if (!value.isString() || !isBase64(value.asString()))
                {
                    JLOG(j.warn())
                        << "XPOP.validation.unl.blob wrong format";
                    return {};
                }
                found |= 4;
            }
            else if (key == "signature")
            {
                if (!value.isString() || !isHex(value.asString()))
                {
                    JLOG(j.warn())
                        << "XPOP.validation.unl.signature wrong format";
                    return {};
                }
                found |= 8;
            }
            else if (key == "version")
            {
                if (!value.isInt())
                {
                    JLOG(j.warn())
                        << "XPOP.validation.unl.version wrong format";
                    return {};
                }
                found |= 16;
            }
        }

        if (found != 0b11111)
        {
            JLOG(j.warn()) << "XPOP.validation.unl missing required field(s)";
            return {};
        }

        // Optional chain array for ledger-chaining recovery
        if (xpop.isMember("chain"))
        {
            auto const& chain = xpop["chain"];
            if (!chain.isArray())
            {
                JLOG(j.warn()) << "XPOP.chain must be an array";
                return {};
            }

            for (unsigned int i = 0; i < chain.size(); ++i)
            {
                auto const& entry = chain[i];
                if (!entry.isObject())
                {
                    JLOG(j.warn())
                        << "XPOP.chain[" << i << "] is not an object";
                    return {};
                }

                // Same field validation as the ledger section
                auto checkChainHex64 =
                    [&](char const* field) -> bool {
                    return entry[field].isString() &&
                        entry[field].asString().size() == 64 &&
                        isHex(entry[field].asString());
                };

                if (!checkChainHex64("acroot") ||
                    !checkChainHex64("txroot") ||
                    !checkChainHex64("phash"))
                {
                    JLOG(j.warn())
                        << "XPOP.chain[" << i
                        << "] hash fields missing or wrong format";
                    return {};
                }

                if (!entry["close"].isInt() || !entry["cres"].isInt() ||
                    !entry["index"].isInt() || !entry["flags"].isInt() ||
                    !entry["pclose"].isInt())
                {
                    JLOG(j.warn())
                        << "XPOP.chain[" << i
                        << "] integer fields missing or wrong format";
                    return {};
                }

                if (entry["coins"].isInt())
                {
                    // ok
                }
                else if (entry["coins"].isString())
                {
                    if (!parseUint64(entry["coins"].asString()))
                    {
                        JLOG(j.warn())
                            << "XPOP.chain[" << i
                            << "].coins wrong format";
                        return {};
                    }
                }
                else
                {
                    JLOG(j.warn())
                        << "XPOP.chain[" << i
                        << "].coins missing or wrong type";
                    return {};
                }
            }
        }

        return xpop;
    }
    catch (...)
    {
        JLOG(j.warn()) << "Exception occurred during XPOP validation";
    }

    return {};
}

// ============================================================
// Inner transaction extraction
// ============================================================

std::pair<std::unique_ptr<STTx const>, std::unique_ptr<STObject const>>
getInnerTxn(
    STTx const& outer,
    beast::Journal const& j,
    Json::Value const* xpop)
{
    std::optional<Json::Value> xpop_storage;

    if (!xpop && outer.isFieldPresent(sfBlob))
    {
        xpop_storage = syntaxCheckXPOP(outer.getFieldVL(sfBlob), j);
        xpop = &(*xpop_storage);
    }

    if (!xpop)
        return {};

    auto rawTx = strUnHex((*xpop)[jss::transaction][jss::blob].asString());
    auto meta = strUnHex((*xpop)[jss::transaction][jss::meta].asString());

    if (!rawTx)
    {
        JLOG(j.warn()) << "Import: failed to deserialize tx blob (invalid hex) "
                       << outer.getTransactionID();
        return {};
    }

    if (!meta)
    {
        JLOG(j.warn()) << "Import: failed to deserialize tx meta (invalid hex) "
                       << outer.getTransactionID();
        return {};
    }

    try
    {
        return {
            std::make_unique<STTx const>(
                SerialIter{rawTx->data(), rawTx->size()}),
            std::make_unique<STObject const>(
                SerialIter(meta->data(), meta->size()), sfMetadata)};
    }
    catch (std::exception& e)
    {
        JLOG(j.warn()) << "Import: failed to deserialize tx blob/meta ("
                       << e.what()
                       << ") outer txid: " << outer.getTransactionID();
        return {};
    }
}

// ============================================================
// VL utilities
// ============================================================

std::optional<std::pair<uint32_t, PublicKey>>
getVLInfo(Json::Value const& xpop, beast::Journal const& j)
{
    auto const data = base64_decode(
        xpop["validation"][jss::unl][jss::blob].asString());
    Json::Reader r;
    Json::Value list;
    if (!r.parse(data, list))
    {
        JLOG(j.warn())
            << "Import: unl blob was not valid json (after base64 decoding)";
        return {};
    }
    auto const sequence = list[jss::sequence].asUInt();
    auto const m = deserializeManifest(base64_decode(
        xpop["validation"][jss::unl][jss::manifest].asString()));
    if (!m)
    {
        JLOG(j.warn()) << "Import: failed to deserialize manifest";
        return {};
    }
    return {{sequence, m->masterKey}};
}

// ============================================================
// Proof verification
// ============================================================

bool
proofContainsHash(
    Json::Value const& proof,
    std::string const& hash,
    int depth)
{
    if (depth > kMaxMerkleDepth)
        return false;

    if (!proof.isObject() && !proof.isArray())
        return false;

    Json::Value const* p = &proof;

    if (proof.isMember("children"))
        p = &proof["children"];

    for (int x = 0; x < 16; ++x)
    {
        Json::Value const* entry = p->isObject()
            ? &((*p)[std::string(1, "0123456789ABCDEF"[x])])
            : &((*p)[x]);

        if (entry->isNull())
            continue;

        if ((entry->isString() && entry->asString() == hash) ||
            (entry->isObject() && entry->isMember(jss::hash) &&
             (*entry)[jss::hash] == hash) ||
            proofContainsHash(*entry, hash, depth + 1))
            return true;
    }

    return false;
}

uint256
computeMerkleRoot(Json::Value const& proof, int depth)
{
    const uint256 nullhash;

    if (depth > kMaxMerkleDepth)
        return nullhash;

    if (!proof.isObject() && !proof.isArray())
        return nullhash;

    sha512_half_hasher h;
    using beast::hash_append;
    hash_append(h, HashPrefix::innerNode);

    if (proof.isArray())
    {
        for (const auto& entry : proof)
        {
            if (entry.isString())
            {
                uint256 entryHash;
                if (entryHash.parseHex(entry.asString()))
                    hash_append(h, entryHash);
            }
            else
            {
                hash_append(h, computeMerkleRoot(entry, depth + 1));
            }
        }
    }
    else if (proof.isObject())
    {
        for (int x = 0; x < 16; ++x)
        {
            std::string const nibble(1, "0123456789ABCDEF"[x]);
            if (!proof["children"].isMember(nibble))
            {
                hash_append(h, nullhash);
            }
            else if (
                proof["children"][nibble]["children"].size() == 0u)
            {
                uint256 entryHash;
                if (entryHash.parseHex(
                        proof["children"][nibble][jss::hash].asString()))
                    hash_append(h, entryHash);
            }
            else
            {
                hash_append(
                    h,
                    computeMerkleRoot(
                        proof["children"][nibble], depth + 1));
            }
        }
    }

    return static_cast<uint256>(h);
}

uint256
computeLedgerHash(
    Json::Value const& lgr,
    uint256 const& computedTxRoot)
{
    auto coins = parseUint64(lgr["coins"].asString());
    uint256 phash, acroot;

    if (!coins || !phash.parseHex(lgr["phash"].asString()) ||
        !acroot.parseHex(lgr["acroot"].asString()))
    {
        return uint256{};
    }

    return sha512Half(
        HashPrefix::ledgerMaster,
        std::uint32_t(lgr["index"].asUInt()),
        *coins,
        phash,
        computedTxRoot,
        acroot,
        std::uint32_t(lgr["pclose"].asUInt()),
        std::uint32_t(lgr["close"].asUInt()),
        std::uint8_t(lgr["cres"].asUInt()),
        std::uint8_t(lgr["flags"].asUInt()));
}

// ============================================================
// Validator quorum
// ============================================================

ValidatorListInfo
parseValidatorList(
    Json::Value const& validatorArray,
    beast::Journal const& j)
{
    ValidatorListInfo info;

    for (auto const& val : validatorArray)
    {
        if (!val.isObject() || !val.isMember(jss::validation_public_key) ||
            !val[jss::validation_public_key].isString() ||
            !val.isMember(jss::manifest) || !val[jss::manifest].isString())
        {
            JLOG(j.warn()) << "Import: unl blob contained invalid "
                              "validator entry, skipping";
            continue;
        }

        std::optional<Blob> const ret =
            strUnHex(val[jss::validation_public_key].asString());

        if (!ret || !publicKeyType(makeSlice(*ret)))
        {
            JLOG(j.warn()) << "Import: unl blob contained an invalid "
                              "validator key, skipping "
                           << val[jss::validation_public_key].asString();
            continue;
        }

        auto const m =
            deserializeManifest(base64_decode(val[jss::manifest].asString()));

        if (!m)
        {
            JLOG(j.warn())
                << "Import: unl blob contained an invalid manifest, skipping";
            continue;
        }

        if (strHex(m->masterKey) != val[jss::validation_public_key])
        {
            JLOG(j.warn())
                << "Import: manifest master key did not match, skipping";
            continue;
        }

        if (!m->verify())
        {
            JLOG(j.warn())
                << "Import: manifest signature invalid, skipping";
            continue;
        }

        if (!m->signingKey)
        {
            JLOG(j.warn())
                << "Import: manifest has no signing key (revoked), skipping";
            continue;
        }

        // Only count validators that pass all validation checks
        info.totalCount++;

        std::string const nodepub =
            toBase58(TokenType::NodePublic, *m->signingKey);
        std::string const nodemaster =
            toBase58(TokenType::NodePublic, m->masterKey);
        info.validators[nodepub] = strHex(*m->signingKey);
        info.validatorsMaster[nodemaster] = nodepub;
    }

    return info;
}

uint64_t
countValidations(
    Json::Value const& validationData,
    ValidatorListInfo const& validators,
    uint256 const& computedLedgerHash,
    beast::Journal const& j)
{
    uint64_t validationCount = 0;
    std::set<std::string> usedKeys;

    for (const auto& key : validationData.getMemberNames())
    {
        auto nodepub = key;
        auto const datakey = nodepub;

        // If the specified node address is a master address, remap to the
        // regular signing address
        auto regular = validators.validatorsMaster.find(nodepub);
        if (regular != validators.validatorsMaster.end() &&
            validators.validators.find(regular->second) !=
                validators.validators.end())
        {
            usedKeys.emplace(nodepub);
            nodepub = regular->second;
        }

        auto const signingKey = validators.validators.find(nodepub);
        if (signingKey == validators.validators.end())
        {
            JLOG(j.trace()) << "Import: validator nodepub " << nodepub
                            << " not in validator list, skipping";
            continue;
        }

        if (usedKeys.find(nodepub) != usedKeys.end())
        {
            JLOG(j.trace()) << "Import: validator nodepub " << nodepub
                            << " key appears more than once, skipping";
            continue;
        }

        usedKeys.emplace(nodepub);

        try
        {
            auto const valBlob =
                strUnHex(validationData[datakey].asString());

            if (!valBlob)
            {
                JLOG(j.warn())
                    << "Import: validation was not valid hex, nodepub: "
                    << nodepub;
                continue;
            }

            SerialIter sit(makeSlice(*valBlob));
            auto val = std::make_unique<STValidation>(
                std::ref(sit),
                [](PublicKey const& pk) { return calcNodeID(pk); },
                false);

            if (val->getLedgerHash() != computedLedgerHash)
            {
                JLOG(j.warn())
                    << "Import: validation was not for computed ledger hash "
                    << computedLedgerHash << " it was for "
                    << val->getLedgerHash();
                continue;
            }

            if (strHex(val->getSignerPublic()) != signingKey->second)
            {
                JLOG(j.warn())
                    << "Import: validation not signed with recognised key, "
                    << "nodepub: " << nodepub;
                continue;
            }

            if (!val->isValid())
            {
                JLOG(j.warn()) << "Import: validation not correctly signed, "
                               << "nodepub: " << nodepub;
                continue;
            }

            validationCount++;
        }
        catch (...)
        {
            JLOG(j.warn()) << "Import: validation not parseable, "
                           << "nodepub: " << nodepub;
            continue;
        }
    }

    return validationCount;
}

bool
hasQuorum(uint64_t totalValidators, uint64_t validationCount)
{
    // Integer arithmetic: quorum = ceil(totalValidators * 4/5)
    uint64_t quorum =
        (totalValidators * kQuorumNumerator + kQuorumDenominator - 1) /
        kQuorumDenominator;
    if (quorum == 0)
        quorum = 1;
    return validationCount >= quorum;
}

// ============================================================
// Ledger chaining for recovery
// ============================================================

uint256
verifyLedgerChain(
    Json::Value const& chain,
    uint256 const& startingLedgerHash,
    beast::Journal const& j)
{
    if (!chain.isArray() || chain.size() == 0)
        return uint256{};

    uint256 prevHash = startingLedgerHash;

    for (unsigned int i = 0; i < chain.size(); ++i)
    {
        auto const& entry = chain[i];

        // Verify phash links to previous ledger
        uint256 entryPhash;
        if (!entryPhash.parseHex(entry["phash"].asString()))
        {
            JLOG(j.warn())
                << "Import: chain[" << i << "] phash not valid hex";
            return uint256{};
        }

        if (entryPhash != prevHash)
        {
            JLOG(j.warn())
                << "Import: chain[" << i
                << "] phash does not match previous hash. Expected "
                << prevHash << " got " << entryPhash;
            return uint256{};
        }

        // Compute this chain entry's ledger hash.
        // Chain entries don't have transactions relevant to us, so txroot
        // is taken directly from the entry (not recomputed from proof).
        uint256 txroot;
        if (!txroot.parseHex(entry["txroot"].asString()))
        {
            JLOG(j.warn())
                << "Import: chain[" << i << "] txroot not valid hex";
            return uint256{};
        }

        prevHash = computeLedgerHash(entry, txroot);

        if (prevHash == uint256{})
        {
            JLOG(j.warn())
                << "Import: chain[" << i
                << "] error computing ledger hash";
            return uint256{};
        }
    }

    return prevHash;
}

}  // namespace import
}  // namespace xrpl
