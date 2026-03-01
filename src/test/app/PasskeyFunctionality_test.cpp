//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2024 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

// Comprehensive functionality test suite for XLS-XXd P256 Passkey
// Authentication.  Covers SetPasskeyList creation, deletion, validation,
// duplicate detection, and publicKeyType behaviour.

#include <test/jtx.h>

#include <xrpl/basics/Slice.h>
#include <xrpl/basics/StringUtilities.h>
#include <xrpl/basics/strHex.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/KeyType.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/protocol/jss.h>

namespace xrpl {
namespace test {

struct PasskeyFunctionality_test : public beast::unit_test::suite
{
    // =========================================================================
    // Helpers
    // =========================================================================

    // Build a SetPasskeyList transaction from a pre-built JSON passkeys array.
    Json::Value
    setPasskeyList(
        jtx::Account const& account,
        Json::Value const& passkeys)
    {
        Json::Value jv;
        jv[jss::TransactionType] = jss::PasskeyListSet;
        jv[jss::Account] = account.human();
        jv[sfPasskeys.jsonName] = passkeys;
        return jv;
    }

    // Build a SetPasskeyList transaction from a vector of (id-hex, pk-hex)
    // pairs -- a convenience wrapper used by several test cases.
    Json::Value
    setPasskeyListFromPairs(
        jtx::Account const& account,
        std::vector<std::pair<std::string, std::string>> const& passkeys)
    {
        Json::Value arr(Json::arrayValue);
        for (auto const& [id, pk] : passkeys)
        {
            Json::Value entry;
            entry[sfPasskeyID.jsonName] = id;
            entry[sfPublicKey.jsonName] = pk;
            arr.append(entry);
        }
        return setPasskeyList(account, arr);
    }

    // Create a single passkey entry as JSON.
    static Json::Value
    makePasskeyEntry(
        std::string const& passkeyId,
        std::string const& publicKeyHex)
    {
        Json::Value entry;
        entry[sfPasskeyID.jsonName] = passkeyId;
        entry[sfPublicKey.jsonName] = publicKeyHex;
        return entry;
    }

    // Generate a synthetic 65-byte P256 public key hex string.
    // The key has the 0xF6 prefix followed by deterministic test data.
    // These are NOT valid curve points, but SetPasskeyList preflight
    // only checks prefix and size (not point-on-curve), so they are
    // accepted by the current implementation.
    static std::string
    makeP256KeyHex(uint8_t seed = 1)
    {
        // 65 bytes = 0xF6 + 64 bytes of filler.
        std::string hex = "F6";
        for (int i = 1; i < 65; ++i)
        {
            uint8_t byte =
                static_cast<uint8_t>((seed * 37 + i) & 0xFF);
            hex += strHex(Slice(&byte, 1));
        }
        return hex;
    }

    // Generate a unique PasskeyID hex string from a numeric index.
    // Produces a zero-padded 64-char hex string (32 bytes / uint256).
    static std::string
    makePasskeyIdHex(unsigned index)
    {
        // Build a 32-byte big-endian representation of `index`.
        std::string hex;
        hex.reserve(64);
        for (int i = 0; i < 28; ++i)
            hex += "00";
        // Last 4 bytes encode the index in big-endian.
        uint8_t buf[4];
        buf[0] = static_cast<uint8_t>((index >> 24) & 0xFF);
        buf[1] = static_cast<uint8_t>((index >> 16) & 0xFF);
        buf[2] = static_cast<uint8_t>((index >> 8) & 0xFF);
        buf[3] = static_cast<uint8_t>(index & 0xFF);
        hex += strHex(Slice(buf, 4));
        return hex;
    }

    // =========================================================================
    // 1. testSetPasskeyListCreate
    // =========================================================================

