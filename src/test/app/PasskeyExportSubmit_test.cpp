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

// Test submitting an Export transaction with PasskeySignature via tx_blob.
// Exercises the full WebAuthn P256 signing + challenge validation path.

#include <test/jtx.h>

#include <xrpl/basics/StringUtilities.h>
#include <xrpl/basics/base64.h>
#include <xrpl/json/json_reader.h>
#include <xrpl/basics/strHex.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/KeyType.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/STObject.h>
#include <xrpl/protocol/SecretKey.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/Sign.h>
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/protocol/digest.h>
#include <xrpl/protocol/jss.h>

namespace xrpl {
namespace test {

struct PasskeyExportSubmit_test : public beast::unit_test::suite
{
    // =========================================================================
    // Helpers
    // =========================================================================

    // base64url encode (RFC 4648 §5): + → -, / → _, strip padding
    static std::string
    base64url_encode(std::uint8_t const* data, std::size_t len)
    {
        std::string b64 = base64_encode(data, len);
        for (auto& c : b64)
        {
            if (c == '+')
                c = '-';
            else if (c == '/')
                c = '_';
        }
        // Strip trailing '='
        while (!b64.empty() && b64.back() == '=')
            b64.pop_back();
        return b64;
    }

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

    // =========================================================================
    // TEST: Submit Export with PasskeySignature via tx_blob
    // =========================================================================

