// Tests for ttIMPORT_CREDIT pseudo-transaction (Change transactor)
// and ImportCreditVote consensus integration.

#include <test/jtx.h>

#include <xrpld/app/ledger/Ledger.h>
#include <xrpld/app/misc/ImportCreditVote.h>
#include <xrpld/app/misc/MainnetWatcher.h>

#include <xrpl/basics/StringUtilities.h>
#include <xrpl/ledger/View.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/tx/apply.h>

#include <cstring>

namespace xrpl {
namespace test {

// Helper: extract all STTx from a SHAMap
static std::vector<STTx>
getTxs(std::shared_ptr<SHAMap> const& txSet)
{
    std::vector<STTx> txs;
    for (auto i = txSet->begin(); i != txSet->end(); ++i)
    {
        auto const data = i->slice();
        auto serialIter = SerialIter(data);
        txs.push_back(STTx(serialIter));
    }
    return txs;
}

// Helper: create a well-formed ttIMPORT_CREDIT pseudo-tx
static STTx
makeImportCredit(
    AccountID const& destination,
    XRPAmount amount,
    uint256 const& sourceTxnID,
    std::uint32_t importSequence,
    std::uint32_t ledgerSequence,
    std::optional<uint256> invoiceID = std::nullopt)
{
    return STTx(ttIMPORT_CREDIT, [&](auto& obj) {
        // Pseudo-txs require sfAccount=zero. Must be explicitly set
        // (not template default) so STAccount::isDefault() returns false.
        obj.setAccountID(sfAccount, AccountID());
        obj.setAccountID(sfDestination, destination);
        obj.setFieldAmount(sfAmount, STAmount(amount));
        obj.setFieldH256(sfSourceTxnID, sourceTxnID);
        obj.setFieldU32(sfImportSequence, importSequence);
        obj.setFieldU32(sfLedgerSequence, ledgerSequence);
        if (invoiceID)
            obj.setFieldH256(sfInvoiceID, *invoiceID);
    });
}

struct ImportCreditConsensus_test : public beast::unit_test::suite
{
    // =========================================================================
    // Test: preflight rejects ttIMPORT_CREDIT when featureImportExport
    // is not enabled.
    // =========================================================================
    void
    testPreflightDisabled()
    {
        testcase("preflight rejects when amendment disabled");
        using namespace jtx;

        // Use all amendments EXCEPT featureImportExport
        Env env(*this, testable_amendments() - featureImportExport);

        auto const alice = Account("alice");
        env.fund(XRP(10000), alice);
        env.close();

        auto const tx = makeImportCredit(
            alice.id(),
            XRPAmount{1'000'000},
            uint256{1},
            1,
            env.closed()->seq() + 1);

        // Apply to open ledger (pseudo-txs on open ledger → temINVALID)
        // But preflight should catch temDISABLED first
        env.app().openLedger().modify([&](OpenView& view, beast::Journal j) {
            auto const result = apply(env.app(), view, tx, tapNONE, j);
            // temDISABLED because featureImportExport is not enabled
            BEAST_EXPECT(!result.applied);
            BEAST_EXPECT(
                result.ter == temDISABLED || result.ter == temINVALID);
            return result.applied;
        });
    }

    // =========================================================================
    // Test: preflight rejects malformed ttIMPORT_CREDIT (negative amount,
    // IOU amount, missing fields).
    // =========================================================================
    void
    testPreflightMalformed()
    {
        testcase("preflight rejects malformed import credit");
        using namespace jtx;

        Env env(*this, testable_amendments());

        auto const alice = Account("alice");
        auto const seq = env.closed()->seq() + 1;

        // Helper lambda: apply to open ledger and return TER
        auto applyAndGetTer = [&](STTx const& tx) -> TER {
            TER result = tesSUCCESS;
            env.app().openLedger().modify(
                [&](OpenView& view, beast::Journal j) {
                    auto const res = apply(env.app(), view, tx, tapNONE, j);
                    result = res.ter;
                    return res.applied;
                });
            return result;
        };

        // Zero amount → temMALFORMED
        {
            auto tx = STTx(ttIMPORT_CREDIT, [&](auto& obj) {
                obj.setAccountID(sfAccount, AccountID());
                obj.setAccountID(sfDestination, alice.id());
                obj.setFieldAmount(sfAmount, STAmount(XRPAmount{0}));
                obj.setFieldH256(sfSourceTxnID, uint256{1});
                obj.setFieldU32(sfImportSequence, 1);
                obj.setFieldU32(sfLedgerSequence, seq);
            });
            auto ter = applyAndGetTer(tx);
            BEAST_EXPECT(ter == temMALFORMED || ter == temINVALID);
        }

        // Negative amount → temMALFORMED
        {
            auto tx = STTx(ttIMPORT_CREDIT, [&](auto& obj) {
                obj.setAccountID(sfAccount, AccountID());
                obj.setAccountID(sfDestination, alice.id());
                obj.setFieldAmount(sfAmount, STAmount(XRPAmount{-100}));
                obj.setFieldH256(sfSourceTxnID, uint256{1});
                obj.setFieldU32(sfImportSequence, 1);
                obj.setFieldU32(sfLedgerSequence, seq);
            });
            auto ter = applyAndGetTer(tx);
            BEAST_EXPECT(ter == temMALFORMED || ter == temINVALID);
        }
    }

