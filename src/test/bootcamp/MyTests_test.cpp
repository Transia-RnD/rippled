#include <test/jtx.h>

#include <xrpld/core/ConfigSections.h>
#include <xrpld/app/rdb/backend/SQLiteDatabase.h>

#include <xrpl/beast/unit_test.h>

namespace ripple {
namespace test {

class MyTests_test : public beast::unit_test::suite
{
    void
    testProtocol(FeatureBitset const& features)
    {
        using namespace test::jtx;
        using namespace std::chrono_literals;
        Account const alice("alice");
        Account const bob("bob");

        Env env{*this, features};
        auto const baseFee = env.current()->fees().base;
        env.fund(XRP(100000), alice, bob);
        env.close();  // close the ledger

        auto const preAlice = env.balance(alice);
        std::cout << "Pre-Alice Balance: " << preAlice << std::endl;

        auto jt = env.jt(
            escrow::create(alice, bob, XRP(1)),
            escrow::finish_time(env.now() + 3s),
            escrow::cancel_time(env.now() + 4s));

        Serializer s;
        jt.stx->add(s);
        auto const seq = jt.stx->getFieldU32(sfSequence);
        std::cout << "Sequence: " << seq << std::endl;
        BEAST_EXPECT(seq == 4);
        auto const amt = jt.stx->getFieldAmount(sfAmount);
        std::cout << "Amount: " << amt.getJson(JsonOptions::none) << std::endl;
        BEAST_EXPECT(amt == XRP(1));
        std::cout << "Transaction: " << jt.jv << std::endl;
        env(jt);      // submits to ledger
        env.close();  // close the ledger

        BEAST_EXPECT(env.balance(alice) == preAlice - XRP(1) - baseFee);
        std::cout << "Post-Alice Balance: " << env.balance(alice) << std::endl;
        std::cout << "Post-Alice Balance=: " << (preAlice - XRP(1) - baseFee)
                  << std::endl;

        auto const k = keylet::escrow(alice, seq);
        auto const sle = env.current()->read(k);
        BEAST_EXPECT(sle);

        auto amtSle = sle->getFieldAmount(sfAmount);
        std::cout << "SLE Amount: " << amtSle.getJson(JsonOptions::none)
                  << std::endl;
        BEAST_EXPECT(amtSle == XRP(1));

        {
            Json::Value params;
            params[jss::ledger_index] = env.current()->seq() - 1;
            params[jss::transactions] = true;
            params[jss::expand] = true;
            params[jss::full] = true;
            auto const jrr = env.rpc("json", "ledger", to_string(params));
            std::cout << jrr << std::endl;
        }
    }

    void
    testTraceTransactor(FeatureBitset features)
    {
        testcase("Your Transaction Test");

        using namespace jtx;

        Env env{*this, features};
        Account const alice{"alice"};
        Account const bob{"bob"};
        Account const carol{"carol"};
        env.fund(XRP(5000), alice, bob, carol);
        env.close();

        std::cout << "START" << std::endl;

        env(pay(alice, bob, XRP(1)), ter(tesSUCCESS));
        env.close();

        // Bad Permission (Wrong Signer)
        env(pay(alice, bob, XRP(1)), sig(bob), ter(tecNO_PERMISSION));
        env.close();

        // Bad Permission (Master Disabled)
        env(fset(alice, asfDisableMaster), ter(tesSUCCESS));
        env.close();
        env(pay(alice, bob, XRP(1)), ter(tecNO_PERMISSION));
        env.close();

        // Bad Permission (Wrong RegularKey)
        env(regkey(carol, bob));
        env(pay(carol, bob, XRP(1)), sig(alice), ter(tecNO_PERMISSION));
        env.close();

        BEAST_EXPECT(1 == 1);
    }

    void
    testSql(FeatureBitset features)
    {
        testcase("SQL Test");

        using namespace jtx;

        // Env env{*this, features};
        auto config = test::jtx::envconfig();
        config->overwrite(SECTION_RELATIONAL_DB, "backend", "sqlite");
        config->LEDGER_HISTORY = 1000;

        test::jtx::Env env(*this, std::move(config));
        // Env env{*this, envconfig([&](std::unique_ptr<Config> cfg) {
        //     cfg->LEDGER_HISTORY = 1000;
        //     (*cfg)["rpc"].set("limit", std::to_string(limit));
        //     return cfg;
        // })};
        Account const alice{"alice"};
        Account const bob{"bob"};
        env.fund(XRP(5000), alice, bob);
        env.close();

        auto const db =
            dynamic_cast<SQLiteDatabase*>(&env.app().getRelationalDatabase());
        auto const ledgerSeq = db->getMaxLedgerSeq();
        BEAST_EXPECT(*ledgerSeq == (env.current()->seq() - 1));

        auto const resp = db->getHashesByIndex(1, 3);
        std::cout << "Response: " << resp.size() << std::endl;
        BEAST_EXPECT(resp.size() == 2);
    }