    void
    testSetPasskeyListCreate(FeatureBitset features)
    {
        testcase("SetPasskeyList - create");

        using namespace test::jtx;

        // -----------------------------------------------------------------
        // 1a. Success: create with 1 valid P256 key
        // -----------------------------------------------------------------
        {
            Env env{*this, features};
            Account const alice{"alice"};
            env.fund(XRP(10000), alice);
            env.close();

            BEAST_EXPECT(ownerCount(env, alice) == 0);

            Json::Value passkeys(Json::arrayValue);
            passkeys.append(
                makePasskeyEntry(makePasskeyIdHex(1), makeP256KeyHex(1)));

            env(setPasskeyList(alice, passkeys), ter(tesSUCCESS));
            env.close();

            // Verify the ledger object exists.
            auto const sle = env.le(keylet::passkeyList(alice.id()));
            BEAST_EXPECT(sle != nullptr);

            // Owner count should have increased by 1.
            BEAST_EXPECT(ownerCount(env, alice) == 1);
        }

        // -----------------------------------------------------------------
        // 1b. Success: create with multiple P256 keys (up to 10)
        // -----------------------------------------------------------------
        {
            Env env{*this, features};
            Account const bob{"bob"};
            env.fund(XRP(10000), bob);
            env.close();

            Json::Value passkeys(Json::arrayValue);
            for (unsigned i = 1; i <= 10; ++i)
            {
                passkeys.append(makePasskeyEntry(
                    makePasskeyIdHex(i), makeP256KeyHex(i)));
            }

            env(setPasskeyList(bob, passkeys), ter(tesSUCCESS));
            env.close();

            auto const sle = env.le(keylet::passkeyList(bob.id()));
            BEAST_EXPECT(sle != nullptr);
            BEAST_EXPECT(ownerCount(env, bob) == 1);
        }

        // -----------------------------------------------------------------
        // 1c. Reject: too many passkeys (> 10)
        // -----------------------------------------------------------------
        {
            Env env{*this, features};
            Account const carol{"carol"};
            env.fund(XRP(10000), carol);
            env.close();

            Json::Value passkeys(Json::arrayValue);
            for (unsigned i = 1; i <= 11; ++i)
            {
                passkeys.append(makePasskeyEntry(
                    makePasskeyIdHex(i), makeP256KeyHex(i)));
            }

            env(setPasskeyList(carol, passkeys), ter(temMALFORMED));
            env.close();

            // No ledger object should have been created.
            auto const sle = env.le(keylet::passkeyList(carol.id()));
            BEAST_EXPECT(sle == nullptr);
            BEAST_EXPECT(ownerCount(env, carol) == 0);
        }

        // -----------------------------------------------------------------
        // 1d. Reject: invalid flags
        // -----------------------------------------------------------------
        {
            Env env{*this, features};
            Account const dave{"dave"};
            env.fund(XRP(10000), dave);
            env.close();

            Json::Value passkeys(Json::arrayValue);
            passkeys.append(
                makePasskeyEntry(makePasskeyIdHex(1), makeP256KeyHex(1)));

            auto jv = setPasskeyList(dave, passkeys);
            jv[jss::Flags] = tfUniversalMask;

            env(jv, ter(temINVALID_FLAG));
            env.close();

            auto const sle = env.le(keylet::passkeyList(dave.id()));
            BEAST_EXPECT(sle == nullptr);
        }
    }

    // =========================================================================
    // 2. testSetPasskeyListDelete
    // =========================================================================