    void
    testExportWithPasskeySignature(FeatureBitset features)
    {
        testcase("Export with PasskeySignature submitted via tx_blob");
        using namespace test::jtx;

        Env env{*this, features};

        // 1. Create a P256 account and a destination account
        Account const alice{"alice", KeyType::p256};
        Account const bob{"bob"};
        env.fund(XRP(1000000), alice, bob);
        env.close();

        // 2. Set up passkey list for alice with her P256 public key
        auto const passkeyId = strHex(
            Slice(alice.pk().data(), 20));  // First 20 bytes as credential ID

        Json::Value passkeys(Json::arrayValue);
        Json::Value wrapper;
        auto& entry = wrapper[sfPasskey.jsonName];
        entry[sfPasskeyID.jsonName] = passkeyId;
        entry[sfPublicKey.jsonName] = strHex(alice.pk());
        passkeys.append(wrapper);

        env(setPasskeyList(alice, passkeys), sig(alice));
        env.close();

        // Verify passkey list was created
        auto const passkeyListSle =
            env.le(keylet::passkeyList(alice.id()));
        BEAST_EXPECT(passkeyListSle != nullptr);

        // 3. Build the Export transaction as an STTx
        auto const seq = env.seq(alice);
        auto const fee = env.current()->fees().base;
        auto const lastLedger = env.current()->seq() + 10;

        STTx exportTx(ttEXPORT, [&](auto& obj) {
            obj.setAccountID(sfAccount, alice.id());
            obj.setAccountID(sfDestination, bob.id());
            obj.setFieldAmount(sfAmount, XRP(100));
            obj.setFieldU32(sfSequence, seq);
            obj.setFieldAmount(sfFee, fee);
            obj.setFieldU32(sfLastLedgerSequence, lastLedger);
            // Set the P256 public key as SigningPubKey
            obj.setFieldVL(
                sfSigningPubKey,
                Blob(alice.pk().data(), alice.pk().data() + alice.pk().size()));
        });

        // 4. Get the signing data (this is what the challenge must match)
        Serializer s;
        s.add32(HashPrefix::txSign);
        exportTx.addWithoutSigningFields(s);
        auto const signingData = s.getData();

        // 5. Build authenticatorData (RP ID hash + flags + sign count)
        // Standard WebAuthn authenticator data: 37 bytes minimum
        //   - 32 bytes: SHA-256 of RP ID
        //   - 1 byte: flags (0x1D = UP + UV + AT + ED)
        //   - 4 bytes: sign count
        Blob authenticatorData;
        auto const rpIdHash = sha256(makeSlice(std::string("localhost")));
        authenticatorData.insert(
            authenticatorData.end(),
            rpIdHash.data(),
            rpIdHash.data() + rpIdHash.size());
        authenticatorData.push_back(0x1D);  // flags
        // sign count = 0
        authenticatorData.push_back(0x00);
        authenticatorData.push_back(0x00);
        authenticatorData.push_back(0x00);
        authenticatorData.push_back(0x00);

        // 6. Build clientDataJSON with challenge = base64url(signingData)
        auto const challengeB64 = base64url_encode(
            reinterpret_cast<std::uint8_t const*>(signingData.data()),
            signingData.size());

        std::string clientDataJSON =
            "{\"type\":\"webauthn.get\","
            "\"challenge\":\"" +
            challengeB64 +
            "\","
            "\"origin\":\"http://localhost:3002\","
            "\"crossOrigin\":false}";

        // 7. Compute WebAuthn signing data:
        //    signingData = authenticatorData || SHA256(clientDataJSON)
        auto const clientDataHash = sha256(makeSlice(clientDataJSON));

        Blob webauthnSignData(
            authenticatorData.begin(), authenticatorData.end());
        webauthnSignData.insert(
            webauthnSignData.end(),
            clientDataHash.data(),
            clientDataHash.data() + clientDataHash.size());

        // 8. Sign with the P256 secret key
        //    sign() internally does SHA256(message) then ECDSA_sign
        auto const signature = sign(alice.pk(), alice.sk(), makeSlice(webauthnSignData));

        // Debug: verify the signature locally before submitting
        bool localVerify = verify(
            alice.pk(),
            makeSlice(webauthnSignData),
            Slice(signature.data(), signature.size()));
        log << "Local P256 verify: " << (localVerify ? "PASS" : "FAIL") << std::endl;
        log << "SigningPubKey type: " << static_cast<int>(*publicKeyType(alice.pk())) << std::endl;
        log << "SigningPubKey hex: " << strHex(alice.pk()) << std::endl;
        log << "Signature hex: " << strHex(Slice(signature.data(), signature.size())) << std::endl;
        log << "webauthnSignData size: " << webauthnSignData.size() << std::endl;
        log << "signingData size: " << signingData.size() << std::endl;
        log << "challengeB64: " << challengeB64 << std::endl;
        log << "clientDataJSON: " << clientDataJSON << std::endl;
        BEAST_EXPECT(localVerify);

        // 9. Build the PasskeySignature object and attach to the STTx
        STObject passkeySignature(sfPasskeySignature);
        passkeySignature.setFieldVL(
            sfPasskeyID,
            *strUnHex(passkeyId));
        passkeySignature.setFieldVL(
            sfAuthenticatorData, authenticatorData);
        passkeySignature.setFieldVL(
            sfClientDataJSON,
            Blob(clientDataJSON.begin(), clientDataJSON.end()));
        passkeySignature.setFieldVL(
            sfSignature,
            Blob(signature.data(), signature.data() + signature.size()));

        exportTx.setFieldObject(sfPasskeySignature, passkeySignature);

        // Debug: verify signing data is stable after adding PasskeySignature
        {
            Serializer s2;
            s2.add32(HashPrefix::txSign);
            exportTx.addWithoutSigningFields(s2);
            auto const signingData2 = s2.getData();
            log << "signingData2 size (after adding PasskeySignature): "
                << signingData2.size() << std::endl;
            bool signingDataMatch = (signingData == signingData2);
            log << "Signing data stable after setFieldObject: "
                << (signingDataMatch ? "YES" : "NO") << std::endl;
            if (!signingDataMatch)
            {
                log << "signingData  hex: " << strHex(signingData) << std::endl;
                log << "signingData2 hex: " << strHex(signingData2) << std::endl;
            }
            BEAST_EXPECT(signingDataMatch);
        }

        // Debug: roundtrip through serialization
        {
            auto const blob = exportTx.getSerializer().getData();
            SerialIter sit(makeSlice(blob));
            // Parse fields manually to avoid signature check in STTx ctor
            STObject parsed(sit, sfGeneric);
            Serializer s3;
            s3.add32(HashPrefix::txSign);
            parsed.addWithoutSigningFields(s3);
            auto const signingData3 = s3.getData();
            log << "signingData3 size (after roundtrip): "
                << signingData3.size() << std::endl;
            bool roundtripMatch = (signingData == signingData3);
            log << "Signing data matches after serialization roundtrip: "
                << (roundtripMatch ? "YES" : "NO") << std::endl;
            if (!roundtripMatch)
            {
                log << "signingData  hex: " << strHex(signingData) << std::endl;
                log << "signingData3 hex: " << strHex(signingData3) << std::endl;
            }
            BEAST_EXPECT(roundtripMatch);
        }

        // 10. Serialize to tx_blob and submit via RPC
        auto const txBlob = strHex(exportTx.getSerializer().slice());
        log << "tx_blob: " << txBlob << std::endl;

        Json::Value jvResult;
        jvResult[jss::tx_blob] = txBlob;
        auto const result =
            env.rpc("json", "submit", to_string(jvResult));

        // 11. Verify the result
        auto const& res = result[jss::result];
        log << "Submit result: " << to_string(res) << std::endl;
        BEAST_EXPECT(
            res[jss::status] == "success");
        BEAST_EXPECT(
            res[jss::engine_result] == "tesSUCCESS");

        // Close ledger and verify the export happened
        env.close();

        // Verify ExportSequence incremented
        auto const sle = env.le(keylet::account(alice.id()));
        BEAST_EXPECT(sle);
        if (sle)
        {
            BEAST_EXPECT(sle->isFieldPresent(sfExportSequence));
            BEAST_EXPECT(sle->getFieldU32(sfExportSequence) == 1);
        }
    }