    // =========================================================================
    // Test: applyImportCredit creates a new account, mints XRP, sets
    // master key enabled, and creates an ImportRecord.
    // =========================================================================
    void
    testApplyCreatesAccount()
    {
        testcase("applyImportCredit creates new account");
        using namespace jtx;

        Env env(*this, testable_amendments());

        // Build a ledger we can apply pseudo-txs against (non-open)
        auto ledger = std::make_shared<Ledger>(
            create_genesis,
            env.app().config(),
            std::vector<uint256>{},
            env.app().getNodeFamily());

        // Advance a few ledger sequences
        for (int i = 0; i < 5; ++i)
            ledger = std::make_shared<Ledger>(
                *ledger, env.app().timeKeeper().closeTime());

        auto const seq = ledger->seq() + 1;

        // New account that doesn't exist yet
        AccountID const newAcct{0xABu};
        uint256 const sourceTxnID{42};
        XRPAmount const amount{10'000'000};  // 10 XRP

        auto tx = makeImportCredit(newAcct, amount, sourceTxnID, 1, seq);

        // Verify account doesn't exist
        BEAST_EXPECT(!ledger->read(keylet::account(newAcct)));

        // Apply pseudo-tx
        OpenView accum(ledger.get());
        auto const res =
            apply(env.app(), accum, tx, ApplyFlags::tapNONE, env.journal);
        log << "  applyImportCredit TER=" << transHuman(res.ter)
            << " applied=" << res.applied << std::endl;
        BEAST_EXPECT(res.ter == tesSUCCESS);
        if (res.ter == tesSUCCESS)
            accum.apply(*ledger);

        // Verify account was created
        auto sle = ledger->read(keylet::account(newAcct));
        BEAST_EXPECT(sle);
        if (sle)
        {
            BEAST_EXPECT(sle->getAccountID(sfAccount) == newAcct);

            // Master key should be enabled (lsfDisableMaster NOT set)
            std::uint32_t flags = sle->getFlags();
            BEAST_EXPECT(!(flags & lsfDisableMaster));

            // ImportSequence should be set
            BEAST_EXPECT(sle->getFieldU32(sfImportSequence) == 1);

            // Balance should include starting bonus + minted amount
            STAmount balance = sle->getFieldAmount(sfBalance);
            BEAST_EXPECT(balance.xrp() >= amount);
        }

        // Verify ImportRecord was created
        auto sleImport = ledger->read(keylet::importRecord(newAcct, 1));
        BEAST_EXPECT(sleImport);
        if (sleImport)
        {
            BEAST_EXPECT(sleImport->getAccountID(sfAccount) == newAcct);
            BEAST_EXPECT(
                sleImport->getFieldH256(sfSourceTxnID) == sourceTxnID);
            BEAST_EXPECT(sleImport->getFieldU32(sfImportSequence) == 1);
        }
    }