    void
    testSetPasskeyListDelete(FeatureBitset features)
    {
        testcase("SetPasskeyList - delete");

        using namespace test::jtx;

        Env env{*this, features};
        Account const alice{"alice"};
        env.fund(XRP(10000), alice);
        env.close();

        // First, create a passkey list.
        Json::Value passkeys(Json::arrayValue);
        passkeys.append(
            makePasskeyEntry(makePasskeyIdHex(1), makeP256KeyHex(1)));

        env(setPasskeyList(alice, passkeys), ter(tesSUCCESS));
        env.close();

        BEAST_EXPECT(ownerCount(env, alice) == 1);
        auto sle = env.le(keylet::passkeyList(alice.id()));
        BEAST_EXPECT(sle != nullptr);

        // -----------------------------------------------------------------
        // 2a. Delete by sending empty passkeys array
        // -----------------------------------------------------------------
        Json::Value emptyPasskeys(Json::arrayValue);
        env(setPasskeyList(alice, emptyPasskeys), ter(tesSUCCESS));
        env.close();

        // -----------------------------------------------------------------
        // 2b. Verify PasskeyList ledger object is removed
        // -----------------------------------------------------------------
        sle = env.le(keylet::passkeyList(alice.id()));
        BEAST_EXPECT(sle == nullptr);
        BEAST_EXPECT(ownerCount(env, alice) == 0);

        // -----------------------------------------------------------------
        // 2c. Deleting when no list exists should fail
        // -----------------------------------------------------------------
        env(setPasskeyList(alice, emptyPasskeys), ter(tecNO_ENTRY));
        env.close();
    }

    // =========================================================================
    // 3. testSetPasskeyListValidation
    // =========================================================================

    void
    testSetPasskeyListValidation(FeatureBitset features)
    {
        testcase("SetPasskeyList - validation");

        using namespace test::jtx;

        Env env{*this, features};
        Account const alice{"alice"};
        env.fund(XRP(10000), alice);
        env.close();

        // -----------------------------------------------------------------
        // 3a. Reject passkey with missing PasskeyID
        // -----------------------------------------------------------------
        {
            Json::Value passkeys(Json::arrayValue);
            Json::Value entry;
            // No PasskeyID field, only PublicKey.
            entry[sfPublicKey.jsonName] = makeP256KeyHex(1);
            passkeys.append(entry);

            env(setPasskeyList(alice, passkeys), ter(temMALFORMED));
            env.close();
        }

        // -----------------------------------------------------------------
        // 3b. Reject passkey with missing PublicKey
        // -----------------------------------------------------------------
        {
            Json::Value passkeys(Json::arrayValue);
            Json::Value entry;
            entry[sfPasskeyID.jsonName] = makePasskeyIdHex(1);
            // No PublicKey field.
            passkeys.append(entry);

            env(setPasskeyList(alice, passkeys), ter(temMALFORMED));
            env.close();
        }

        // -----------------------------------------------------------------
        // 3c. Reject non-P256 public key: secp256k1 (33 bytes, 0x02 prefix)
        // -----------------------------------------------------------------
        {
            // 33-byte secp256k1 compressed key
            std::string secpKey = "02";
            for (int i = 0; i < 32; ++i)
                secpKey += "01";

            Json::Value passkeys(Json::arrayValue);
            passkeys.append(
                makePasskeyEntry(makePasskeyIdHex(1), secpKey));

            env(setPasskeyList(alice, passkeys), ter(temMALFORMED));
            env.close();
        }

        // -----------------------------------------------------------------
        // 3d. Reject non-P256 public key: secp256k1 (33 bytes, 0x03 prefix)
        // -----------------------------------------------------------------
        {
            std::string secpKey = "03";
            for (int i = 0; i < 32; ++i)
                secpKey += "AA";

            Json::Value passkeys(Json::arrayValue);
            passkeys.append(
                makePasskeyEntry(makePasskeyIdHex(1), secpKey));

            env(setPasskeyList(alice, passkeys), ter(temMALFORMED));
            env.close();
        }

        // -----------------------------------------------------------------
        // 3e. Reject non-P256 public key: ed25519 (33 bytes, 0xED prefix)
        // -----------------------------------------------------------------
        {
            std::string edKey = "ED";
            for (int i = 0; i < 32; ++i)
                edKey += "01";

            Json::Value passkeys(Json::arrayValue);
            passkeys.append(
                makePasskeyEntry(makePasskeyIdHex(1), edKey));

            env(setPasskeyList(alice, passkeys), ter(temMALFORMED));
            env.close();
        }

        // -----------------------------------------------------------------
        // 3f. Reject 65-byte key with wrong prefix (0x04 instead of 0xF6)
        // -----------------------------------------------------------------
        {
            // 65 bytes starting with 0x04 (uncompressed secp256k1 format).
            std::string wrongPrefixKey = "04";
            for (int i = 0; i < 64; ++i)
                wrongPrefixKey += "01";

            Json::Value passkeys(Json::arrayValue);
            passkeys.append(
                makePasskeyEntry(makePasskeyIdHex(1), wrongPrefixKey));

            env(setPasskeyList(alice, passkeys), ter(temMALFORMED));
            env.close();
        }

        // -----------------------------------------------------------------
        // 3g. Reject key that is too short (< 65 bytes but has 0xF6 prefix)
        // -----------------------------------------------------------------
        {
            // 5 bytes with P256 prefix.
            std::string shortKey = "F601020304";

            Json::Value passkeys(Json::arrayValue);
            passkeys.append(
                makePasskeyEntry(makePasskeyIdHex(1), shortKey));

            env(setPasskeyList(alice, passkeys), ter(temMALFORMED));
            env.close();
        }

        // -----------------------------------------------------------------
        // 3h. Reject key that is too long (> 65 bytes)
        // -----------------------------------------------------------------
        {
            std::string longKey = "F6";
            for (int i = 0; i < 66; ++i)
                longKey += "01";  // 67 bytes total

            Json::Value passkeys(Json::arrayValue);
            passkeys.append(
                makePasskeyEntry(makePasskeyIdHex(1), longKey));

            env(setPasskeyList(alice, passkeys), ter(temMALFORMED));
            env.close();
        }

        // -----------------------------------------------------------------
        // 3i. Reject empty public key (zero bytes)
        // -----------------------------------------------------------------
        {
            Json::Value passkeys(Json::arrayValue);
            passkeys.append(
                makePasskeyEntry(makePasskeyIdHex(1), ""));

            env(setPasskeyList(alice, passkeys), ter(temMALFORMED));
            env.close();
        }

        // -----------------------------------------------------------------
        // After all rejections, no PasskeyList should exist.
        // -----------------------------------------------------------------
        auto const sle = env.le(keylet::passkeyList(alice.id()));
        BEAST_EXPECT(sle == nullptr);
        BEAST_EXPECT(ownerCount(env, alice) == 0);
    }