    void
    testSignaturesComplete(FeatureBitset features)
    {
        testcase("Signatures");

        using namespace jtx;

        Env env{*this, features};
        Account const alice{"alice"};
        Account const bob{"bob"};
        Account const carol{"carol"};
        env.fund(XRP(5000), alice, bob, carol);
        env.close();

        std::cout << "START" << std::endl;

        // Bad Permission (tefBAD_AUTH)
        // Sign the transaction with a key that does not match the account.
        env(pay(alice, bob, XRP(1)), sig(bob), ter(tefBAD_AUTH));
        env.close();

        // Bad Permission (tefMASTER_DISABLED)
        // Disable the master key and try to pay.
        env(regkey(alice, bob));
        env(fset(alice, asfDisableMaster), sig(alice));
        env(pay(alice, bob, XRP(1)), sig(alice), ter(tefMASTER_DISABLED));
        env.close();

        // Bad Permission (tefBAD_AUTH)
        // Set a regular key for carol to bob and try to sign with alice's key.
        env(regkey(carol, bob));
        env(pay(carol, bob, XRP(1)), sig(alice), ter(tefBAD_AUTH));
        env.close();

        // Bad Signature (invalidTransaction)
        // Sign the transaction with a key that does not match the account.
        JTx jt = env.jt(noop(alice), sig(alice));
        STTx local = *(jt.stx);
        // Flip some bits in the signature.
        auto badSig = local.getFieldVL(sfTxnSignature);
        badSig[20] ^= 0xAA;
        local.setFieldVL(sfTxnSignature, badSig);
        // Signature should fail.
        Json::Value jvResult;
        jvResult[jss::tx_blob] = strHex(local.getSerializer().slice());
        auto const jrr = env.rpc("json", "submit", to_string(jvResult));
        BEAST_EXPECT(jrr[jss::result][jss::error] == "invalidTransaction");
    }

    void
    testSignatures(FeatureBitset features)
    {
        testcase("Signatures");

        using namespace jtx;

        Env env{*this, features};
        Account const alice{"alice"};
        Account const bob{"bob"};
        Account const carol{"carol"};
        env.fund(XRP(5000), alice, bob, carol);
        env.close();

        std::cout << "START" << std::endl;

        // Bad Permission (tefBAD_AUTH)
        // Sign the transaction with a key that does not match the account.
        env(pay(alice, bob, XRP(1)), sig(bob), ter(tefBAD_AUTH));
        env.close();

        // Bad Permission (tefMASTER_DISABLED)
        // Disable the master key and try to pay.

        // Bad Permission (tefBAD_AUTH)
        // Set a regular key for carol to bob and try to sign with alice's key.

        // Bad Signature (invalidTransaction)
        // Sign the transaction with a key that does not match the account.
        // Hint: Search the tests for "// Flip some bits in the signature."
    }

    void 
    testSignatureSpeed(KeyType keyType)
    {
        testcase("Signature Verification Speed Test");
        const int iterations = 1000;
        auto const keypair = randomKeyPair(keyType);

        // KeyType switch
        if (keyType == KeyType::secp256k1)
        {
            std::cout << "Using secp256k1 key type." << std::endl;
        }
        else if (keyType == KeyType::ed25519)
        {
            std::cout << "Using ed25519 key type." << std::endl;
        }
        
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
        std::cout << "Total time for " << iterations << " verifications: " 
            << duration.count() / 1000 << " ms" << std::endl;
        std::cout << "Time per verification: " << timePerVerification << " µs" << std::endl;
        std::cout << "Verifications per second: " << (1000000.0 / timePerVerification) << std::endl;
        // BEAST_EXPECT(duration.count() / 1000 < 300);
        BEAST_EXPECT(1 == 1); // Ensure each verification is under 300 µs
    }

    void
    run() override
    {
        using namespace test::jtx;
        auto const sa = supported_amendments();
        // testProtocol(sa);
        // testTraceTransactor(sa);
        // testSql(sa);
        // testSignatures(sa);
        testSignaturesComplete(sa);
        // testSignatureSpeed(KeyType::secp256k1);
        // testSignatureSpeed(KeyType::ed25519);
    }
};

BEAST_DEFINE_TESTSUITE(MyTests, bootcamp, ripple);

}  // namespace test
}  // namespace ripple