//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2023 XRPL-Labs.

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

#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/jss.h>
#include <test/jtx.h>

namespace ripple {
namespace test {
class Wildcard_test : public beast::unit_test::suite
{
    std::unique_ptr<Config>
    makeNetworkConfig(uint32_t networkID)
    {
        using namespace jtx;
        return envconfig([&](std::unique_ptr<Config> cfg) {
            cfg->NETWORK_ID = networkID;
            Section config;
            config.append(
                {"reference_fee = 10",
                 "account_reserve = 1000000",
                 "owner_reserve = 200000"});
            auto setup = setup_FeeVote(config);
            cfg->FEES = setup;
            return cfg;
        });
    }

    void
    testWildcardSign(FeatureBitset features)
    {
        using namespace test::jtx;

        testcase("wildcard sign");

        for (bool const wildcardNetwork : {true, false})
        {
            auto const network = wildcardNetwork ? 65535 : 21337;
            Env env{*this, makeNetworkConfig(network), features};
            Account const alice{"alice"};
            Account const bob{"bob"};
            Account const carol{"carol"};
            Account const dave{"dave"};
            env.fund(XRP(1000), alice, bob, carol, dave);
            env.close();

            Json::Value jv;
            jv[jss::Account] = alice.human();
            jv[jss::Destination] = bob.human();
            jv[jss::TransactionType] = "Payment";
            jv[jss::Amount] = "1000000";

            // lambda that submits an STTx and returns the resulting JSON.
            auto submitSTTx = [&env](STTx const& stx) {
                Json::Value jvResult;
                jvResult[jss::tx_blob] = strHex(stx.getSerializer().slice());
                return env.rpc("json", "submit", to_string(jvResult));
            };

            // Account/RegularKey Sign
            {
                JTx tx = env.jt(jv, sig(bob));
                STTx local = *(tx.stx);
                auto const info = submitSTTx(local);
                auto const tecResult =
                    wildcardNetwork ? "tesSUCCESS" : "tefBAD_AUTH";
                BEAST_EXPECT(
                    info[jss::result][jss::engine_result] == tecResult);
            }

            // Multi Sign
            {
                env(signers(alice, 1, {{bob, 1}, {carol, 1}}));
                env.close();

                JTx tx = env.jt(jv, msig(dave), fee(XRP(1)));
                STTx local = *(tx.stx);
                auto const info = submitSTTx(local);
                auto const tecResult =
                    wildcardNetwork ? "tesSUCCESS" : "tefBAD_SIGNATURE";
                BEAST_EXPECT(
                    info[jss::result][jss::engine_result] == tecResult);
            }
        }
    }

    void
    testSimplePayment(FeatureBitset features)
    {
        using namespace test::jtx;

        testcase("simple payment");

        Env env{*this, envconfig(), features};
        Account const alice{"alice"};
        Account const bob{"bob"};
        Account const carol{"carol"};
        Account const dave{"dave", KeyType::dilithium};
        env.fund(XRP(1000), alice, bob, carol, dave);
        env.close();

        env(fset(dave, asfForceQuantum));
        env(pay(dave, bob, XRP(100)));
        env.close();

        Json::Value params;
        params[jss::ledger_index] = env.current()->seq() - 1;
        params[jss::transactions] = true;
        params[jss::expand] = true;
        auto const jrr = env.rpc("json", "ledger", to_string(params));
        std::cout << jrr << std::endl;
    }

    void 
    testSignatureSpeed(KeyType keyType)
    {
        testcase("Signature Verification Speed Test");
        const int iterations = 10000;
        auto const keypair = randomKeyPair(keyType);

        // KeyType switch
        
        // Create a transaction and sign it
        STTx tx(ttACCOUNT_SET, [&keypair](auto& obj) {
            obj.setAccountID(sfAccount, calcAccountID(keypair.first));
            obj.setFieldVL(sfMessageKey, keypair.first.slice());
            obj.setFieldVL(sfSigningPubKey, keypair.first.slice());
        });
        
        tx.sign(keypair.first, keypair.second);
        std::unordered_set<uint256, beast::uhash<>> const presets;
        Rules const defaultRules{presets};
        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < iterations; ++i)
        {
            auto result = tx.checkSign(STTx::RequireFullyCanonicalSig::yes, defaultRules);
            if (!result)
            {
                std::cout << "Signature verification failed on iteration " << i << std::endl;
                break;
            }
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        double timePerVerification = static_cast<double>(duration.count()) / iterations;
        if (keyType == KeyType::secp256k1)
        {
            std::cout << "KeyType::secp256k1: Total time for " << iterations << " verifications: " << duration.count() / 1000 << " ms" << std::endl;
            std::cout << "KeyType::secp256k1: Time per verification: " << timePerVerification << " µs" << std::endl;
            std::cout << "KeyType::secp256k1: Verifications per second: " << (1000000.0 / timePerVerification) << std::endl;
        }
        else if (keyType == KeyType::ed25519)
        {
            std::cout << "KeyType::ed25519: Total time for " << iterations << " verifications: " << duration.count() / 1000 << " ms" << std::endl;
            std::cout << "KeyType::ed25519: Time per verification: " << timePerVerification << " µs" << std::endl;
            std::cout << "KeyType::ed25519: Verifications per second: " << (1000000.0 / timePerVerification) << std::endl;
        }
        else if (keyType == KeyType::dilithium)
        {
            std::cout << "KeyType::dilithium: Total time for " << iterations << " verifications: " << duration.count() / 1000 << " ms" << std::endl;
            std::cout << "KeyType::dilithium: Time per verification: " << timePerVerification << " µs" << std::endl;
            std::cout << "KeyType::dilithium: Verifications per second: " << (1000000.0 / timePerVerification) << std::endl;
        }
        BEAST_EXPECT(1 == 1);
    }

    void
    testQuantum(FeatureBitset features)
    {
        using namespace test::jtx;

        testcase("quantum");

        Env env{*this, envconfig(), features};
        Account const alice{"alice"};
        Account const bob{"bob"};
        Account const carol{"carol"};
        Account const dave{"dave", KeyType::dilithium};
        env.fund(XRP(1000), alice, bob, carol, dave);
        env.close();

        Json::Value jv;
        jv[sfAccount.jsonName] = alice.human();
        jv[sfQuantumPublicKey.jsonName] = strHex(dave.pk().slice());
        jv[sfTransactionType.jsonName] = jss::SetQuantumKey;

        env(jv);
        env.close();

        Json::Value params;
        params[jss::ledger_index] = env.current()->seq() - 1;
        params[jss::transactions] = true;
        params[jss::expand] = true;
        auto const jrr = env.rpc("json", "ledger", to_string(params));
        std::cout << jrr << std::endl;
    }

    void
    testWithFeats(FeatureBitset features)
    {
        // testWildcardSign(features);
        // testSimplePayment(features);
        // testSignatureSpeed(KeyType::secp256k1);
        // testSignatureSpeed(KeyType::ed25519);
        // testSignatureSpeed(KeyType::dilithium);
        testQuantum(features);
    }

public:
    void
    run() override
    {
        using namespace test::jtx;
        auto const sa = supported_amendments();
        testWithFeats(sa);
    }
};

BEAST_DEFINE_TESTSUITE(Wildcard, app, ripple);
}  // namespace test
}  // namespace ripple