    // =========================================================================
    // 4. testSetPasskeyListDuplicates
    // =========================================================================

    void
    testSetPasskeyListDuplicates(FeatureBitset features)
    {
        testcase("SetPasskeyList - duplicate detection");

        using namespace test::jtx;

        Env env{*this, features};
        Account const alice{"alice"};
        env.fund(XRP(10000), alice);
        env.close();

        auto const id1 = makePasskeyIdHex(1);
        auto const id2 = makePasskeyIdHex(2);
        auto const pk1 = makeP256KeyHex(1);
        auto const pk2 = makeP256KeyHex(2);

        // -----------------------------------------------------------------
        // 4a. Reject duplicate PasskeyID (same ID, different keys)
        // -----------------------------------------------------------------
        {
            Json::Value passkeys(Json::arrayValue);
            passkeys.append(makePasskeyEntry(id1, pk1));
            passkeys.append(makePasskeyEntry(id1, pk2));  // same ID

            env(setPasskeyList(alice, passkeys), ter(temMALFORMED));
            env.close();
        }

        // -----------------------------------------------------------------
        // 4b. Reject duplicate PublicKey (different IDs, same key)
        // -----------------------------------------------------------------
        {
            Json::Value passkeys(Json::arrayValue);
            passkeys.append(makePasskeyEntry(id1, pk1));
            passkeys.append(makePasskeyEntry(id2, pk1));  // same key

            env(setPasskeyList(alice, passkeys), ter(temMALFORMED));
            env.close();
        }

        // -----------------------------------------------------------------
        // 4c. Reject fully duplicate entries (same ID and same key)
        // -----------------------------------------------------------------
        {
            Json::Value passkeys(Json::arrayValue);
            passkeys.append(makePasskeyEntry(id1, pk1));
            passkeys.append(makePasskeyEntry(id1, pk1));

            env(setPasskeyList(alice, passkeys), ter(temMALFORMED));
            env.close();
        }

        // -----------------------------------------------------------------
        // 4d. Two different passkeys with different IDs and keys succeed
        // -----------------------------------------------------------------
        {
            Json::Value passkeys(Json::arrayValue);
            passkeys.append(makePasskeyEntry(id1, pk1));
            passkeys.append(makePasskeyEntry(id2, pk2));

            env(setPasskeyList(alice, passkeys), ter(tesSUCCESS));
            env.close();

            auto const sle = env.le(keylet::passkeyList(alice.id()));
            BEAST_EXPECT(sle != nullptr);
            BEAST_EXPECT(ownerCount(env, alice) == 1);
        }
    }

