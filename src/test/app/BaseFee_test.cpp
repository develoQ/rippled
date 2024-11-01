//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2023 XRPL-Labs

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
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/protocol/jss.h>

namespace ripple {
namespace test {

class BaseFee_test : public beast::unit_test::suite
{
    void
    testRPCCall(
        jtx::Env& env,
        Json::Value tx,
        std::string expected,
        bool testSerialized = true)
    {
        {
            Json::Value params;
            params[jss::tx_json] = tx;

            // fee request
            auto const jrr = env.rpc("json", "fee", to_string(params));
            // std::cout << "RESULT: " << jrr << "\n";

            // verify base fee & open ledger fee
            auto const drops = jrr[jss::result][jss::drops];
            auto const baseFee = drops[jss::base_fee];
            BEAST_EXPECT(baseFee == expected);
            auto const openLedgerFee = drops[jss::open_ledger_fee];
            BEAST_EXPECT(openLedgerFee == expected);
        }

        if (testSerialized)
        {
            auto const jtx = env.jt(tx);
            // build tx_blob
            Json::Value params;
            params[jss::tx_blob] = strHex(jtx.stx->getSerializer().slice());

            // fee request
            auto const jrr = env.rpc("json", "fee", to_string(params));
            // std::cout << "RESULT: " << jrr << "\n";

            // verify base fee & open ledger fee
            auto const drops = jrr[jss::result][jss::drops];
            auto const baseFee = drops[jss::base_fee];
            BEAST_EXPECT(baseFee == expected);
            auto const openLedgerFee = drops[jss::open_ledger_fee];
            BEAST_EXPECT(openLedgerFee == expected);
        }
    }

    void
    testBaseFee(FeatureBitset features)
    {
        testcase("base fee");

        using namespace test::jtx;
        using namespace std::literals;

        Env env{*this, features};

        auto const account = Account("alice");
        env.fund(XRP(1000), account);
        env.close();

        // build tx
        auto tx = fset(account, 0);

        // verify fee
        std::string baseFee = to_string(env.current()->fees().base);
        testRPCCall(env, tx, baseFee);
    }

    void
    testWithSpecialTransactionTypes(FeatureBitset features)
    {
        testcase("special transaction types");

        using namespace test::jtx;
        using namespace std::literals;

        Env env{*this, features};

        auto const incReserve = env.current()->fees().increment;

        auto const account = Account("alice");
        auto const bob = Account("bob");
        env.fund(XRP(1000), account, bob);
        env.close();

        // build tx
        auto tx = acctdelete(account, bob);

        // verify fee
        std::string const feeResult = to_string(incReserve);
        testRPCCall(env, tx, feeResult);
    }

    void
    testConditionalEscrowFinish(FeatureBitset features)
    {
        testcase("conditional escrow");

        std::array<std::uint8_t, 4> const fb1 = {{0xA0, 0x02, 0x80, 0x00}};

        std::array<std::uint8_t, 39> const cb1 = {
            {0xA0, 0x25, 0x80, 0x20, 0xE3, 0xB0, 0xC4, 0x42, 0x98, 0xFC,
             0x1C, 0x14, 0x9A, 0xFB, 0xF4, 0xC8, 0x99, 0x6F, 0xB9, 0x24,
             0x27, 0xAE, 0x41, 0xE4, 0x64, 0x9B, 0x93, 0x4C, 0xA4, 0x95,
             0x99, 0x1B, 0x78, 0x52, 0xB8, 0x55, 0x81, 0x01, 0x00}};

        using namespace test::jtx;
        using namespace std::literals;

        Env env{*this, features};

        auto const account = Account("alice");
        env.fund(XRP(1000), account);
        env.close();

        // build tx
        auto const seq1 = env.seq(account);
        Json::Value tx = finish(account, account, seq1);
        tx[jss::Condition] = strHex(cb1);
        tx[jss::Fulfillment] = strHex(fb1);

        // verify fee
        auto const baseFee = env.current()->fees().base;
        std::string expectedFee =
            to_string(baseFee + baseFee * (32 + (fb1.size() / 16)));

        testRPCCall(env, tx, expectedFee);
    }

    void
    testMultisig(FeatureBitset features)
    {
        testcase("multisig");

        using namespace test::jtx;
        using namespace std::literals;

        Env env{*this, features};

        auto const account = Account("alice");

        // signers
        auto const bogie = Account("bogie");
        auto const demon = Account("demon");
        auto const ghost = Account("ghost");
        auto const haunt = Account("haunt");
        auto const jinni = Account("jinni");
        auto const phase = Account("phase");
        auto const shade = Account("shade");
        auto const spirit = Account("spirit");

        env.fund(XRP(1000), account);
        env.close();

        // build tx
        Json::Value tx = fset(account, 0);
        tx[jss::SigningPubKey] = "";

        std::vector<Account> signerAccounts = {
            bogie, demon, ghost, haunt, jinni, phase, shade, spirit};
        for (auto const& account : signerAccounts)
        {
            Json::Value signer;
            signer[jss::Account] = account.human();
            signer[jss::SigningPubKey] = "";
            Json::Value signerOuter;
            signerOuter[jss::Signer] = signer;
            tx[jss::Signers].append(signerOuter);
        }

        // verify fee
        auto const baseFee = env.current()->fees().base;
        std::string expectedFee =
            to_string(baseFee + (signerAccounts.size() * baseFee));
        testRPCCall(env, tx, expectedFee, false);
    }

    void
    testWithFeats(FeatureBitset features)
    {
        testBaseFee(features);
        testWithSpecialTransactionTypes(features);
        testConditionalEscrowFinish(features);
        testMultisig(features);
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

BEAST_DEFINE_TESTSUITE(BaseFee, app, ripple);

}  // namespace test
}  // namespace ripple
