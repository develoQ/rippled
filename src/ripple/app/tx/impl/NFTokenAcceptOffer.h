//------------------------------------------------------------------------------
/*
  This file is part of rippled: https://github.com/ripple/rippled
  Copyright (c) 2021 Ripple Labs Inc.

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

#ifndef RIPPLE_TX_NFTOKENACCEPTOFFER_H_INCLUDED
#define RIPPLE_TX_NFTOKENACCEPTOFFER_H_INCLUDED

#include <ripple/app/tx/impl/Transactor.h>

namespace ripple {

struct PayResult
{
    /// TER for transfering the payment funds
    TER ter;
    // The computed input amount.
    STAmount actualAmountIn;
    // The computed output amount.
    STAmount actualAmountOut;
};

class NFTokenAcceptOffer : public Transactor
{
private:
    PayResult
    pay(AccountID const& from,
        AccountID const& to,
        std::optional<STAmount> const& deliver_amount,
        std::optional<STAmount> const& send_max);

    TER
    acceptOffer(
        std::shared_ptr<SLE> const& offer,
        std::optional<STAmount> acceptAmount);

    TER
    bridgeOffers(
        std::shared_ptr<SLE> const& buy,
        std::shared_ptr<SLE> const& sell);

    TER
    transferNFToken(
        AccountID const& buyer,
        AccountID const& seller,
        uint256 const& nfTokenID);

public:
    static constexpr ConsequencesFactoryType ConsequencesFactory{Normal};

    explicit NFTokenAcceptOffer(ApplyContext& ctx) : Transactor(ctx)
    {
    }

    static NotTEC
    preflight(PreflightContext const& ctx);

    static TER
    preclaim(PreclaimContext const& ctx);

    TER
    doApply() override;
};

}  // namespace ripple

#endif