    // =========================================================================
    // 5. testPublicKeyType
    // =========================================================================

    void
    testPublicKeyType(FeatureBitset features)
    {
        testcase("publicKeyType identification");

        // This test verifies the publicKeyType() function distinguishes
        // key types correctly, tested indirectly through Slice inspection
        // and through SetPasskeyList transaction validation.

        using namespace test::jtx;

        // -----------------------------------------------------------------
        // 5a. 33-byte key with 0xED prefix -> ed25519
        // -----------------------------------------------------------------
        {
            uint8_t key[33];
            key[0] = 0xED;
            std::memset(key + 1, 0x42, 32);
            auto const keyType = publicKeyType(Slice(key, sizeof(key)));
            BEAST_EXPECT(keyType.has_value());
            if (keyType)
                BEAST_EXPECT(*keyType == KeyType::ed25519);
        }

        // -----------------------------------------------------------------
        // 5b. 33-byte key with 0x02 prefix -> secp256k1
        // -----------------------------------------------------------------
        {
            uint8_t key[33];
            key[0] = 0x02;
            std::memset(key + 1, 0x42, 32);
            auto const keyType = publicKeyType(Slice(key, sizeof(key)));
            BEAST_EXPECT(keyType.has_value());
            if (keyType)
                BEAST_EXPECT(*keyType == KeyType::secp256k1);
        }

        // -----------------------------------------------------------------
        // 5c. 33-byte key with 0x03 prefix -> secp256k1
        // -----------------------------------------------------------------
        {
            uint8_t key[33];
            key[0] = 0x03;
            std::memset(key + 1, 0x42, 32);
            auto const keyType = publicKeyType(Slice(key, sizeof(key)));
            BEAST_EXPECT(keyType.has_value());
            if (keyType)
                BEAST_EXPECT(*keyType == KeyType::secp256k1);
        }

        // -----------------------------------------------------------------
        // 5d. 65-byte key with 0xF6 prefix -> p256
        // -----------------------------------------------------------------
        {
            uint8_t key[65];
            key[0] = 0xF6;
            std::memset(key + 1, 0x42, 64);
            auto const keyType = publicKeyType(Slice(key, sizeof(key)));
            BEAST_EXPECT(keyType.has_value());
            if (keyType)
                BEAST_EXPECT(*keyType == KeyType::p256);
        }

        // -----------------------------------------------------------------
        // 5e. 65-byte key without 0xF6 prefix -> nullopt (rejected)
        // -----------------------------------------------------------------
        {
            // 0x04 prefix (uncompressed secp256k1)
            uint8_t key[65];
            key[0] = 0x04;
            std::memset(key + 1, 0x42, 64);
            auto const keyType = publicKeyType(Slice(key, sizeof(key)));
            // Should either be nullopt or NOT p256.
            if (keyType)
                BEAST_EXPECT(*keyType != KeyType::p256);
        }

        // -----------------------------------------------------------------
        // 5f. 65-byte key with 0x00 prefix -> nullopt (rejected)
        // -----------------------------------------------------------------
        {
            uint8_t key[65];
            key[0] = 0x00;
            std::memset(key + 1, 0x42, 64);
            auto const keyType = publicKeyType(Slice(key, sizeof(key)));
            if (keyType)
                BEAST_EXPECT(*keyType != KeyType::p256);
        }

        // -----------------------------------------------------------------
        // 5g. 65-byte key with 0xFF prefix -> nullopt (rejected)
        // -----------------------------------------------------------------
        {
            uint8_t key[65];
            key[0] = 0xFF;
            std::memset(key + 1, 0x42, 64);
            auto const keyType = publicKeyType(Slice(key, sizeof(key)));
            if (keyType)
                BEAST_EXPECT(*keyType != KeyType::p256);
        }

        // -----------------------------------------------------------------
        // 5h. Wrong size key (16 bytes) -> nullopt
        // -----------------------------------------------------------------
        {
            uint8_t key[16];
            std::memset(key, 0xAB, sizeof(key));
            auto const keyType = publicKeyType(Slice(key, sizeof(key)));
            BEAST_EXPECT(!keyType.has_value());
        }

        // -----------------------------------------------------------------
        // 5i. Wrong size key (1 byte) -> nullopt
        // -----------------------------------------------------------------
        {
            uint8_t key[1] = {0xF6};
            auto const keyType = publicKeyType(Slice(key, sizeof(key)));
            BEAST_EXPECT(!keyType.has_value());
        }

        // -----------------------------------------------------------------
        // 5j. Empty slice -> nullopt
        // -----------------------------------------------------------------
        {
            auto const keyType = publicKeyType(Slice(nullptr, 0));
            BEAST_EXPECT(!keyType.has_value());
        }

        // -----------------------------------------------------------------
        // 5k. Indirect test via SetPasskeyList: P256 key accepted,
        //     secp256k1 and ed25519 rejected.
        // -----------------------------------------------------------------
        {
            Env env{*this, features};
            Account const tester{"tester"};
            env.fund(XRP(10000), tester);
            env.close();

            auto const id = makePasskeyIdHex(1);

            // Valid P256 key -- should succeed.
            {
                Json::Value passkeys(Json::arrayValue);
                passkeys.append(makePasskeyEntry(id, makeP256KeyHex(1)));
                env(setPasskeyList(tester, passkeys), ter(tesSUCCESS));
                env.close();
            }

            // Clean up for the next sub-test.
            {
                Json::Value empty(Json::arrayValue);
                env(setPasskeyList(tester, empty), ter(tesSUCCESS));
                env.close();
            }

            // ed25519 key -- should be rejected.
            {
                std::string edKey = "ED";
                for (int i = 0; i < 32; ++i)
                    edKey += "42";
                Json::Value passkeys(Json::arrayValue);
                passkeys.append(makePasskeyEntry(id, edKey));
                env(setPasskeyList(tester, passkeys), ter(temMALFORMED));
                env.close();
            }

            // secp256k1 key -- should be rejected.
            {
                std::string secpKey = "02";
                for (int i = 0; i < 32; ++i)
                    secpKey += "42";
                Json::Value passkeys(Json::arrayValue);
                passkeys.append(makePasskeyEntry(id, secpKey));
                env(setPasskeyList(tester, passkeys), ter(temMALFORMED));
                env.close();
            }
        }
    }

