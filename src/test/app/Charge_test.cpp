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

#include <ripple/basics/BasicConfig.h>
#include <ripple/core/ConfigSections.h>
#include <ripple/protocol/jss.h>
#include <test/jtx.h>
#include <test/jtx/Env.h>

namespace ripple {
namespace test {

class Charge_test : public beast::unit_test::suite
{
public:
    void
    run() override
    {
        using namespace jtx;
        static FeatureBitset const all{supported_amendments()};

        testCharge(all);
    }

    XRPAmount
    reserve(jtx::Env& env, std::uint32_t count) const
    {
        return env.current()->fees().accountReserve(count);
    }

    void
    testCharge(FeatureBitset features)
    {
        using namespace jtx;
        testcase("Charge");

        Account const alice("alice");
        Account const bob("bob");
        Account const gw("gw");
        Account const noacc("noacc");
        auto const USD = gw["USD"];

        {
            // Native Token
            // With Transaction
            Env env{*this};

            auto const fee = env.current()->fees().base;
            env.fund(XRP(1000), alice, bob);
            env.close();

            env(noop(alice), charge(bob, XRP(-1)), ter(temBAD_AMOUNT));
            env(noop(alice), charge(bob, XRP(0)), ter(temBAD_AMOUNT));
            env(noop(alice), charge(alice, XRP(1)), ter(temDST_IS_SRC));
            env(noop(alice), charge(noacc, XRP(1)), ter(tecNO_DST));
            env.close();

            env(pay(env.master, alice, drops(fee)));
            env.close();

            env(noop(alice),
                charge(bob, XRP(1001) - reserve(env, 0)),
                ter(tecUNFUNDED_PAYMENT));
            env.close();
            env(pay(env.master, alice, drops(fee)));
            env.close();

            // AccountSet
            BEAST_EXPECT(env.balance(alice, XRP) == XRP(1000));
            BEAST_EXPECT(env.balance(bob, XRP) == XRP(1000));
            env(noop(alice), charge(bob, XRP(1)));
            env.close();
            BEAST_EXPECT(env.balance(alice, XRP) == XRP(1000 - 1) - fee);
            BEAST_EXPECT(env.balance(bob, XRP) == XRP(1000 + 1));

            env(pay(env.master, alice, drops(fee)));
            env.close();

            // Payment
            BEAST_EXPECT(env.balance(alice, XRP) == XRP(999));
            BEAST_EXPECT(env.balance(bob, XRP) == XRP(1001));
            env(pay(alice, bob, XRP(10)), charge(bob, XRP(1)));
            env.close();
            BEAST_EXPECT(env.balance(alice, XRP) == XRP(999 - 10 - 1) - fee);
            BEAST_EXPECT(env.balance(bob, XRP) == XRP(1001 + 10 + 1));
        }

        {
            // IOU Token
            Env env{*this};
            env.fund(XRP(1000), alice, bob, gw);
            env.close();

            // No Trustline
            env(noop(gw), charge(alice, USD(1)), ter(tecNO_LINE));
            env(noop(alice), charge(gw, USD(1)), ter(tecNO_LINE));
            env(noop(alice), charge(bob, USD(1)), ter(tecNO_LINE));
            env.close();

            env(trust(alice, USD(1000)));
            env(trust(bob, USD(1000)));
            env.close();

            // AccountSet
            env(pay(gw, alice, USD(1001)), ter(tecPATH_PARTIAL));
            env(noop(gw), charge(alice, USD(1001)), ter(tecPATH_PARTIAL));

            BEAST_EXPECT(env.balance(alice, USD) == USD(0));
            env(noop(gw), charge(alice, USD(1)));
            env.close();
            BEAST_EXPECT(env.balance(alice, USD) == USD(1));

            // Freeze
            env(trust(gw, alice["USD"](0), tfSetFreeze));
            env.close();
            env(noop(alice), charge(bob, USD(1)), ter(tecFROZEN));
            env(noop(gw), charge(alice, USD(1)));
            env(noop(alice), charge(gw, USD(1)));
            env.close();
            env(trust(gw, alice["USD"](0), tfClearFreeze));
            env.close();

            // Global Freeze
            env(fset(gw, asfGlobalFreeze));
            env.close();
            env(noop(alice), charge(bob, USD(1)), ter(tecFROZEN));
            env(noop(gw), charge(alice, USD(1)));
            env(noop(alice), charge(gw, USD(1)));
            env.close();
            env(fclear(gw, asfGlobalFreeze));
            env.close();

            // TransferFee
            env(rate(gw, 1.25));
            env.close();
            env(noop(gw), charge(bob, USD(1)));
            BEAST_EXPECT(env.balance(bob, USD) == USD(1));
            env(noop(bob), charge(gw, USD(1)));
            BEAST_EXPECT(env.balance(bob, USD) == USD(0));

            BEAST_EXPECT(env.balance(alice, USD) == USD(1));
            env(noop(alice), charge(bob, USD(1)));
            BEAST_EXPECT(env.balance(alice, USD) == USD(0));
            BEAST_EXPECT(env.balance(bob, USD) == USD(0.8));
            env(pay(gw, alice, USD(1)));
            env(pay(bob, gw, USD(0.8)));
            env.close();
            env(rate(gw, 1));
            env.close();

            // Payment
            // from issuer
            env(pay(gw, alice, USD(1)),
                charge(alice, USD(1000)),
                ter(tecPATH_PARTIAL));
            env(pay(gw, alice, USD(1000)),
                charge(alice, USD(1)),
                ter(tecPATH_PARTIAL));
            env.close();
            BEAST_EXPECT(env.balance(alice, USD) == USD(1));
            env(pay(gw, alice, USD(4)), charge(alice, USD(5)));
            env.close();
            BEAST_EXPECT(env.balance(alice, USD) == USD(10));

            // to issuer
            env(pay(alice, gw, USD(1)),
                charge(gw, USD(11)),
                ter(tecPATH_PARTIAL));
            env(pay(alice, gw, USD(10)),
                charge(gw, USD(1)),
                ter(tecPATH_PARTIAL));
            env.close();
            BEAST_EXPECT(env.balance(alice, USD) == USD(10));
            env(pay(alice, gw, USD(1)), charge(gw, USD(9)));
            env.close();
            BEAST_EXPECT(env.balance(alice, USD) == USD(0));

            // from non-issuer
            env(pay(gw, alice, USD(10)));
            env(pay(alice, bob, USD(1)),
                charge(bob, USD(11)),
                ter(tecPATH_PARTIAL));
            env(pay(alice, bob, USD(10)),
                charge(bob, USD(1)),
                ter(tecPATH_PARTIAL));
            env.close();
            BEAST_EXPECT(env.balance(alice, USD) == USD(10));
            BEAST_EXPECT(env.balance(bob, USD) == USD(0));
            env(pay(alice, bob, USD(1)), charge(bob, USD(1)));
            env.close();
            BEAST_EXPECT(env.balance(alice, USD) == USD(8));
            BEAST_EXPECT(env.balance(bob, USD) == USD(2));
        }
    }
};

BEAST_DEFINE_TESTSUITE(Charge, tx, ripple);

}  // namespace test
}  // namespace ripple