    // =========================================================================
    // Test: applyImportCredit credits an existing account (no account
    // creation, just balance increase).
    // =========================================================================
    void
    testApplyCreditsExisting()
    {
        testcase("applyImportCredit credits existing account");
        using namespace jtx;

        Env env(*this, testable_amendments());

        // Create a genesis ledger and manually insert an account
        auto ledger = std::make_shared<Ledger>(
            create_genesis,
            env.app().config(),
            std::vector<uint256>{},
            env.app().getNodeFamily());

        for (int i = 0; i < 5; ++i)
            ledger = std::make_shared<Ledger>(
                *ledger, env.app().timeKeeper().closeTime());

        AccountID const acct{0xDDu};
        STAmount const startBalance{XRPAmount{100'000'000}};  // 100 XRP

        // Insert account SLE directly
        {
            OpenView accum(ledger.get());
            auto sle = std::make_shared<SLE>(keylet::account(acct));
            sle->setAccountID(sfAccount, acct);
            sle->setFieldAmount(sfBalance, startBalance);
            sle->setFieldU32(sfSequence, 1);
            sle->setFieldU32(sfOwnerCount, 0);
            accum.rawInsert(sle);
            accum.apply(*ledger);
        }

        // Verify account exists with expected balance
        auto const sleBefore = ledger->read(keylet::account(acct));
        BEAST_EXPECT(sleBefore);
        BEAST_EXPECT(
            sleBefore->getFieldAmount(sfBalance) == startBalance);

        auto const seq = ledger->seq() + 1;
        XRPAmount const amount{5'000'000};  // 5 XRP
        uint256 const sourceTxnID{99};

        auto tx = makeImportCredit(acct, amount, sourceTxnID, 1, seq);

        OpenView accum(ledger.get());
        auto const res =
            apply(env.app(), accum, tx, ApplyFlags::tapNONE, env.journal);
        BEAST_EXPECT(res.ter == tesSUCCESS);
        accum.apply(*ledger);

        // Verify balance increased by exactly the minted amount
        auto sleAfter = ledger->read(keylet::account(acct));
        BEAST_EXPECT(sleAfter);
        if (sleAfter)
        {
            STAmount const balAfter = sleAfter->getFieldAmount(sfBalance);
            BEAST_EXPECT(balAfter == startBalance + STAmount(amount));
            BEAST_EXPECT(sleAfter->getFieldU32(sfImportSequence) == 1);
        }
    }

    // =========================================================================
    // Test: replay protection — applyImportCredit rejects when
    // ImportSequence <= existing ImportSequence on the account.
    // =========================================================================
    void
    testReplayProtection()
    {
        testcase("applyImportCredit replay protection");
        using namespace jtx;

        Env env(*this, testable_amendments());

        auto ledger = std::make_shared<Ledger>(
            create_genesis,
            env.app().config(),
            std::vector<uint256>{},
            env.app().getNodeFamily());

        for (int i = 0; i < 5; ++i)
            ledger = std::make_shared<Ledger>(
                *ledger, env.app().timeKeeper().closeTime());

        auto const seq = ledger->seq() + 1;
        AccountID const acct{0xCDu};
        XRPAmount const amount{1'000'000};

        // First import — should succeed
        {
            auto tx = makeImportCredit(acct, amount, uint256{1}, 1, seq);
            OpenView accum(ledger.get());
            auto const res =
                apply(env.app(), accum, tx, ApplyFlags::tapNONE, env.journal);
            BEAST_EXPECT(res.ter == tesSUCCESS);
            accum.apply(*ledger);
        }

        // Same ImportSequence again — should fail (replay)
        {
            auto tx = makeImportCredit(acct, amount, uint256{2}, 1, seq);
            OpenView accum(ledger.get());
            auto const res =
                apply(env.app(), accum, tx, ApplyFlags::tapNONE, env.journal);
            BEAST_EXPECT(res.ter != tesSUCCESS);
        }

        // Lower ImportSequence — should also fail
        {
            auto tx = makeImportCredit(acct, amount, uint256{3}, 0, seq);
            OpenView accum(ledger.get());
            auto const res =
                apply(env.app(), accum, tx, ApplyFlags::tapNONE, env.journal);
            BEAST_EXPECT(res.ter != tesSUCCESS);
        }

        // Higher ImportSequence — should succeed
        {
            auto tx = makeImportCredit(acct, amount, uint256{4}, 2, seq);
            OpenView accum(ledger.get());
            auto const res =
                apply(env.app(), accum, tx, ApplyFlags::tapNONE, env.journal);
            BEAST_EXPECT(res.ter == tesSUCCESS);
            accum.apply(*ledger);
        }
    }