    // =========================================================================
    // 6. testSetPasskeyListUpdate
    // =========================================================================

    void
    testSetPasskeyListUpdate(FeatureBitset features)
    {
        testcase("SetPasskeyList - update existing list");

        using namespace test::jtx;

        Env env{*this, features};
        Account const alice{"alice"};
        env.fund(XRP(10000), alice);
        env.close();

        // Create initial passkey list with 2 entries.
        {
            Json::Value passkeys(Json::arrayValue);
            passkeys.append(
                makePasskeyEntry(makePasskeyIdHex(1), makeP256KeyHex(1)));
            passkeys.append(
                makePasskeyEntry(makePasskeyIdHex(2), makeP256KeyHex(2)));

            env(setPasskeyList(alice, passkeys), ter(tesSUCCESS));
            env.close();
            BEAST_EXPECT(ownerCount(env, alice) == 1);
        }

        // Update to a different set of passkeys (3 entries now).
        {
            Json::Value passkeys(Json::arrayValue);
            passkeys.append(
                makePasskeyEntry(makePasskeyIdHex(10), makeP256KeyHex(10)));
            passkeys.append(
                makePasskeyEntry(makePasskeyIdHex(11), makeP256KeyHex(11)));
            passkeys.append(
                makePasskeyEntry(makePasskeyIdHex(12), makeP256KeyHex(12)));

            env(setPasskeyList(alice, passkeys), ter(tesSUCCESS));
            env.close();

            // Owner count should remain 1 (no extra reserve for update).
            BEAST_EXPECT(ownerCount(env, alice) == 1);

            auto const sle = env.le(keylet::passkeyList(alice.id()));
            BEAST_EXPECT(sle != nullptr);
        }
    }

