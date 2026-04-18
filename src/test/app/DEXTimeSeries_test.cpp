#include <test/jtx.h>
#include <test/jtx/AMM.h>
#include <test/jtx/AMMTest.h>
#include <test/jtx/envconfig.h>

#include <xrpld/app/misc/DEXTimeSeriesReader.h>
#include <xrpld/app/misc/DEXTimeSeriesStore.h>
#include <xrpld/app/misc/DEXTimeSeriesWriter.h>

#include <xrpl/protocol/jss.h>

namespace xrpl {
namespace test {

class DEXTimeSeries_test : public jtx::AMMTest
{
    static std::unique_ptr<Config>
    dexTimeSeriesConfig(std::unique_ptr<Config> cfg)
    {
        auto& section = cfg->section("dex_timeseries");
        section.set("enabled", "1");
        section.set("start_sequence", "0");
        section.set("tick_retention_hours", "24");
        section.set("candle_1m_retention_hours", "168");
        section.set("candle_5m_retention_hours", "720");
        section.set("candle_1h_retention_hours", "8760");
        section.set("candle_1d_retention_hours", "0");
        section.set("amm_snapshot_retention_hours", "8760");
        section.set("map_size_gb", "1");
        return cfg;
    }

    void
    testStoreEnabled()
    {
        testcase("Store enabled and open");

        using namespace jtx;
        Env env(*this, envconfig(dexTimeSeriesConfig));

        auto& writer = env.app().getDEXTimeSeriesWriter();
        auto* store = writer.getStore();
        BEAST_EXPECT(store != nullptr);
        if (store)
            BEAST_EXPECT(store->isOpen());
    }

    void
    testDexStatusRPC()
    {
        testcase("dex_status RPC returns enabled");

        using namespace jtx;
        Env env(*this, envconfig(dexTimeSeriesConfig));

        Json::Value params;
        auto resp =
            env.rpc("json", "dex_status", to_string(params));
        auto const& result = resp[jss::result];

        BEAST_EXPECT(result[jss::status] == "success");
        BEAST_EXPECT(result["enabled"].asBool() == true);
    }

