//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2023 XRPLF

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

#include <ripple/app/main/BasicApp.h>
#include <ripple/app/misc/ValidatorList.h>
#include <ripple/app/misc/ValidatorSite.h>
#include <ripple/core/ConfigSections.h>
#include <ripple/json/json_reader.h>
#include <ripple/json/json_writer.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/jss.h>
#include <test/jtx.h>
#include <test/jtx/TrustedPublisherServer.h>

namespace ripple {
namespace test {

class GenerateXPOP_test : public beast::unit_test::suite
{
    static Json::Value
    makeXpop(std::string strJson)
    {
        Json::Value xpop;
        Json::Reader reader;
        reader.parse(strJson, xpop);
        return xpop;
    }

    static Json::Value
    loadXpop(std::string xpopStr)
    {
        std::string fn = "xpop/" + xpopStr;
        try
        {
            // check if file exists and is not empty
            if (!std::filesystem::exists(fn) ||
                std::filesystem::file_size(fn) == 0)
            {
                std::cout << "file was zero size or didn't exist"
                          << "\n";
                return {};
            }

            // open file and read its content
            std::ifstream inFile(fn, std::ios::in | std::ios::binary);
            if (inFile)
            {
                std::string content(
                    (std::istreambuf_iterator<char>(inFile)),
                    std::istreambuf_iterator<char>());

                return makeXpop(content);
            }
            else
            {
                std::cout << "failed to open file"
                          << "\n";
                return {};
            }
        }
        catch (std::filesystem::filesystem_error& e)
        {
            std::cout << "Failed to load file " + fn + " (" + e.what() + ")";
            return {};
        }
        catch (std::runtime_error& e)
        {
            std::cout << e.what();
            return {};
        }
    }

    std::unique_ptr<Config>
    staticVLConfig()
    {
        using namespace jtx;
        using namespace std::string_literals;

        std::set<std::string> const keys = {
            "n949f75evCHwgyP4fPVgaHqNHxUVN15PsJEZ3B3HnXPcPjcZAoy7",
            "n9MD5h24qrQqiyBC8aeqqCWvpiBiYQ3jxSr91uiDvmrkyHRdYLUj"};
        return envconfig([&](std::unique_ptr<Config> cfg) {
            for (auto const& key : keys)
                cfg->section(SECTION_VALIDATORS).append(key);
            cfg->XPOP_DIR = "xpop";
            // cfg->section(SECTION_RPC_STARTUP).append(
            // "{ \"command\": \"log_level\", \"severity\": \"debug\" "
            // "}");
            return cfg;
        });
    }

    void
    testStaticUNL(FeatureBitset features)
    {
        testcase("static unl");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, staticVLConfig()};

        auto const alice = Account("alice");
        auto const bob = Account("bob");
        env.fund(XRP(1000), alice, bob);

        Json::Value jv;
        jv[jss::Account] = alice.human();
        jv[jss::TransactionType] = jss::AccountSet;
        jv[sfOperationLimit.jsonName] = to_string(21337);
        env(jv, fee(1000), ter(tesSUCCESS));
        env.close();
        std::string txHash =
            boost::lexical_cast<std::string>(env.tx()->getTransactionID());
        env.close();
        auto const xpopJson = loadXpop(txHash);
        BEAST_EXPECT(
            xpopJson[jss::transaction][jss::blob] ==
            "1200032400000004201D000053596840000000000003E873210388935426E0D080"
            "83314842EDFBB2D517BD47699F9A4527318A8E10468C97C052744730450221009D"
            "FEE065335CEEB6A4129BC96F4B4F8BF0294378A95560D20DEFD0483087FC2A0220"
            "71F931EA89DA8362E0B7CA5F5D99150CBD0445CBD2E0F61CCA1BBA28FE1D043081"
            "14AE123A8556F3CF91154711376AFB0F894F832B3D");
        BEAST_EXPECT(
            xpopJson[jss::transaction][jss::meta] ==
            "201C00000003F8E51100612500000003559CE54C3B934E473A995B477E92EC229F"
            "99CED5B62BF4D2ACE4DC42719103AE2F5692FA6A9FC8EA6018D5D16532D7795C91"
            "BFB0831355BDFDA177E86C8BF997985FE6240000000462400000003B9ACA00E1E7"
            "220080000024000000052D0000000062400000003B9AC6188114AE123A8556F3CF"
            "91154711376AFB0F894F832B3DE1E1F1031000");
        BEAST_EXPECT(xpopJson[jss::validation][jss::unl][jss::blob] == "");
        BEAST_EXPECT(xpopJson[jss::validation][jss::unl][jss::manifest] == "");
        BEAST_EXPECT(
            xpopJson[jss::validation][jss::unl][jss::public_key] == "");
        BEAST_EXPECT(xpopJson[jss::validation][jss::unl][jss::signature] == "");
        BEAST_EXPECT(xpopJson[jss::validation][jss::unl][jss::version] == 0);
    }