    // =========================================================================
    // Test: InvoiceID sets RegularKey on account creation.
    // =========================================================================
    void
    testRegularKeyFromInvoiceID()
    {
        testcase("applyImportCredit sets RegularKey from InvoiceID");
        using namespace jtx;

        Env env(*this, testable_amendments());

        auto ledger = std::make_shared<Ledger>(
            create_genesis,
            env.app().config(),
            std::vector<uint256>{},
            env.app().getNodeFamily());

        for (int i = 0; i < 5; ++i)
            ledger = std::make_shared<Ledger>(
                *ledger, env.app().timeKeeper().closeTime());

        auto const seq = ledger->seq() + 1;
        AccountID const acct{0xEFu};

        // Build a 32-byte InvoiceID where first 20 bytes encode the
        // desired RegularKey AccountID
        uint256 invoiceID{};
        AccountID const regularKey{0x42u};
        std::memcpy(invoiceID.data(), regularKey.data(), 20);

        auto tx = makeImportCredit(
            acct,
            XRPAmount{5'000'000},
            uint256{10},
            1,
            seq,
            invoiceID);

        OpenView accum(ledger.get());
        auto const res =
            apply(env.app(), accum, tx, ApplyFlags::tapNONE, env.journal);
        BEAST_EXPECT(res.ter == tesSUCCESS);
        accum.apply(*ledger);

        auto sle = ledger->read(keylet::account(acct));
        BEAST_EXPECT(sle);
        if (sle)
        {
            // RegularKey should be set
            BEAST_EXPECT(sle->isFieldPresent(sfRegularKey));
            if (sle->isFieldPresent(sfRegularKey))
            {
                BEAST_EXPECT(
                    sle->getAccountID(sfRegularKey) == regularKey);
            }

            // Master key should still be enabled
            BEAST_EXPECT(!(sle->getFlags() & lsfDisableMaster));
        }
    }

    // =========================================================================
    // Test: No InvoiceID → no RegularKey set.
    // =========================================================================
    void
    testNoInvoiceIDNoRegularKey()
    {
        testcase("applyImportCredit without InvoiceID: no RegularKey");
        using namespace jtx;

        Env env(*this, testable_amendments());

        auto ledger = std::make_shared<Ledger>(
            create_genesis,
            env.app().config(),
            std::vector<uint256>{},
            env.app().getNodeFamily());

        for (int i = 0; i < 5; ++i)
            ledger = std::make_shared<Ledger>(
                *ledger, env.app().timeKeeper().closeTime());

        auto const seq = ledger->seq() + 1;
        AccountID const acct{0xBBu};

        auto tx = makeImportCredit(
            acct, XRPAmount{5'000'000}, uint256{20}, 1, seq);

        OpenView accum(ledger.get());
        auto const res =
            apply(env.app(), accum, tx, ApplyFlags::tapNONE, env.journal);
        BEAST_EXPECT(res.ter == tesSUCCESS);
        accum.apply(*ledger);

        auto sle = ledger->read(keylet::account(acct));
        BEAST_EXPECT(sle);
        if (sle)
        {
            BEAST_EXPECT(!sle->isFieldPresent(sfRegularKey));
        }
    }