    // =========================================================================
    // 7. testSetPasskeyListReserve
    // =========================================================================

    void
    testSetPasskeyListReserve(FeatureBitset features)
    {
        testcase("SetPasskeyList - reserve handling");

        using namespace test::jtx;

        Env env{*this, features};
        Account const alice{"alice"};

        // Fund alice just barely enough for the account reserve, but not
        // enough for the additional owner reserve needed to create a
        // PasskeyList.
        auto const acctReserve = env.current()->fees().reserve;
        auto const incReserve = env.current()->fees().increment;
        auto const baseFee = env.current()->fees().base;

        env.fund(acctReserve, alice);
        env.close();
        BEAST_EXPECT(ownerCount(env, alice) == 0);

        // Attempt to create a passkey list without enough reserve.
        {
            Json::Value passkeys(Json::arrayValue);
            passkeys.append(
                makePasskeyEntry(makePasskeyIdHex(1), makeP256KeyHex(1)));
            env(setPasskeyList(alice, passkeys),
                ter(tecINSUFFICIENT_RESERVE));
            env.close();
        }

        // Fund enough for one owner reserve plus transaction fees.
        env(jtx::pay(
                env.master, alice, drops(incReserve + 2 * baseFee)));
        env.close();

        // Now creation should succeed.
        {
            Json::Value passkeys(Json::arrayValue);
            passkeys.append(
                makePasskeyEntry(makePasskeyIdHex(1), makeP256KeyHex(1)));
            env(setPasskeyList(alice, passkeys), ter(tesSUCCESS));
            env.close();
            BEAST_EXPECT(ownerCount(env, alice) == 1);
        }

        // Delete and verify reserve is freed.
        {
            Json::Value empty(Json::arrayValue);
            env(setPasskeyList(alice, empty), ter(tesSUCCESS));
            env.close();
            BEAST_EXPECT(ownerCount(env, alice) == 0);
        }

        // Re-create should work because reserve was freed.
        {
            Json::Value passkeys(Json::arrayValue);
            passkeys.append(
                makePasskeyEntry(makePasskeyIdHex(1), makeP256KeyHex(1)));
            env(setPasskeyList(alice, passkeys), ter(tesSUCCESS));
            env.close();
            BEAST_EXPECT(ownerCount(env, alice) == 1);
        }
    }

    // =========================================================================
    // 8. testSetPasskeyListBoundary
    // =========================================================================

