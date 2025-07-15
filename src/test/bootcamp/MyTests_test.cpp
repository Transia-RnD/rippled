#include <test/jtx.h>
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
        env.close(); // close the ledger

        auto const preAlice = env.balance(alice);
        std::cout << "Pre-Alice Balance: " << preAlice << std::endl;

        auto jt = env.jt(escrow::create(alice, bob, XRP(1)), 
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
        env(jt); // submits to ledger
        env.close(); // close the ledger

        BEAST_EXPECT(env.balance(alice) == preAlice - XRP(1) - baseFee);
        std::cout << "Post-Alice Balance: " << env.balance(alice) << std::endl;
        std::cout << "Post-Alice Balance=: " << (preAlice - XRP(1) - baseFee) << std::endl;

        auto const k = keylet::escrow(alice, seq);
        auto const sle = env.current()->read(k);
        BEAST_EXPECT(sle);

        auto amtSle = sle->getFieldAmount(sfAmount);
        std::cout << "SLE Amount: " << amtSle.getJson(JsonOptions::none) << std::endl;
        BEAST_EXPECT(amtSle == XRP(1));

        {
            Json::Value params;
            params[jss::ledger_index] = env.current()->seq() - 1;
            params[jss::transactions] = true;
            params[jss::expand] = true;
            auto const jrr = env.rpc("json", "ledger", to_string(params));
            std::cout << jrr << std::endl;
        }
    }

    void 
    run() override
    {
        using namespace test::jtx;
        auto const sa = supported_amendments();
        testProtocol(sa);
    }
};

BEAST_DEFINE_TESTSUITE(MyTests, bootcamp, ripple);

}  // namespace test
}  // namespace ripple