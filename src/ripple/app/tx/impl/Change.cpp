//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

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

#include <ripple/app/ledger/Ledger.h>
#include <ripple/app/main/Application.h>
#include <ripple/app/misc/AmendmentTable.h>
#include <ripple/app/misc/NetworkOPs.h>
#include <ripple/app/tx/impl/Change.h>
#include <ripple/basics/Log.h>
#include <ripple/ledger/Sandbox.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/Indexes.h>
#include <ripple/protocol/TxFlags.h>
#include <string_view>
#include <ripple/app/hook/Guard.h> 
#include <ripple/protocol/AccountID.h>
#include <ripple/app/hook/applyHook.h>
#include <ripple/app/hook/xahau.h>

namespace ripple {

NotTEC
Change::preflight(PreflightContext const& ctx)
{
    auto const ret = preflight0(ctx);
    if (!isTesSuccess(ret))
        return ret;

    auto account = ctx.tx.getAccountID(sfAccount);
    if (account != beast::zero)
    {
        JLOG(ctx.j.warn()) << "Change: Bad source id";
        return temBAD_SRC_ACCOUNT;
    }

    // No point in going any further if the transaction fee is malformed.
    auto const fee = ctx.tx.getFieldAmount(sfFee);
    if (!fee.native() || fee != beast::zero)
    {
        JLOG(ctx.j.warn()) << "Change: invalid fee";
        return temBAD_FEE;
    }

    if (!ctx.tx.getSigningPubKey().empty() || !ctx.tx.getSignature().empty() ||
        ctx.tx.isFieldPresent(sfSigners))
    {
        JLOG(ctx.j.warn()) << "Change: Bad signature";
        return temBAD_SIGNATURE;
    }

    if (ctx.tx.getFieldU32(sfSequence) != 0 ||
        ctx.tx.isFieldPresent(sfPreviousTxnID))
    {
        JLOG(ctx.j.warn()) << "Change: Bad sequence";
        return temBAD_SEQUENCE;
    }

    if (ctx.tx.getTxnType() == ttUNL_MODIFY &&
        !ctx.rules.enabled(featureNegativeUNL))
    {
        JLOG(ctx.j.warn()) << "Change: NegativeUNL not enabled";
        return temDISABLED;
    }

    if (ctx.tx.getTxnType() == ttUNL_REPORT)
    {
        if (!ctx.rules.enabled(featureXahauGenesis))
        {
            JLOG(ctx.j.warn()) << "Change: UNLReport is not enabled.";
            return temDISABLED;
        }

        if (!ctx.tx.isFieldPresent(sfActiveValidator) && !ctx.tx.isFieldPresent(sfImportVLKey))
        {
            JLOG(ctx.j.warn()) << "Change: UNLReport must specify at least one of sfImportVLKey, sfActiveValidator";
            return temMALFORMED;
        }
    }

    return tesSUCCESS;
}

TER
Change::preclaim(PreclaimContext const& ctx)
{
    // If tapOPEN_LEDGER is resurrected into ApplyFlags,
    // this block can be moved to preflight.
    if (ctx.view.open())
    {
        JLOG(ctx.j.warn()) << "Change transaction against open ledger";
        return temINVALID;
    }

    switch (ctx.tx.getTxnType())
    {
        case ttFEE:
            if (ctx.view.rules().enabled(featureXRPFees))
            {
                // The ttFEE transaction format defines these fields as
                // optional, but once the XRPFees feature is enabled, they are
                // required.
                if (!ctx.tx.isFieldPresent(sfBaseFeeDrops) ||
                    !ctx.tx.isFieldPresent(sfReserveBaseDrops) ||
                    !ctx.tx.isFieldPresent(sfReserveIncrementDrops))
                    return temMALFORMED;
                // The ttFEE transaction format defines these fields as
                // optional, but once the XRPFees feature is enabled, they are
                // forbidden.
                if (ctx.tx.isFieldPresent(sfBaseFee) ||
                    ctx.tx.isFieldPresent(sfReferenceFeeUnits) ||
                    ctx.tx.isFieldPresent(sfReserveBase) ||
                    ctx.tx.isFieldPresent(sfReserveIncrement))
                    return temMALFORMED;
            }
            else
            {
                // The ttFEE transaction format formerly defined these fields
                // as required. When the XRPFees feature was implemented, they
                // were changed to be optional. Until the feature has been
                // enabled, they are required.
                if (!ctx.tx.isFieldPresent(sfBaseFee) ||
                    !ctx.tx.isFieldPresent(sfReferenceFeeUnits) ||
                    !ctx.tx.isFieldPresent(sfReserveBase) ||
                    !ctx.tx.isFieldPresent(sfReserveIncrement))
                    return temMALFORMED;
                // The ttFEE transaction format defines these fields as
                // optional, but without the XRPFees feature, they are
                // forbidden.
                if (ctx.tx.isFieldPresent(sfBaseFeeDrops) ||
                    ctx.tx.isFieldPresent(sfReserveBaseDrops) ||
                    ctx.tx.isFieldPresent(sfReserveIncrementDrops))
                    return temDISABLED;
            }
            return tesSUCCESS;
        case ttAMENDMENT:
        case ttUNL_MODIFY:
        case ttUNL_REPORT:
        case ttEMIT_FAILURE:
            return tesSUCCESS;
        default:
            return temUNKNOWN;
    }
}

TER
Change::doApply()
{
    switch (ctx_.tx.getTxnType())
    {
        case ttAMENDMENT:
            return applyAmendment();
        case ttFEE:
            return applyFee();
        case ttUNL_MODIFY:
            return applyUNLModify();
        case ttEMIT_FAILURE:
            return applyEmitFailure();
        case ttUNL_REPORT:
            return applyUNLReport();
        default:
            assert(0);
            return tefFAILURE;
    }
}

TER
Change::applyUNLReport()
{
    auto sle = view().peek(keylet::UNLReport());

    auto const seq = view().info().seq;

    bool const created = !sle;

    if (created)
        sle = std::make_shared<SLE>(keylet::UNLReport());
    
    bool const reset =
        sle->isFieldPresent(sfPreviousTxnLgrSeq) &&
            sle->getFieldU32(sfPreviousTxnLgrSeq) < seq;

    auto canonicalize = [&](SField const& arrayType, SField const& objType)
        -> std::vector<STObject>
    {
        auto const existing = 
            reset || !sle->isFieldPresent(arrayType)
            ? STArray(arrayType)
            : sle->getFieldArray(arrayType);

        // canonically order using std::set
        std::map<PublicKey, AccountID> ordered;
        for (auto const& obj: existing)
        {
            auto pk = obj.getFieldVL(sfPublicKey);
            if (!publicKeyType(makeSlice(pk)))
                continue;

            PublicKey p(makeSlice(pk));
            ordered.emplace(p,
                    obj.isFieldPresent(sfAccount) ? obj.getAccountID(sfAccount) : calcAccountID(p));
        };

        if (ctx_.tx.isFieldPresent(objType))
        {
            auto pk = 
                const_cast<ripple::STTx&>(ctx_.tx)
                    .getField(objType)
                    .downcast<STObject>()
                    .getFieldVL(sfPublicKey);

            if (publicKeyType(makeSlice(pk)))
            {
                PublicKey p(makeSlice(pk));
                ordered.emplace(p, calcAccountID(p));
            }
        }

        std::vector<STObject> out;
        out.reserve(ordered.size());
        for (auto const& [k, a]: ordered)
        {
            out.emplace_back(objType);
            out.back().setFieldVL(sfPublicKey, k);
            out.back().setAccountID(sfAccount, a);
        }

        return out;
    };


    bool const hasAV = ctx_.tx.isFieldPresent(sfActiveValidator);
    bool const hasVL = ctx_.tx.isFieldPresent(sfImportVLKey);

    // update
    if (hasAV)
        sle->setFieldArray(sfActiveValidators,
            STArray(canonicalize(sfActiveValidators, sfActiveValidator),sfActiveValidators));

    if (hasVL)
        sle->setFieldArray(sfImportVLKeys,
            STArray(canonicalize(sfImportVLKeys, sfImportVLKey),sfImportVLKeys));

    if (created)
        view().insert(sle);
    else
        view().update(sle);

    return tesSUCCESS;
}

void
Change::preCompute()
{
    assert(account_ == beast::zero);
}


inline
std::pair<
    std::map<std::string, XRPAmount>,
    std::map<std::vector<uint8_t>, std::vector<uint8_t>>>
normalizeXahauGenesis(
        std::map<std::string, XRPAmount> const& entries,
        std::map<std::vector<uint8_t>, std::vector<uint8_t>> params,
        beast::Journal const& j)
{
    std::map<std::string, XRPAmount> amounts;
    uint8_t mc = 0;
    for (auto const& [rn, x]: entries)
    {
        if (rn.c_str()[0] != 'n')
        {
            amounts.emplace(rn, x);
            continue;
        }

        auto const pk = parseBase58<PublicKey>(TokenType::NodePublic, rn);
        if (!pk)
        {
            JLOG(j.warn())
                << "featureXahauGenesis could not parse nodepub: " << rn;
            continue;
        }

        AccountID id = calcAccountID(*pk);
        std::string idStr = toBase58(id);
        amounts.emplace(idStr, x);
        JLOG(j.warn())
            << "featureXahauGenesis: "
            << "initial validator: " << rn
            << " =>accid: " << idStr;

        // initial member enumeration
        params.emplace(
                std::vector<uint8_t>{'I', 'M', mc++},
                std::vector<uint8_t>(id.data(), id.data() + 20));
    }

    // initial member count
    params.emplace(
            std::vector<uint8_t>{'I', 'M', 'C'},
            std::vector<uint8_t>{mc});

    return {amounts, params};
};


void
Change::activateXahauGenesis()
{
    JLOG(j_.warn()) << "featureXahauGenesis amendment activation code starting";

    constexpr XRPAmount GENESIS { 1'000'000 * DROPS_PER_XRP };
    constexpr XRPAmount INFRA   { 10'000'000 * DROPS_PER_XRP};
    constexpr XRPAmount EXCHANGE { 2'000'000 * DROPS_PER_XRP};

    auto [initial_distribution, governance_hook_params] =
    normalizeXahauGenesis({
        // distribution targets and initial validators
        // where a nodepub is specified then that is an initial governance member
        // where an r-addr is specified they still get an initial distribution but don't go into the L1 table
        {"rMYm3TY5D3rXYVAz6Zr2PDqEcjsTYbNiAT", INFRA},
        {"nHUG6WyZX5C6YPhxDEUiFdvRsWvbxdXuUcMkMxuqS9C3akrJtJQA", INFRA},
        {"nHDDs26hxCgh74A6QE31CR5QoC17yXdJQXNDXezp8HW93mCYGPG7", INFRA},
        {"nHUNFRAhqbfqBHYxfiAtJDxruSgbsEBUHR6v55MhdUtzTNyXLcR4", INFRA},
        {"nHB4MVtevJBZF4vfdLTecKBxj5KsxERkfk7UNyL9iYtTZvjmMBXw", INFRA},
        {"nHUB9Fh1JXvyMY4NhiCKgg6pkGrB3FoBTAz4dpvKC1fwCMjY1w5N", INFRA},
        {"nHUdqajJr8S1ecKwqVkX4gQNUzQP9RTonZeEZH8vwg7664CZP3QF", INFRA},
        {"nHDnr7GgwZWS7Qb517e5is3pxwVxsNgmmpmQYvrc1ngbPiURBa6B", INFRA},
        {"nHBv6AqLDgWgEVLoNE7jEViv4XG17jj6tpuzTFm664Cc4mcpEgwb", INFRA},
        {"nHUxeL9jgcjhTWepmFnbWpmobZmFBduLkceQddCJnAPghJejdRix", INFRA},
        {"nHUubQ7fqxkwPtwS4pQb2ENZ6fdUcAt7aJRiYcPXjxbbkC778Zjk", INFRA},
    },
    // hook params
    {
        // initial reward rate is 1.003274 per month
        // 1.003274 -xfl-> 6089869970204910592 -le-> 0x00E461EE78908354
        {{'I', 'R', 'R'}, {0x00U, 0xE4U, 0x61U, 0xEEU, 0x78U, 0x90U, 0x83U, 0x54U}},

        // initial reward delay is 365*24*60*60/12 = 2628000 = 0x2819A0 -LEu64-> A019280000000000
        {{'I', 'R', 'D'}, {0xA0U, 0x19U, 0x28U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U}}

        // other params are populated insie the noramlization function
    }, j_);

    const static std::vector<
        std::tuple<
            uint256,                        // hook on
            std::vector<uint8_t>,           // hook code
            std::map<
                std::vector<uint8_t>,       // param name
                std::vector<uint8_t>>>>     // param value
    genesis_hooks =
    {
        {
            // For the Governance Hook: HookOn is set to ttINVOKE only
            ripple::uint256("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF7FFFFFFFFFFFFFFFFFFBFFFFF"),
            XAHAU_GOVERNANCE_HOOK,
            governance_hook_params
        },
        
        {
            // For the Reward Hook: HookOn is set to ttCLAIM_REWARD only
            ripple::uint256("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFBFFFFFFFFFFFFFFFFFFBFFFFF"),
            XAHAU_REWARD_HOOK,
            {}  // no params for this hook
        }
    };


    Sandbox sb(&view());

    // Step 1: mint genesis distribution
    for (auto const& [account, amount] : initial_distribution)
    {
        auto accid_raw = parseBase58<AccountID>(account);
        if (!accid_raw)
        {
            JLOG(j_.warn())
                << "featureXahauGenesis could not parse an r-address: " << account;
            continue;
        }

        auto accid = *accid_raw;

        auto const kl = keylet::account(accid);

        auto sle = sb.peek(kl);
        auto const exists = !!sle;

        STAmount bal = exists ? sle->getFieldAmount(sfBalance) + STAmount{amount} : STAmount{amount};
        if (bal <= beast::zero)
        {
            JLOG(j_.warn())
                << "featureXahauGenesis tried to set <= 0 balance on " <<  account << ", bailing";
            return;
        }

        // the account should not exist but if it does then handle it properly
        if (!exists)
        {
            sle = std::make_shared<SLE>(kl);
            sle->setAccountID(sfAccount, accid);

            std::uint32_t const seqno{
                sb.rules().enabled(featureDeletableAccounts) ? sb.seq()
                                                             : 1};
            sle->setFieldU32(sfSequence, seqno);

        }

        sle->setFieldAmount(sfBalance, bal);

        if (exists)
            sb.update(sle);
        else
            sb.insert(sle);

    };

    // Step 2: burn genesis funds to (almost) zero
    static auto const accid = calcAccountID(
        generateKeyPair(KeyType::secp256k1, generateSeed("masterpassphrase"))
            .first);

    auto const kl = keylet::account(accid);
    auto sle = sb.peek(kl);
    if (!sle)
    {
        JLOG(j_.warn())
            << "featureXahauGenesis genesis account doesn't exist!!";

        return;
    }

    sle->setFieldAmount(sfBalance, GENESIS);

    // Step 3: blackhole genesis
    sle->setAccountID(sfRegularKey, noAccount());
    sle->setFieldU32(sfFlags, lsfDisableMaster);
       

    // Step 4: install genesis hooks
    sle->setFieldU32(sfOwnerCount, sle->getFieldU32(sfOwnerCount) + genesis_hooks.size());
    sb.update(sle);

    if (sb.exists(keylet::hook(accid)))
    {
        JLOG(j_.warn())
            << "featureXahauGenesis genesis account already has hooks object in ledger, bailing";
        return;
    }

    {
        ripple::STArray hooks{sfHooks, genesis_hooks.size()};
        int hookCount = 0;

        for (auto const& [hookOn, wasmBytes, params] : genesis_hooks)
        {

            std::ostringstream loggerStream;
            auto result =
                validateGuards(
                    wasmBytes,   // wasm to verify
                    loggerStream,
                    "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh"
                );

            if (!result)
            {
                std::string s = loggerStream.str();

                char* data = s.data();
                size_t len = s.size();

                char* last = data;
                size_t i = 0;
                for (; i < len; ++i)
                {
                    if (data[i] == '\n')
                    {
                        data[i] = '\0';
                        j_.warn() << last;
                        last = data + i;
                    }
                }

                if (last < data + i)
                    j_.warn() << last;

                JLOG(j_.warn())
                    << "featureXahauGenesis initial hook failed to validate guards, bailing";
                    
                return;
            }

            std::optional<std::string> result2 =
                hook::HookExecutor::validateWasm(wasmBytes.data(), (size_t)wasmBytes.size());

            if (result2)
            {
                JLOG(j_.warn())
                    << "featureXahauGenesis tried to set a hook with invalid code. VM error: "
                    << *result2 << ", bailing";
                return;
            }

            auto hookHash = ripple::sha512Half_s(ripple::Slice(wasmBytes.data(), wasmBytes.size()));
            auto const kl = keylet::hookDefinition(hookHash);
            if (view().exists(kl))
            {
                JLOG(j_.warn())
                    << "featureXahauGenesis genesis hookDefinition already exists !!! bailing";
                return;
            }

            auto hookDef = std::make_shared<SLE>(kl);

            hookDef->setFieldH256(sfHookHash, hookHash);
            hookDef->setFieldH256(sfHookOn, hookOn);
            hookDef->setFieldH256(sfHookNamespace, UINT256_BIT[hookCount++]);

            // parameters
            {
                std::vector<STObject> vec;
                for (auto const& [k, v]: params)
                {
                    STObject param(sfHookParameter);
                    param.setFieldVL(sfHookParameterName, k);
                    param.setFieldVL(sfHookParameterValue, v);
                    vec.emplace_back(std::move(param));
                };
            
                hookDef->setFieldArray(sfHookParameters, STArray(vec, sfHookParameters));
            }

            hookDef->setFieldU8(sfHookApiVersion, 0);
            hookDef->setFieldVL(sfCreateCode, wasmBytes);
            hookDef->setFieldH256(sfHookSetTxnID,  ctx_.tx.getTransactionID());
            hookDef->setFieldU64(sfReferenceCount, 1);
            hookDef->setFieldAmount(sfFee,
                    XRPAmount {hook::computeExecutionFee(result->first)});
            if (result->second > 0)
                hookDef->setFieldAmount(sfHookCallbackFee, 
                    XRPAmount {hook::computeExecutionFee(result->second)});

            sb.insert(hookDef);

            STObject hookObj {sfHook};
            hookObj.setFieldH256(sfHookHash, hookHash);
            hooks.push_back(hookObj);

        }

        auto sle = std::make_shared<SLE>(keylet::hook(accid));
        sle->setFieldArray(sfHooks, hooks);
        sle->setAccountID(sfAccount, accid);

        auto const page = sb.dirInsert(
            keylet::ownerDir(accid),
            keylet::hook(accid),
            describeOwnerDir(accid));

        if (!page)
        {
            JLOG(j_.warn())
                << "featureXahauGenesis genesis directory full when trying to insert hooks object, bailing";
            return;
        }
        sle->setFieldU64(sfOwnerNode, *page);
        sb.insert(sle);
    }

    JLOG(j_.warn()) << "featureXahauGenesis amendment executed successfully";
    
    sb.apply(ctx_.rawView());
}

void
Change::activateTrustLinesToSelfFix()
{
    JLOG(j_.warn()) << "fixTrustLinesToSelf amendment activation code starting";

    auto removeTrustLineToSelf = [this](Sandbox& sb, uint256 id) {
        auto tl = sb.peek(keylet::child(id));

        if (tl == nullptr)
        {
            JLOG(j_.warn()) << id << ": Unable to locate trustline";
            return true;
        }

        if (tl->getType() != ltRIPPLE_STATE)
        {
            JLOG(j_.warn()) << id << ": Unexpected type "
                            << static_cast<std::uint16_t>(tl->getType());
            return true;
        }

        auto const& lo = tl->getFieldAmount(sfLowLimit);
        auto const& hi = tl->getFieldAmount(sfHighLimit);

        if (lo != hi)
        {
            JLOG(j_.warn()) << id << ": Trustline doesn't meet requirements";
            return true;
        }

        if (auto const page = tl->getFieldU64(sfLowNode); !sb.dirRemove(
                keylet::ownerDir(lo.getIssuer()), page, tl->key(), false))
        {
            JLOG(j_.error()) << id << ": failed to remove low entry from "
                             << toBase58(lo.getIssuer()) << ":" << page
                             << " owner directory";
            return false;
        }

        if (auto const page = tl->getFieldU64(sfHighNode); !sb.dirRemove(
                keylet::ownerDir(hi.getIssuer()), page, tl->key(), false))
        {
            JLOG(j_.error()) << id << ": failed to remove high entry from "
                             << toBase58(hi.getIssuer()) << ":" << page
                             << " owner directory";
            return false;
        }

        if (tl->getFlags() & lsfLowReserve)
            adjustOwnerCount(
                sb, sb.peek(keylet::account(lo.getIssuer())), -1, j_);

        if (tl->getFlags() & lsfHighReserve)
            adjustOwnerCount(
                sb, sb.peek(keylet::account(hi.getIssuer())), -1, j_);

        sb.erase(tl);

        JLOG(j_.warn()) << "Successfully deleted trustline " << id;

        return true;
    };

    using namespace std::literals;

    Sandbox sb(&view());

    if (removeTrustLineToSelf(
            sb,
            uint256{
                "2F8F21EFCAFD7ACFB07D5BB04F0D2E18587820C7611305BB674A64EAB0FA71E1"sv}) &&
        removeTrustLineToSelf(
            sb,
            uint256{
                "326035D5C0560A9DA8636545DD5A1B0DFCFF63E68D491B5522B767BB00564B1A"sv}))
    {
        JLOG(j_.warn()) << "fixTrustLinesToSelf amendment activation code "
                           "executed successfully";
        sb.apply(ctx_.rawView());
    }
}

TER
Change::applyAmendment()
{
    uint256 amendment(ctx_.tx.getFieldH256(sfAmendment));

    auto const k = keylet::amendments();

    SLE::pointer amendmentObject = view().peek(k);

    if (!amendmentObject)
    {
        amendmentObject = std::make_shared<SLE>(k);
        view().insert(amendmentObject);
    }

    STVector256 amendments = amendmentObject->getFieldV256(sfAmendments);

    if (std::find(amendments.begin(), amendments.end(), amendment) !=
        amendments.end())
        return tefALREADY;

    auto flags = ctx_.tx.getFlags();

    const bool gotMajority = (flags & tfGotMajority) != 0;
    const bool lostMajority = (flags & tfLostMajority) != 0;

    if (gotMajority && lostMajority)
        return temINVALID_FLAG;

    STArray newMajorities(sfMajorities);

    bool found = false;
    if (amendmentObject->isFieldPresent(sfMajorities))
    {
        const STArray& oldMajorities =
            amendmentObject->getFieldArray(sfMajorities);
        for (auto const& majority : oldMajorities)
        {
            if (majority.getFieldH256(sfAmendment) == amendment)
            {
                if (gotMajority)
                    return tefALREADY;
                found = true;
            }
            else
            {
                // pass through
                newMajorities.push_back(majority);
            }
        }
    }

    if (!found && lostMajority)
        return tefALREADY;

    if (gotMajority)
    {
        // This amendment now has a majority
        newMajorities.push_back(STObject(sfMajority));
        auto& entry = newMajorities.back();
        entry.emplace_back(STUInt256(sfAmendment, amendment));
        entry.emplace_back(STUInt32(
            sfCloseTime, view().parentCloseTime().time_since_epoch().count()));

        if (!ctx_.app.getAmendmentTable().isSupported(amendment))
        {
            JLOG(j_.warn()) << "Unsupported amendment " << amendment
                            << " received a majority.";
        }
    }
    else if (!lostMajority)
    {
        // No flags, enable amendment
        amendments.push_back(amendment);
        amendmentObject->setFieldV256(sfAmendments, amendments);

        if (amendment == fixTrustLinesToSelf)
            activateTrustLinesToSelfFix();
        else if (amendment == featureXahauGenesis)
            activateXahauGenesis();

        ctx_.app.getAmendmentTable().enable(amendment);

        if (!ctx_.app.getAmendmentTable().isSupported(amendment))
        {
            JLOG(j_.error()) << "Unsupported amendment " << amendment
                             << " activated: server blocked.";
            ctx_.app.getOPs().setAmendmentBlocked();
        }
    }

    if (newMajorities.empty())
        amendmentObject->makeFieldAbsent(sfMajorities);
    else
        amendmentObject->setFieldArray(sfMajorities, newMajorities);

    view().update(amendmentObject);

    return tesSUCCESS;
}

TER
Change::applyFee()
{
    auto const k = keylet::fees();

    SLE::pointer feeObject = view().peek(k);

    if (!feeObject)
    {
        feeObject = std::make_shared<SLE>(k);
        view().insert(feeObject);
    }
    auto set = [](SLE::pointer& feeObject, STTx const& tx, auto const& field) {
        feeObject->at(field) = tx[field];
    };
    if (view().rules().enabled(featureXRPFees))
    {
        set(feeObject, ctx_.tx, sfBaseFeeDrops);
        set(feeObject, ctx_.tx, sfReserveBaseDrops);
        set(feeObject, ctx_.tx, sfReserveIncrementDrops);
        // Ensure the old fields are removed
        feeObject->makeFieldAbsent(sfBaseFee);
        feeObject->makeFieldAbsent(sfReferenceFeeUnits);
        feeObject->makeFieldAbsent(sfReserveBase);
        feeObject->makeFieldAbsent(sfReserveIncrement);
    }
    else
    {
        set(feeObject, ctx_.tx, sfBaseFee);
        set(feeObject, ctx_.tx, sfReferenceFeeUnits);
        set(feeObject, ctx_.tx, sfReserveBase);
        set(feeObject, ctx_.tx, sfReserveIncrement);
    }

    view().update(feeObject);

    JLOG(j_.warn()) << "Fees have been changed";
    return tesSUCCESS;
}

TER
Change::applyEmitFailure()
{
    uint256 txnID(ctx_.tx.getFieldH256(sfTransactionHash));
    do
    {
        JLOG(j_.warn())
            << "HookEmit[" << txnID << "]: ttEmitFailure removing emitted txn";

        auto key = keylet::emittedTxn(txnID);

        auto const& sle = view().peek(key);

        if (!sle)
        {
            // RH NOTE: This will now be the normal execution path, the alternative will only occur if something
            // went really wrong with the hook callback
//            JLOG(j_.warn())
//                << "HookError[" << txnID << "]: ttEmitFailure (Change) tried to remove already removed emittedtxn";
            break;
        }

        if (!view().dirRemove(
                keylet::emittedDir(),
                sle->getFieldU64(sfOwnerNode),
                key,
                false))
        {
            JLOG(j_.fatal())
                << "HookError[" << txnID << "]: ttEmitFailure (Change) tefBAD_LEDGER";
            return tefBAD_LEDGER;
        }

        view().erase(sle);
    } while (0);
    return tesSUCCESS;
}

TER
Change::applyUNLModify()
{
    if (!isFlagLedger(view().seq()))
    {
        JLOG(j_.warn()) << "N-UNL: applyUNLModify, not a flag ledger, seq="
                        << view().seq();
        return tefFAILURE;
    }

    if (!ctx_.tx.isFieldPresent(sfUNLModifyDisabling) ||
        ctx_.tx.getFieldU8(sfUNLModifyDisabling) > 1 ||
        !ctx_.tx.isFieldPresent(sfLedgerSequence) ||
        !ctx_.tx.isFieldPresent(sfUNLModifyValidator))
    {
        JLOG(j_.warn()) << "N-UNL: applyUNLModify, wrong Tx format.";
        return tefFAILURE;
    }

    bool const disabling = ctx_.tx.getFieldU8(sfUNLModifyDisabling);
    auto const seq = ctx_.tx.getFieldU32(sfLedgerSequence);
    if (seq != view().seq())
    {
        JLOG(j_.warn()) << "N-UNL: applyUNLModify, wrong ledger seq=" << seq;
        return tefFAILURE;
    }

    Blob const validator = ctx_.tx.getFieldVL(sfUNLModifyValidator);
    if (!publicKeyType(makeSlice(validator)))
    {
        JLOG(j_.warn()) << "N-UNL: applyUNLModify, bad validator key";
        return tefFAILURE;
    }

    JLOG(j_.info()) << "N-UNL: applyUNLModify, "
                    << (disabling ? "ToDisable" : "ToReEnable")
                    << " seq=" << seq
                    << " validator data:" << strHex(validator);

    auto const k = keylet::negativeUNL();
    SLE::pointer negUnlObject = view().peek(k);
    if (!negUnlObject)
    {
        negUnlObject = std::make_shared<SLE>(k);
        view().insert(negUnlObject);
    }

    bool const found = [&] {
        if (negUnlObject->isFieldPresent(sfDisabledValidators))
        {
            auto const& negUnl =
                negUnlObject->getFieldArray(sfDisabledValidators);
            for (auto const& v : negUnl)
            {
                if (v.isFieldPresent(sfPublicKey) &&
                    v.getFieldVL(sfPublicKey) == validator)
                    return true;
            }
        }
        return false;
    }();

    if (disabling)
    {
        // cannot have more than one toDisable
        if (negUnlObject->isFieldPresent(sfValidatorToDisable))
        {
            JLOG(j_.warn()) << "N-UNL: applyUNLModify, already has ToDisable";
            return tefFAILURE;
        }

        // cannot be the same as toReEnable
        if (negUnlObject->isFieldPresent(sfValidatorToReEnable))
        {
            if (negUnlObject->getFieldVL(sfValidatorToReEnable) == validator)
            {
                JLOG(j_.warn())
                    << "N-UNL: applyUNLModify, ToDisable is same as ToReEnable";
                return tefFAILURE;
            }
        }

        // cannot be in negative UNL already
        if (found)
        {
            JLOG(j_.warn())
                << "N-UNL: applyUNLModify, ToDisable already in negative UNL";
            return tefFAILURE;
        }

        negUnlObject->setFieldVL(sfValidatorToDisable, validator);
    }
    else
    {
        // cannot have more than one toReEnable
        if (negUnlObject->isFieldPresent(sfValidatorToReEnable))
        {
            JLOG(j_.warn()) << "N-UNL: applyUNLModify, already has ToReEnable";
            return tefFAILURE;
        }

        // cannot be the same as toDisable
        if (negUnlObject->isFieldPresent(sfValidatorToDisable))
        {
            if (negUnlObject->getFieldVL(sfValidatorToDisable) == validator)
            {
                JLOG(j_.warn())
                    << "N-UNL: applyUNLModify, ToReEnable is same as ToDisable";
                return tefFAILURE;
            }
        }

        // must be in negative UNL
        if (!found)
        {
            JLOG(j_.warn())
                << "N-UNL: applyUNLModify, ToReEnable is not in negative UNL";
            return tefFAILURE;
        }

        negUnlObject->setFieldVL(sfValidatorToReEnable, validator);
    }

    view().update(negUnlObject);
    return tesSUCCESS;
}

}  // namespace ripple
