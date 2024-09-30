//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012-2015 Ripple Labs Inc.

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

#include <xrpld/app/ledger/OpenLedger.h>
#include <xrpld/app/main/Application.h>
#include <xrpld/app/misc/TxQ.h>
#include <xrpld/app/tx/detail/Transactor.h>
#include <xrpld/rpc/Context.h>
#include <xrpld/rpc/GRPCHandlers.h>
#include <xrpld/rpc/detail/TransactionSign.h>
#include <xrpl/basics/FeeUnits.h>
#include <xrpl/basics/mulDiv.h>
#include <xrpl/protocol/ErrorCodes.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/RPCErr.h>

namespace ripple {

inline std::optional<XRPAmount>
getTxnFees(RPC::JsonContext const& context)
{
    auto const& params(context.params);
    Json::Value tx_json;  // the tx as a JSON
    if (params.isMember(jss::tx_blob))
    {
        if (params.isMember(jss::tx_json))
        {
            // both `tx_blob` and `tx_json` included
            return std::nullopt;
        }

        auto const blob = context.params[jss::tx_blob];
        if (!blob.isString())
        {
            return std::nullopt;
        }
        auto unHexed = strUnHex(blob.asString());

        if (!unHexed || !unHexed->size())
            return std::nullopt;

        try
        {
            SerialIter sitTrans(makeSlice(*unHexed));
            tx_json = STObject(std::ref(sitTrans), sfGeneric)
                          .getJson(JsonOptions::none);
        }
        catch (std::runtime_error& e)
        {
            return std::nullopt;
        }
    }
    else if (params.isMember(jss::tx_json))
    {
        tx_json = params[jss::tx_json];
        if (!tx_json.isObject())
        {
            return std::nullopt;
        }
    }
    else
    {
        // neither `tx_blob` nor `tx_json` included
        return std::nullopt;
    }

    // basic sanity checks for transaction shape
    if (!tx_json.isMember(jss::TransactionType))
    {
        return std::nullopt;
    }
    if (!tx_json.isMember(jss::Account))
    {
        return std::nullopt;
    }

    return RPC::getBaseFee(context.app, context.app.config(), tx_json);
}

Json::Value
doFee(RPC::JsonContext& context)
{
    // get txn fees, if any
    std::optional<XRPAmount> txnFees;
    try
    {
        txnFees = getTxnFees(context);
    }
    catch (std::exception& e)
    {
        Json::Value jvResult;
        jvResult[jss::error] = "invalidTransaction";
        jvResult[jss::error_exception] = e.what();
        return jvResult;
    }

    auto result = context.app.getTxQ().doRPC(context.app, txnFees);
    if (result.type() == Json::objectValue)
        return result;
    assert(false);
    RPC::inject_error(rpcINTERNAL, context.params);
    return context.params;
}

}  // namespace ripple