    void
    testInvalid(FeatureBitset features)
    {
        testcase("invalid");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, staticVLConfig()};

        auto const alice = Account("alice");
        env.fund(XRP(200), alice);
        env.close();

        // Account Set Min
        {
            Json::Value jv;
            jv[jss::Account] = alice.human();
            jv[jss::TransactionType] = jss::AccountSet;
            env(jv, fee(10), ter(tesSUCCESS));
            env.close();
            std::string txHash =
                boost::lexical_cast<std::string>(env.tx()->getTransactionID());
            env.close();
            auto const xpopJson = loadXpop(txHash);
        }
    }

    void
    testAccountSet(FeatureBitset features)
    {
        testcase("account set");

        using namespace test::jtx;
        using namespace std::literals;

        auto const totalCoins = drops(100'000'000'000'000'000);

        // Account Set Min
        {
            test::jtx::Env env{*this, staticVLConfig()};
            auto const alice = Account("alice");
            env.fund(XRP(200), alice);
            env.close();

            // burn 100,000 xrp
            auto const master = Account("masterpassphrase");
            auto const burnAmount = drops(100'000);
            env(noop(master), fee(burnAmount), ter(tesSUCCESS));
            env.close();

            // confirm burn + fee
            auto const feeDrops = env.current()->fees().base;
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == totalCoins - burnAmount - (2 *
            feeDrops));

            // account set tx
            auto const burnFee = drops(10);
            Json::Value jv;
            jv[jss::Account] = alice.human();
            jv[jss::TransactionType] = jss::AccountSet;
            jv[sfOperationLimit.jsonName] = to_string(21337);
            env(jv, fee(burnFee), ter(tesSUCCESS));
            env.close();

            // confirm xpop generated
            std::string txHash =
                boost::lexical_cast<std::string>(env.tx()->getTransactionID());
            env.close();
            auto const xpopJson = loadXpop(txHash);
            BEAST_EXPECT(
                xpopJson[jss::transaction][jss::blob] ==
                "1200032400000004201D0000535968400000000000000A73210388935426E0"
                "D08083314842EDFBB2D517BD47699F9A4527318A8E10468C97C05274463044"
                "02201274F31DFEC9F84FF6BDEFED152D3E22C9FC2E4C71C92FF16F82EB59FD"
                "1C9DC202201D86B9274DA0CAFA78D2A55917C29D755C5C0C453AA0B022E282"
                "4539381C757B8114AE123A8556F3CF91154711376AFB0F894F832B3D");
            BEAST_EXPECT(
                xpopJson[jss::transaction][jss::meta] ==
                "201C00000000F8E51100612500000003559CE54C3B934E473A995B477E92EC"
                "229F99CED5B62BF4D2ACE4DC42719103AE2F5692FA6A9FC8EA6018D5D16532"
                "D7795C91BFB0831355BDFDA177E86C8BF997985FE624000000046241634577"
                "F2402E00E1E7220080000024000000052D000000006241634577F2402DF681"
                "14AE123A8556F3CF91154711376AFB0F894F832B3DE1E1F1031000");

            // confirm coins burned
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == preCoins - burnFee);
        }
        // Account Set Max
        {
            test::jtx::Env env{*this, staticVLConfig()};
            auto const alice = Account("alice");
            env.fund(XRP(200), alice);
            env.close();

            // burn 100,000 xrp
            auto const master = Account("masterpassphrase");
            auto const burnAmount = drops(100'000);
            env(noop(master), fee(burnAmount), ter(tesSUCCESS));
            env.close();

            // transfer balance to alice
            auto const masterBal = env.balance(master);
            env(pay(master, alice, XRP(99'999'998'000)));
            env.close();

            // confirm burn + fee
            auto const feeDrops = env.current()->fees().base;
            auto const preCoins = env.current()->info().drops;
            BEAST_EXPECT(preCoins == totalCoins - burnAmount - (3 * feeDrops));

            // account set tx
            Json::Value jv;
            auto const burnFee = drops(99'000'000'000'000'000);
            jv[jss::Account] = alice.human();
            jv[jss::TransactionType] = jss::AccountSet;
            jv[sfOperationLimit.jsonName] = to_string(21337);
            env(jv, fee(burnFee), ter(tesSUCCESS));
            env.close();
            std::string txHash =
                boost::lexical_cast<std::string>(env.tx()->getTransactionID());
            env.close();
            auto const xpopJson = loadXpop(txHash);
            BEAST_EXPECT(
                xpopJson[jss::transaction][jss::blob] ==
                "1200032400000004201D0000535968415FB7F9B8C3800073210388935426E0"
                "D08083314842EDFBB2D517BD47699F9A4527318A8E10468C97C05274463044"
                "022076183539FABE6F1E318AA46BF556E5B63AD09B82061138CF364EA348CD"
                "B6A2F3022073D191C8A638DEBBEEB86D863F1D493C3A0DE36EB08FEDFC23C7"
                "593771A45BB88114AE123A8556F3CF91154711376AFB0F894F832B3D");
            BEAST_EXPECT(
                xpopJson[jss::transaction][jss::meta] ==
                "201C00000000F8E51100612500000005550F5633D3EF0C3BCB8BCCA10A0659"
                "81DA925427748FC3CA7F5B24B2891C1B07F55692FA6A9FC8EA6018D5D16532"
                "D7795C91BFB0831355BDFDA177E86C8BF997985FE624000000046241634577"
                "F2402E00E1E7220080000024000000052D000000006240038D7E397CAE0081"
                "14AE123A8556F3CF91154711376AFB0F894F832B3DE1E1F1031000");

            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == preCoins - burnFee);
        }
    }

    void
    testSetRegularKey(FeatureBitset features)
    {
        testcase("set regular key");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, staticVLConfig()};

        auto const totalCoins = drops(100'000'000'000'000'000);

        auto const alice = Account("alice");
        env.fund(XRP(1000), alice);
        env.close();

        // burn 100,000 xrp
        auto const master = Account("masterpassphrase");
        auto const burnAmount = drops(100'000);
        env(noop(master), fee(burnAmount), ter(tesSUCCESS));
        env.close();

        // confirm burn + fee
        auto const feeDrops = env.current()->fees().base;
        auto const preCoins = env.current()->info().drops;
        BEAST_EXPECT(preCoins == totalCoins - burnAmount - (2 * feeDrops));

        {
            auto jv = regkey(alice, disabled);
            jv[sfOperationLimit.jsonName] = to_string(21337);
            env(jv);
            env.close();
            std::string txHash =
                boost::lexical_cast<std::string>(env.tx()->getTransactionID());
            env.close();
            auto const xpopJson = loadXpop(txHash);
            BEAST_EXPECT(
                xpopJson[jss::transaction][jss::blob] ==
                "1200052400000004201D0000535968400000000000000A73210388935426E0"
                "D08083314842EDFBB2D517BD47699F9A4527318A8E10468C97C05274463044"
                "02202DE5A067CC28E2ADE6FD25505097E5B264B3F6CD7F05710F48EA07A901"
                "5CA94C0220221F81F06CDC507F78E87C072302D3FBA26D4AA800200958CC22"
                "498483140BDD8114AE123A8556F3CF91154711376AFB0F894F832B3D");
            BEAST_EXPECT(
                xpopJson[jss::transaction][jss::meta] ==
                "201C00000000F8E51100612500000003559CE54C3B934E473A995B477E92EC"
                "229F99CED5B62BF4D2ACE4DC42719103AE2F5692FA6A9FC8EA6018D5D16532"
                "D7795C91BFB0831355BDFDA177E86C8BF997985FE622008000002400000004"
                "62400000003B9ACA00E1E7220081000024000000052D000000006240000000"
                "3B9AC9F68114AE123A8556F3CF91154711376AFB0F894F832B3DE1E1F10310"
                "00");

            // confirm coins burned
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == preCoins - feeDrops);
        }
    }

    void
    testSignersListSet(FeatureBitset features)
    {
        testcase("signers list set");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, staticVLConfig()};

        auto const totalCoins = drops(100'000'000'000'000'000);

        auto const alice = Account("alice");
        auto const bob = Account("bob");
        env.fund(XRP(1000), alice, bob);
        env.close();

        // burn 100,000 xrp
        auto const master = Account("masterpassphrase");
        auto const burnAmount = drops(100'000);
        env(noop(master), fee(burnAmount), ter(tesSUCCESS));
        env.close();

        // confirm burn + fee
        auto const feeDrops = env.current()->fees().base;
        auto const preCoins = env.current()->info().drops;
        BEAST_EXPECT(preCoins == totalCoins - burnAmount - (4 * feeDrops));

        {
            auto jv = signers(alice, 1, {{bob, 1}});
            jv[sfOperationLimit.jsonName] = to_string(21337);
            env(jv);
            env.close();
            std::string txHash =
                boost::lexical_cast<std::string>(env.tx()->getTransactionID());
            env.close();
            auto const xpopJson = loadXpop(txHash);
            BEAST_EXPECT(
                xpopJson[jss::transaction][jss::blob] ==
                "12000C2400000004201D0000535920230000000168400000000000000A7321"
                "0388935426E0D08083314842EDFBB2D517BD47699F9A4527318A8E10468C97"
                "C05274463044022025C63B9BE7A1F058BDF77FCBD41C6D2255419DC69783F2"
                "D9D84B967D5A45753402204CF2C5DC124E61CCDD02F948807F008DAF536A66"
                "11C78004C68B600B0EBC6E248114AE123A8556F3CF91154711376AFB0F894F"
                "832B3DF4EB1300018114F51DFC2A09D62CBBA1DFBDD4691DAC96AD98B90FE1"
                "F1");
            BEAST_EXPECT(
                xpopJson[jss::transaction][jss::meta] ==
                "201C00000000F8E311005356472CD116F1449F280243169C442271168E3687"
                "50479CC7B20816170EDBDCA4E6E82200010000202300000001F4EB13000181"
                "14F51DFC2A09D62CBBA1DFBDD4691DAC96AD98B90FE1F1E1E1E51100612500"
                "000003559CE54C3B934E473A995B477E92EC229F99CED5B62BF4D2ACE4DC42"
                "719103AE2F5692FA6A9FC8EA6018D5D16532D7795C91BFB0831355BDFDA177"
                "E86C8BF997985FE624000000042D0000000062400000003B9ACA00E1E72200"
                "80000024000000052D0000000162400000003B9AC9F68114AE123A8556F3CF"
                "91154711376AFB0F894F832B3DE1E1E311006456A33EC6BB85FB5674074C4A"
                "3A43373BB17645308F3EAE1933E3E35252162B217DE858A33EC6BB85FB5674"
                "074C4A3A43373BB17645308F3EAE1933E3E35252162B217D8214AE123A8556"
                "F3CF91154711376AFB0F894F832B3DE1E1F1031000");

            // confirm coins burned
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(postCoins == preCoins - feeDrops);
        }
    }

public:
    void
    run() override
    {
        using namespace test::jtx;
        FeatureBitset const all{supported_amendments()};
        testWithFeats(all);
    }

    void
    testWithFeats(FeatureBitset features)
    {
        // testStaticUNL(features);
        // testInvalid(features);
        testAccountSet(features);
        // testSetRegularKey(features);
        // testSignersListSet(features);
    }
};

BEAST_DEFINE_TESTSUITE(GenerateXPOP, app, ripple);

}  // namespace test
}  // namespace ripple