    void
    testSetPasskeyListBoundary(FeatureBitset features)
    {
        testcase("SetPasskeyList - boundary cases");

        using namespace test::jtx;

        Env env{*this, features};
        Account const alice{"alice"};
        env.fund(XRP(10000), alice);
        env.close();

        // -----------------------------------------------------------------
        // 8a. Exactly 10 passkeys (maximum allowed) -- should succeed.
        // -----------------------------------------------------------------
        {
            Json::Value passkeys(Json::arrayValue);
            for (unsigned i = 1; i <= 10; ++i)
            {
                passkeys.append(makePasskeyEntry(
                    makePasskeyIdHex(i), makeP256KeyHex(i)));
            }

            env(setPasskeyList(alice, passkeys), ter(tesSUCCESS));
            env.close();
            BEAST_EXPECT(ownerCount(env, alice) == 1);
        }

        // -----------------------------------------------------------------
        // 8b. Update from 10 passkeys to a single passkey.
        // -----------------------------------------------------------------
        {
            Json::Value passkeys(Json::arrayValue);
            passkeys.append(
                makePasskeyEntry(makePasskeyIdHex(99), makeP256KeyHex(99)));

            env(setPasskeyList(alice, passkeys), ter(tesSUCCESS));
            env.close();
            BEAST_EXPECT(ownerCount(env, alice) == 1);
        }

        // -----------------------------------------------------------------
        // 8c. Exactly 11 passkeys -- should fail.
        // -----------------------------------------------------------------
        {
            Json::Value passkeys(Json::arrayValue);
            for (unsigned i = 1; i <= 11; ++i)
            {
                passkeys.append(makePasskeyEntry(
                    makePasskeyIdHex(i + 100), makeP256KeyHex(i + 100)));
            }

            env(setPasskeyList(alice, passkeys), ter(temMALFORMED));
            env.close();
        }

        // -----------------------------------------------------------------
        // 8d. Exactly 0 passkeys (empty array) on existing list -- deletes.
        // -----------------------------------------------------------------
        {
            Json::Value empty(Json::arrayValue);
            env(setPasskeyList(alice, empty), ter(tesSUCCESS));
            env.close();

            auto const sle = env.le(keylet::passkeyList(alice.id()));
            BEAST_EXPECT(sle == nullptr);
            BEAST_EXPECT(ownerCount(env, alice) == 0);
        }
    }

    // =========================================================================
    // 9. testSetPasskeyListAmendment
    // =========================================================================

    void
    testSetPasskeyListAmendment(FeatureBitset features)
    {
        testcase("SetPasskeyList - amendment disabled");

        using namespace test::jtx;

        // With the featurePasskey amendment disabled, the transaction
        // should be rejected.
        Env env{*this, features - featurePasskey};
        Account const alice{"alice"};
        env.fund(XRP(10000), alice);
        env.close();

        Json::Value passkeys(Json::arrayValue);
        passkeys.append(
            makePasskeyEntry(makePasskeyIdHex(1), makeP256KeyHex(1)));

        env(setPasskeyList(alice, passkeys), ter(temDISABLED));
        env.close();

        auto const sle = env.le(keylet::passkeyList(alice.id()));
        BEAST_EXPECT(sle == nullptr);
    }

    // =========================================================================
    // run()
    // =========================================================================

public:
    void
    run() override
    {
        using namespace test::jtx;
        auto const sa = testable_amendments();

        testSetPasskeyListCreate(sa);
        testSetPasskeyListDelete(sa);
        testSetPasskeyListValidation(sa);
        testSetPasskeyListDuplicates(sa);
        testPublicKeyType(sa);
        testSetPasskeyListUpdate(sa);
        testSetPasskeyListReserve(sa);
        testSetPasskeyListBoundary(sa);
        testSetPasskeyListAmendment(sa);
    }
};

BEAST_DEFINE_TESTSUITE(PasskeyFunctionality, app, ripple);

}  // namespace test
}  // namespace xrpl
