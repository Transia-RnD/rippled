//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2025 Ripple Labs Inc.

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

#include <test/jtx.h>

#include <xrpld/app/rdb/backend/StopLossDB.h>

#include <xrpl/protocol/TxFlags.h>

#include <algorithm>

namespace ripple {
namespace test {

class StopLoss_test : public beast::unit_test::suite
{
public:
    void
    testStopLoss(FeatureBitset features)
    {
        testcase("stop loss orders");

        using namespace jtx;
        Env env{*this, features};

        auto const gw = Account{"gateway"};
        auto const alice = Account{"alice"};
        auto const bob = Account{"bob"};
        auto const carol = Account{"carol"};
        auto const USD = gw["USD"];

        // Setup accounts
        env.fund(XRP(10000), alice, bob, carol, gw);
        env.close();
        env.trust(USD(1000000), alice);
        env.trust(USD(1000000), bob);
        env.trust(USD(1000000), carol);
        env.close();
        env(pay(gw, alice, USD(10000)));
        env(pay(gw, bob, USD(10000)));
        env(pay(gw, carol, USD(10000)));
        env.close();

        std::uint32_t aliceTicketSeq{env.seq(alice) + 1};
        env(ticket::create(alice, 3));

        // Initial market state: Set XRP/USD price at ~0.025 USD per XRP
        env(offer(bob, USD(1000), XRP(40000)));  // 0.025 USD/XRP
        env.close();

        // Test 1: Stop-Loss Sell
        // Alice wants to sell XRP if price drops to 0.020 USD/XRP or below
        {
            testcase("stop loss sell order");

            // Create pre-signed OfferCreate to sell XRP for USD using ticket
            STTx offerTx(
                ttOFFER_CREATE, [&alice, &aliceTicketSeq, &USD](STObject& obj) {
                    obj.setAccountID(sfAccount, alice.id());
                    obj.setFieldU32(sfTicketSequence, aliceTicketSeq++);
                    obj.setFieldU32(sfSequence, 0);
                    obj.setFieldAmount(sfTakerPays, USD(200));    // Want USD
                    obj.setFieldAmount(sfTakerGets, XRP(10000));  // Selling XRP
                    obj.setFieldU32(sfFlags, tfSell | tfImmediateOrCancel);
                });

            // Sign the transaction
            offerTx.sign(alice.pk(), alice.sk());

            // Submit as stop-loss order
            Json::Value params;
            params[jss::tx_blob] = strHex(offerTx.getSerializer().peekData());
            params["stop_price"] = "0.020";
            params["stop_type"] = "stop_loss_sell";

            auto const result =
                env.rpc("json", "submit_passive", to_string(params));
            std::cout << result.toStyledString() << std::endl;
            BEAST_EXPECT(
                result[jss::result][jss::engine_result] == "tesSUCCESS");

            // Price hasn't dropped yet, order shouldn't trigger
            env.close();

            // Market maker drops the price below stop (0.020)
            env(offer(carol, XRP(180), USD(10000)),
                txflags(tfSell));  // 0.018 USD/XRP
            env.close();

            {
                Json::Value params;
                params[jss::ledger_index] = env.current()->seq() - 1;
                params[jss::transactions] = true;
                params[jss::expand] = true;
                auto const jrr = env.rpc("json", "ledger", to_string(params));
                std::cout << "jrr: " << jrr << "\n";
            }

            auto& stopLossDB = env.app().getStopLossDB();
            auto const orders = stopLossDB.getOrderCount();
            BEAST_EXPECT(orders == 1);
            auto const markets = stopLossDB.getMonitoredMarkets();
            BEAST_EXPECT(markets.size() == 1);
        }

        // // Test 2: Stop-Loss Buy (Stop-Buy)
        // // Alice wants to buy XRP if price rises to 0.030 USD/XRP or above
        // {
        //     testcase("stop loss buy order");

        //     STTx buyTx(ttOFFER_CREATE,
        //         [&alice, &ticketId2, &USD](STObject& obj) {
        //             obj.setAccountID(sfAccount, alice.id());
        //             obj.setFieldU256(sfTicketSequence, ticketId2);
        //             obj.setFieldU32(sfSequence, 0);
        //             obj.setFieldAmount(sfTakerPays, XRP(10000)); // Want XRP
        //             obj.setFieldAmount(sfTakerGets, USD(300));    // Paying
        //             USD obj.setFieldU32(sfFlags, tfImmediateOrCancel);
        //         });

        //     buyTx.sign(alice.pk(), alice.sk());

        //     Json::Value params;
        //     params[jss::tx_blob] = strHex(buyTx.getSerializer().peekData());
        //     params[jss::stop_price] = "0.030";
        //     params[jss::stop_type] = "stop_loss_buy";

        //     auto const result = env.rpc("json", "submit_passive",
        //     to_string(params));
        //     BEAST_EXPECT(result[jss::result][jss::engine_result] ==
        //     "tesSUCCESS");

        //     // Price rises above stop
        //     env(offer(bob, USD(1000), XRP(30000))); // 0.033 USD/XRP
        //     env.close();

        //     // Check if stop-buy triggered
        //     auto& stopLossDB = env.app().getStopLossDB();
        //     STAmount highPrice = STAmount(0.033);
        //     auto triggered = stopLossDB.getTriggeredOrders(
        //         highPrice, StopType::STOP_LOSS_BUY);
        //     BEAST_EXPECT(triggered.size() == 1);
        // }

        // // Test 3: Take-Profit Sell
        // // Alice wants to sell XRP if price rises to 0.035 USD/XRP or above
        // {
        //     testcase("take profit sell order");

        //     STTx tpSellTx(ttOFFER_CREATE,
        //         [&alice, &ticketId3, &USD](STObject& obj) {
        //             obj.setAccountID(sfAccount, alice.id());
        //             obj.setFieldU256(sfTicketSequence, ticketId3);
        //             obj.setFieldU32(sfSequence, 0);
        //             obj.setFieldAmount(sfTakerPays, USD(350));   // Want USD
        //             obj.setFieldAmount(sfTakerGets, XRP(10000)); // Selling
        //             XRP obj.setFieldU32(sfFlags, tfSell |
        //             tfImmediateOrCancel);
        //         });

        //     tpSellTx.sign(alice.pk(), alice.sk());

        //     Json::Value params;
        //     params[jss::tx_blob] =
        //     strHex(tpSellTx.getSerializer().peekData());
        //     params[jss::stop_price] = "0.035";
        //     params[jss::stop_type] = "take_profit_sell";

        //     auto const result = env.rpc("json", "submit_passive",
        //     to_string(params));
        //     BEAST_EXPECT(result[jss::result][jss::engine_result] ==
        //     "tesSUCCESS");

        //     // Price rises to trigger take-profit
        //     env(offer(carol, USD(1000), XRP(25000))); // 0.040 USD/XRP
        //     env.close();

        //     // Check if take-profit triggered
        //     auto& stopLossDB = env.app().getStopLossDB();
        //     STAmount tpPrice = STAmount(0.040);
        //     auto triggered = stopLossDB.getTriggeredOrders(
        //         tpPrice, StopType::TAKE_PROFIT_SELL);
        //     BEAST_EXPECT(triggered.size() == 1);
        // }

        // // Verify total orders in database
        // {
        //     auto& stopLossDB = env.app().getStopLossDB();
        //     auto orderCount = stopLossDB.getOrderCount();
        //     BEAST_EXPECT(orderCount == 3); // All 3 orders stored
        // }
    }

    void
    run() override
    {
        using namespace test::jtx;
        FeatureBitset const all{supported_amendments()};
        testStopLoss(all);
    }
};

BEAST_DEFINE_TESTSUITE(StopLoss, rpc, ripple);

}  // namespace test
}  // namespace ripple