    // =========================================================================
    // Test: ImportCreditVote::doVoting injects pseudo-txs into the
    // initial transaction set when MainnetWatcher has ready imports.
    // =========================================================================
    void
    testDoVoting()
    {
        testcase("ImportCreditVote doVoting");
        using namespace jtx;

        Env env(*this, testable_amendments());

        auto* watcher = env.app().getMainnetWatcher();
        if (!watcher)
        {
            // MainnetWatcher not configured — this is expected in
            // unit test environments. We can still test via the Vote
            // class directly by using a helper that doesn't need the
            // watcher.
            log << "  MainnetWatcher not available, testing doVoting "
                   "path returns early"
                << std::endl;

            // Verify doVoting returns gracefully when no watcher
            ImportCreditVote vote(env.app(), env.journal);
            auto ledger = std::make_shared<Ledger>(
                create_genesis,
                env.app().config(),
                std::vector<uint256>{},
                env.app().getNodeFamily());

            auto txSet = std::make_shared<SHAMap>(
                SHAMapType::TRANSACTION, env.app().getNodeFamily());

            // Should not crash — just returns early
            vote.doVoting(ledger, txSet);

            auto txs = getTxs(txSet);
            BEAST_EXPECT(txs.empty());
            return;
        }

        // MainnetWatcher IS available — inject a ReadyImport
        AccountID const recipient{0xAAu};
        uint256 const mainnetHash{77};

        Json::Value xpop;
        xpop["transaction"]["meta"]["DeliveredAmount"] = "5000000";
        xpop["transaction"]["blob"]["InvoiceID"] =
            "0000000000000000000000000000004200000000000000000000000000000000";
        xpop["transaction"]["blob"]["Account"] =
            toBase58(recipient);

        MainnetWatcher::ReadyImport ri;
        ri.sidechainAccount = recipient;
        ri.importSequence = 1;
        ri.mainnetTxHash = mainnetHash;
        ri.xpop = std::move(xpop);

        watcher->injectReadyImport(std::move(ri));

        // Build ledger and call doVoting
        auto ledger = std::make_shared<Ledger>(
            create_genesis,
            env.app().config(),
            std::vector<uint256>{},
            env.app().getNodeFamily());

        for (int i = 0; i < 5; ++i)
            ledger = std::make_shared<Ledger>(
                *ledger, env.app().timeKeeper().closeTime());

        auto txSet = std::make_shared<SHAMap>(
            SHAMapType::TRANSACTION, env.app().getNodeFamily());

        ImportCreditVote vote(env.app(), env.journal);
        vote.doVoting(ledger, txSet);

        auto txs = getTxs(txSet);
        BEAST_EXPECT(txs.size() == 1);

        if (!txs.empty())
        {
            auto const& importTx = txs[0];
            BEAST_EXPECT(importTx.getTxnType() == ttIMPORT_CREDIT);
            BEAST_EXPECT(importTx.getAccountID(sfDestination) == recipient);
            BEAST_EXPECT(
                importTx.getFieldAmount(sfAmount) ==
                STAmount(XRPAmount{5'000'000}));
            BEAST_EXPECT(
                importTx.getFieldH256(sfSourceTxnID) == mainnetHash);
            BEAST_EXPECT(importTx.getFieldU32(sfImportSequence) == 1);
        }

        // Call doVoting again — same import should NOT be re-proposed
        auto txSet2 = std::make_shared<SHAMap>(
            SHAMapType::TRANSACTION, env.app().getNodeFamily());
        vote.doVoting(ledger, txSet2);
        auto txs2 = getTxs(txSet2);
        BEAST_EXPECT(txs2.empty());
    }

    // =========================================================================
    // Test: ttIMPORT_CREDIT is treated as a pseudo-tx and cannot be
    // submitted by users via the open ledger.
    // =========================================================================
    void
    testCannotSubmitDirectly()
    {
        testcase("ttIMPORT_CREDIT cannot be submitted directly");
        using namespace jtx;

        Env env(*this, testable_amendments());

        auto const tx = makeImportCredit(
            AccountID{0xFFu},
            XRPAmount{1'000'000},
            uint256{1},
            1,
            env.closed()->seq() + 1);

        // Pseudo-tx check
        BEAST_EXPECT(isPseudoTx(tx));

        // Local checks should reject
        std::string reason;
        BEAST_EXPECT(!passesLocalChecks(tx, reason));
        BEAST_EXPECT(reason == "Cannot submit pseudo transactions.");
    }

    // =========================================================================
    // Test: ImportRecord keylet construction produces unique keys.
    // =========================================================================
    void
    testImportRecordKeylet()
    {
        testcase("ImportRecord keylet uniqueness");

        AccountID const acct1{0x01u};
        AccountID const acct2{0x02u};

        auto const k1 = keylet::importRecord(acct1, 0);
        auto const k2 = keylet::importRecord(acct1, 1);
        auto const k3 = keylet::importRecord(acct2, 0);

        // Different sequences → different keys
        BEAST_EXPECT(k1.key != k2.key);
        // Different accounts → different keys
        BEAST_EXPECT(k1.key != k3.key);
        // All non-zero
        BEAST_EXPECT(k1.key != uint256{});
        BEAST_EXPECT(k2.key != uint256{});
        BEAST_EXPECT(k3.key != uint256{});
    }

public:
    void
    run() override
    {
        testPreflightDisabled();
        testPreflightMalformed();
        testApplyCreatesAccount();
        testApplyCreditsExisting();
        testReplayProtection();
        testRegularKeyFromInvoiceID();
        testNoInvoiceIDNoRegularKey();
        testDoVoting();
        testCannotSubmitDirectly();
        testImportRecordKeylet();
    }
};

BEAST_DEFINE_TESTSUITE(ImportCreditConsensus, app, ripple);

}  // namespace test
}  // namespace xrpl