    void
    testCandlesFromOffers()
    {
        testcase("Candles generated from offer crossing");

        using namespace jtx;
        Env env(*this, envconfig(dexTimeSeriesConfig));

        auto const gw = Account("gw");
        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const USD = gw["USD"];

        env.fund(XRP(100'000), gw, alice, bob);
        env.close();
        env.trust(USD(100'000), alice, bob);
        env.close();
        env(pay(gw, alice, USD(50'000)));
        env(pay(gw, bob, USD(50'000)));
        env.close();

        // Create crossing offers to generate a trade
        env(offer(alice, USD(100), XRP(1'000)));
        env.close();
        env(offer(bob, XRP(1'000), USD(100)));
        env.close();

        // book_changes key format: "XRP_drops|rBase58.../USD"
        auto const bookKey =
            "XRP_drops|" + gw.human() + "/USD";

        auto& reader = env.app().getDEXTimeSeriesReader();
        auto const lastSeq = reader.getLastIndexedSeq();
        BEAST_EXPECT(lastSeq.has_value());

        // Verify candles exist via reader API
        auto candles = reader.getCandles(
            bookKey, DEXInterval::OneMinute, 0, UINT32_MAX, 100);
        BEAST_EXPECT(candles.size() > 0);
    }

    void
    testTradesFromOffers()
    {
        testcase("Trades generated from offer crossing");

        using namespace jtx;
        Env env(*this, envconfig(dexTimeSeriesConfig));

        auto const gw = Account("gw");
        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const USD = gw["USD"];

        env.fund(XRP(100'000), gw, alice, bob);
        env.close();
        env.trust(USD(100'000), alice, bob);
        env.close();
        env(pay(gw, alice, USD(50'000)));
        env(pay(gw, bob, USD(50'000)));
        env.close();

        // Cross offers to create trades
        env(offer(alice, USD(50), XRP(500)));
        env.close();
        env(offer(bob, XRP(500), USD(50)));
        env.close();

        // book_changes key format: "XRP_drops|rBase58.../USD"
        auto const bookKey =
            "XRP_drops|" + gw.human() + "/USD";

        Json::Value params;
        params["book"] = bookKey;
        params["limit"] = 100u;
        auto resp =
            env.rpc("json", "dex_trades", to_string(params));
        auto const& result = resp[jss::result];

        BEAST_EXPECT(result[jss::status] == "success");
        BEAST_EXPECT(result.isMember("trades"));
        BEAST_EXPECT(result["trades"].size() > 0);

        // Also test the legacy dex_ticks endpoint
        auto resp2 =
            env.rpc("json", "dex_ticks", to_string(params));
        auto const& result2 = resp2[jss::result];

        BEAST_EXPECT(result2[jss::status] == "success");
        BEAST_EXPECT(result2.isMember("trades"));
        BEAST_EXPECT(result2["trades"].size() > 0);
    }

    void
    testCandleAggregation()
    {
        testcase("Candle aggregation across multiple ledgers");

        using namespace jtx;
        Env env(*this, envconfig(dexTimeSeriesConfig));

        auto const gw = Account("gw");
        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const USD = gw["USD"];

        env.fund(XRP(1'000'000), gw, alice, bob);
        env.close();
        env.trust(USD(1'000'000), alice, bob);
        env.close();
        env(pay(gw, alice, USD(500'000)));
        env(pay(gw, bob, USD(500'000)));
        env.close();

        // Create multiple trades in the same 1-minute bucket
        for (int i = 0; i < 5; ++i)
        {
            env(offer(alice, USD(10 + i), XRP(100)));
            env.close();
            env(offer(bob, XRP(100), USD(10 + i)));
            env.close();
        }

        // Verify candles got aggregated
        auto& reader = env.app().getDEXTimeSeriesReader();
        auto const lastSeq = reader.getLastIndexedSeq();
        BEAST_EXPECT(lastSeq.has_value());

        // book_changes key format: "XRP_drops|rBase58.../USD"
        auto const bookKey =
            "XRP_drops|" + gw.human() + "/USD";

        // We should have candles at all 4 intervals
        for (auto iv :
             {DEXInterval::OneMinute,
              DEXInterval::FiveMinute,
              DEXInterval::OneHour,
              DEXInterval::OneDay})
        {
            auto candles = reader.getCandles(
                bookKey, iv, 0, UINT32_MAX, 100);
            BEAST_EXPECT(candles.size() > 0);
        }
    }

    void
    testAMMSnapshots()
    {
        testcase("AMM snapshots indexed on state change");

        using namespace jtx;
        Env env(
            *this,
            envconfig(dexTimeSeriesConfig),
            FeatureBitset(testable_amendments()));

        auto const gw = Account("gw");
        auto const alice = Account("alice");
        auto const USD = gw["USD"];

        env.fund(XRP(100'000), gw, alice);
        env.close();
        env.trust(USD(100'000), alice);
        env.close();
        env(pay(gw, alice, USD(50'000)));
        env.close();

        // Create an AMM pool
        AMM amm(env, alice, XRP(10'000), USD(10'000));

        BEAST_EXPECT(amm.ammExists());

        // The AMM creation should have been indexed
        // Get the AMM account
        auto const ammInfo = amm.ammRpcInfo();
        if (!BEAST_EXPECT(ammInfo.isMember(jss::amm)))
            return;
        auto const ammAcct =
            ammInfo[jss::amm][jss::account].asString();

        // Query AMM history
        Json::Value params;
        params["account"] = ammAcct;
        params["limit"] = 100u;
        auto resp =
            env.rpc("json", "dex_amm_history", to_string(params));
        auto const& result = resp[jss::result];

        BEAST_EXPECT(result[jss::status] == "success");
        BEAST_EXPECT(result.isMember("history"));
    }

    void
    testDexPairsRPC()
    {
        testcase("dex_pairs RPC returns pair list");

        using namespace jtx;
        Env env(*this, envconfig(dexTimeSeriesConfig));

        Json::Value params;
        params["sort"] = "volume";
        params["limit"] = 50u;
        auto resp =
            env.rpc("json", "dex_pairs", to_string(params));
        auto const& result = resp[jss::result];

        BEAST_EXPECT(result[jss::status] == "success");
        BEAST_EXPECT(result.isMember("pairs"));
        BEAST_EXPECT(result["pairs"].isArray());
    }

    void
    testDexPoolsRPC()
    {
        testcase("dex_pools RPC returns pool list");

        using namespace jtx;
        Env env(*this, envconfig(dexTimeSeriesConfig));

        Json::Value params;
        params["sort"] = "tvl";
        params["limit"] = 50u;
        auto resp =
            env.rpc("json", "dex_pools", to_string(params));
        auto const& result = resp[jss::result];

        BEAST_EXPECT(result[jss::status] == "success");
        BEAST_EXPECT(result.isMember("pools"));
        BEAST_EXPECT(result["pools"].isArray());
    }

    void
    testDexTokenSummaryRPC()
    {
        testcase("dex_token_summary RPC");

        using namespace jtx;
        Env env(*this, envconfig(dexTimeSeriesConfig));

        // Single book query
        Json::Value params;
        params["book"] = "XRP|USD.rSomeIssuer";
        auto resp =
            env.rpc("json", "dex_token_summary", to_string(params));
        auto const& result = resp[jss::result];

        BEAST_EXPECT(result[jss::status] == "success");
        BEAST_EXPECT(result.isMember("summaries"));

        // Batch query
        Json::Value params2;
        params2["books"] = Json::arrayValue;
        params2["books"].append("XRP|USD.rSomeIssuer");
        params2["books"].append("XRP|EUR.rSomeIssuer");
        auto resp2 =
            env.rpc("json", "dex_token_summary", to_string(params2));
        auto const& result2 = resp2[jss::result];

        BEAST_EXPECT(result2[jss::status] == "success");
        BEAST_EXPECT(result2["summaries"].size() == 2);
    }

    void
    testCandlesRPCValidation()
    {
        testcase("dex_candles RPC input validation");

        using namespace jtx;
        Env env(*this, envconfig(dexTimeSeriesConfig));

        // Missing required "book" field
        {
            Json::Value params;
            auto resp =
                env.rpc("json", "dex_candles", to_string(params));
            BEAST_EXPECT(
                resp[jss::result].isMember(jss::error));
        }

        // Invalid interval
        {
            Json::Value params;
            params["book"] = "XRP|USD.rTest";
            params["interval"] = "2m";
            auto resp =
                env.rpc("json", "dex_candles", to_string(params));
            BEAST_EXPECT(
                resp[jss::result].isMember(jss::error));
        }

        // start_time > end_time
        {
            Json::Value params;
            params["book"] = "XRP|USD.rTest";
            params["start_time"] = 1000u;
            params["end_time"] = 500u;
            auto resp =
                env.rpc("json", "dex_candles", to_string(params));
            BEAST_EXPECT(
                resp[jss::result].isMember(jss::error));
        }

        // Valid request (empty result is fine)
        {
            Json::Value params;
            params["book"] = "XRP|USD.rTest";
            params["interval"] = "5m";
            auto resp =
                env.rpc("json", "dex_candles", to_string(params));
            auto const& result = resp[jss::result];
            BEAST_EXPECT(result[jss::status] == "success");
            BEAST_EXPECT(result.isMember("candles"));
        }
    }

    void
    testTradesRPCValidation()
    {
        testcase("dex_trades RPC input validation");

        using namespace jtx;
        Env env(*this, envconfig(dexTimeSeriesConfig));

        // Missing "book"
        {
            Json::Value params;
            auto resp =
                env.rpc("json", "dex_trades", to_string(params));
            BEAST_EXPECT(
                resp[jss::result].isMember(jss::error));
        }

        // start_time > end_time
        {
            Json::Value params;
            params["book"] = "XRP|USD.rTest";
            params["start_time"] = 2000u;
            params["end_time"] = 1000u;
            auto resp =
                env.rpc("json", "dex_trades", to_string(params));
            BEAST_EXPECT(
                resp[jss::result].isMember(jss::error));
        }
    }

    void
    testDexPairsRPCValidation()
    {
        testcase("dex_pairs RPC input validation");

        using namespace jtx;
        Env env(*this, envconfig(dexTimeSeriesConfig));

        // Invalid sort field
        {
            Json::Value params;
            params["sort"] = "invalid";
            auto resp =
                env.rpc("json", "dex_pairs", to_string(params));
            BEAST_EXPECT(
                resp[jss::result].isMember(jss::error));
        }

        // Valid sort fields
        for (auto const& sort : {"volume", "trades", "change"})
        {
            Json::Value params;
            params["sort"] = sort;
            auto resp =
                env.rpc("json", "dex_pairs", to_string(params));
            BEAST_EXPECT(
                resp[jss::result][jss::status] == "success");
        }
    }

    void
    testDexPoolsRPCValidation()
    {
        testcase("dex_pools RPC input validation");

        using namespace jtx;
        Env env(*this, envconfig(dexTimeSeriesConfig));

        // Invalid sort
        {
            Json::Value params;
            params["sort"] = "invalid";
            auto resp =
                env.rpc("json", "dex_pools", to_string(params));
            BEAST_EXPECT(
                resp[jss::result].isMember(jss::error));
        }

        // Valid sort fields
        for (auto const& sort : {"tvl", "volume", "apr"})
        {
            Json::Value params;
            params["sort"] = sort;
            auto resp =
                env.rpc("json", "dex_pools", to_string(params));
            BEAST_EXPECT(
                resp[jss::result][jss::status] == "success");
        }
    }

    void
    testLastIndexedSeq()
    {
        testcase("last_indexed_seq advances with ledgers");

        using namespace jtx;
        Env env(*this, envconfig(dexTimeSeriesConfig));

        auto const gw = Account("gw");
        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const USD = gw["USD"];

        env.fund(XRP(100'000), gw, alice, bob);
        env.close();
        env.trust(USD(100'000), alice, bob);
        env.close();
        env(pay(gw, alice, USD(50'000)));
        env(pay(gw, bob, USD(50'000)));
        env.close();

        // Create a trade to trigger indexing
        env(offer(alice, USD(100), XRP(1'000)));
        env.close();
        env(offer(bob, XRP(1'000), USD(100)));
        env.close();

        auto& reader = env.app().getDEXTimeSeriesReader();
        auto seq1 = reader.getLastIndexedSeq();
        BEAST_EXPECT(seq1.has_value());

        // Close more ledgers with trades
        env(offer(alice, USD(200), XRP(2'000)));
        env.close();
        env(offer(bob, XRP(2'000), USD(200)));
        env.close();

        auto seq2 = reader.getLastIndexedSeq();
        BEAST_EXPECT(seq2.has_value());
        if (seq1 && seq2)
        {
            BEAST_EXPECT(*seq2 > *seq1);
        }
    }

    void
    testDisabledConfig()
    {
        testcase("Store disabled when not configured");

        using namespace jtx;
        Env env(*this);

        auto& writer = env.app().getDEXTimeSeriesWriter();
        auto* store = writer.getStore();
        BEAST_EXPECT(store == nullptr);

        // RPC should still work, just return empty/disabled
        Json::Value params;
        auto resp =
            env.rpc("json", "dex_status", to_string(params));
        auto const& result = resp[jss::result];
        BEAST_EXPECT(result[jss::status] == "success");
        BEAST_EXPECT(result["enabled"].asBool() == false);
    }

    void
    testMultipleIntervals()
    {
        testcase("All interval candles populated");

        using namespace jtx;
        Env env(*this, envconfig(dexTimeSeriesConfig));

        auto const gw = Account("gw");
        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const USD = gw["USD"];

        env.fund(XRP(100'000), gw, alice, bob);
        env.close();
        env.trust(USD(100'000), alice, bob);
        env.close();
        env(pay(gw, alice, USD(50'000)));
        env(pay(gw, bob, USD(50'000)));
        env.close();

        env(offer(alice, USD(100), XRP(1'000)));
        env.close();
        env(offer(bob, XRP(1'000), USD(100)));
        env.close();

        // book_changes key format: "XRP_drops|rBase58.../USD"
        auto const bookKey =
            "XRP_drops|" + gw.human() + "/USD";

        // All 4 intervals should be queryable via RPC
        for (auto const& iv : {"1m", "5m", "1h", "1d"})
        {
            Json::Value params;
            params["book"] = bookKey;
            params["interval"] = iv;
            auto resp =
                env.rpc("json", "dex_candles", to_string(params));
            auto const& result = resp[jss::result];
            BEAST_EXPECT(result[jss::status] == "success");
            BEAST_EXPECT(result.isMember("candles"));
            BEAST_EXPECT(result["candles"].size() > 0);
        }
    }

    void
    testBookKeyFormat()
    {
        testcase("Verify book_changes produces indexable keys");

        using namespace jtx;
        Env env(*this, envconfig(dexTimeSeriesConfig));

        auto const gw = Account("gw");
        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const USD = gw["USD"];

        env.fund(XRP(100'000), gw, alice, bob);
        env.close();
        env.trust(USD(100'000), alice, bob);
        env.close();
        env(pay(gw, alice, USD(50'000)));
        env(pay(gw, bob, USD(50'000)));
        env.close();

        env(offer(alice, USD(100), XRP(1'000)));
        env.close();
        env(offer(bob, XRP(1'000), USD(100)));
        env.close();

        // Verify the writer indexed data by querying reader directly.
        // The writer constructs book keys from book_changes output
        // (currency_a|currency_b), so if candles exist the key
        // format is consistent end-to-end.
        auto& reader = env.app().getDEXTimeSeriesReader();
        auto const lastSeq = reader.getLastIndexedSeq();
        BEAST_EXPECT(lastSeq.has_value());

        // book_changes produces keys like "XRP_drops|rBase58.../USD"
        // (currency_a|to_string(issue_b)). Verify candles were stored
        // with this format by querying the reader directly.
        auto const bookKey =
            "XRP_drops|" + gw.human() + "/USD";
        auto candles = reader.getCandles(
            bookKey, DEXInterval::OneMinute, 0, UINT32_MAX, 100);
        BEAST_EXPECT(candles.size() > 0);

        if (!candles.empty())
        {
            auto const& c = candles[0];
            BEAST_EXPECT(c.open != 0);
            BEAST_EXPECT(c.high >= c.low);
            BEAST_EXPECT(c.volumeBase != 0);
            BEAST_EXPECT(c.txCount > 0);
        }

        // Verify trades exist for the same key
        auto trades = reader.getTrades(bookKey, 0, UINT32_MAX, 100);
        BEAST_EXPECT(trades.size() > 0);

        // Verify the RPC also works with this key
        Json::Value params;
        params["book"] = bookKey;
        params["interval"] = "1m";
        auto resp = env.rpc(
            "json", "dex_candles", to_string(params));
        auto const& result = resp[jss::result];
        BEAST_EXPECT(result[jss::status] == "success");
        BEAST_EXPECT(result["candles"].isArray());
        BEAST_EXPECT(result["candles"].size() > 0);
    }

public:
    void
    run() override
    {
        testStoreEnabled();
        testDexStatusRPC();
        testDisabledConfig();
        testCandlesRPCValidation();
        testTradesRPCValidation();
        testDexPairsRPCValidation();
        testDexPoolsRPCValidation();
        testDexPairsRPC();
        testDexPoolsRPC();
        testDexTokenSummaryRPC();
        testLastIndexedSeq();
        testCandlesFromOffers();
        testTradesFromOffers();
        testCandleAggregation();
        testMultipleIntervals();
        testBookKeyFormat();
        testAMMSnapshots();
    }
};

BEAST_DEFINE_TESTSUITE_PRIO(DEXTimeSeries, app, xrpl, 1);

}  // namespace test
}  // namespace xrpl