    // =========================================================================
    // TEST: Submit raw tx_blob (hardcoded values for manual testing)
    // =========================================================================

    void
    testRawTxBlobSubmit(FeatureBitset features)
    {
        testcase("Submit raw tx_blob to ledger");
        using namespace test::jtx;

        Env env{*this, features};

        // This test demonstrates submitting a pre-built tx_blob.
        // Replace the hex string below with your actual tx_blob.
        //
        // NOTE: This will fail with "Invalid signature" unless the
        // account exists in the test ledger with matching sequence,
        // fee, and LastLedgerSequence values.

        std::string const txBlob =
            "120063210000543D2200000000240000245B201B00002B72"
            "6140000000000F424068400000000000000C7341F67B8531"
            "E98C49759E0980EFC80F1F2684F05B53365C2ABCA55C3B1E"
            "8C8AF1C04AEE68389B8293D732089EF0680391CA14D12B44"
            "503A7BEAC356D2F3E06BCF88E18114257BFE2D434767360D"
            "421C0F9DE5D7B77DD051DA83144A9BA0DB663FC30D646967"
            "105DE78176C96AFCB7E028764630440220652DF4AAC6BABE"
            "07CC9376B879E1BCF80A281303CBB9757E4493436B52BA60"
            "4D022027AFDD71ABA64B8C4F265E67F642AEAADD47E519AE"
            "78AD3809DFB17BBDA54EA5702114FCBA95C783E4986758C9"
            "A8F068B914A03AF5D57370222549960DE5880E8C68743417"
            "0F6476605B8FE4AEB9A28632C7995CF3BA831D97631D0000"
            "00007023C16C7B2274797065223A22776562617574686E2E"
            "676574222C226368616C6C656E6765223A22553154594142"
            "49415979454141465139496741414141416B4141416B5779"
            "416241414172636D4641414141414141394351476841414141"
            "414141414144484E42396E75464D656D4D535857654359447"
            "6794138664A6F547757314D325843713870567737486F794B"
            "3863424B376D67346D344B54317A49496E76426F4135484B"
            "464E457252464136652D724456744C7A34477650694F4742"
            "464356375F69314452326332445549634435336C31376439"
            "304648616778524B6D3644625A6A5F44445752705A784264"
            "3534463279577238747722"
            "2C226F726967696E223A22687474703A2F2F6C6F63616C68"
            "6F73743A33303032222C2263726F73734F726967696E223A"
            "66616C73657DE1";

        // ---- Diagnostic: parse the tx_blob and compare signing data vs challenge ----
        auto const blobBytes = strUnHex(txBlob);
        BEAST_EXPECT(blobBytes.has_value());
        if (blobBytes)
        {
            SerialIter sit(makeSlice(*blobBytes));
            STObject parsed(sit, sfGeneric);

            // Compute signing data (what the server would compute)
            Serializer s;
            s.add32(HashPrefix::txSign);
            parsed.addWithoutSigningFields(s);
            auto const serverSigningData = s.getData();
            log << "Server signing data hex (first 20 bytes): "
                << strHex(Slice(serverSigningData.data(),
                    std::min<size_t>(serverSigningData.size(), 20)))
                << std::endl;
            log << "Server signing data size: " << serverSigningData.size() << std::endl;

            // Extract the challenge from clientDataJSON
            if (parsed.isFieldPresent(sfPasskeySignature))
            {
                auto const& pks = static_cast<STObject const&>(
                    parsed.peekAtField(sfPasskeySignature));
                auto const clientDataJSON = pks.getFieldVL(sfClientDataJSON);
                std::string const cdj(clientDataJSON.begin(), clientDataJSON.end());
                log << "clientDataJSON: " << cdj << std::endl;

                Json::Value cdjParsed;
                Json::Reader jsonReader;
                if (jsonReader.parse(cdj, cdjParsed))
                {
                    std::string challenge = cdjParsed["challenge"].asString();
                    log << "challenge (base64url): " << challenge << std::endl;

                    // Decode challenge
                    for (auto& c : challenge) {
                        if (c == '-') c = '+';
                        else if (c == '_') c = '/';
                    }
                    while (challenge.size() % 4 != 0)
                        challenge += '=';
                    auto const decoded = base64_decode(challenge);
                    log << "challenge decoded size: " << decoded.size() << std::endl;
                    log << "challenge decoded hex (first 20 bytes): "
                        << strHex(Slice(
                            reinterpret_cast<uint8_t const*>(decoded.data()),
                            std::min<size_t>(decoded.size(), 20)))
                        << std::endl;

                    // Compare byte by byte
                    if (decoded.size() == serverSigningData.size())
                    {
                        bool match = true;
                        for (size_t i = 0; i < decoded.size(); ++i)
                        {
                            if (static_cast<uint8_t>(decoded[i]) != serverSigningData[i])
                            {
                                log << "MISMATCH at byte " << i
                                    << ": challenge=0x" << strHex(Slice(
                                        reinterpret_cast<uint8_t const*>(&decoded[i]), 1))
                                    << " server=0x" << strHex(Slice(&serverSigningData[i], 1))
                                    << std::endl;
                                match = false;
                                if (i > 5) break;  // show first few mismatches
                            }
                        }
                        if (match)
                            log << "Challenge matches server signing data!" << std::endl;
                    }
                    else
                    {
                        log << "Size mismatch: challenge=" << decoded.size()
                            << " server=" << serverSigningData.size() << std::endl;
                    }
                }
            }
        }

        // Submit and check result
        Json::Value jvResult;
        jvResult[jss::tx_blob] = txBlob;
        auto const result =
            env.rpc("json", "submit", to_string(jvResult));

        auto const& res = result[jss::result];
        BEAST_EXPECT(res.isMember(jss::error) || res.isMember(jss::engine_result));
        log << "Raw tx_blob submit result: " << to_string(res) << std::endl;
    }

public:
    void
    run() override
    {
        using namespace test::jtx;
        auto const sa = testable_amendments();

        testExportWithPasskeySignature(sa);
        testRawTxBlobSubmit(sa);
    }
};

BEAST_DEFINE_TESTSUITE(PasskeyExportSubmit, app, ripple);

}  // namespace test
}  // namespace xrpl
