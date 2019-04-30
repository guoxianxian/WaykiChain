#include <boost/assign/list_of.hpp>
#include <boost/foreach.hpp>
#include <algorithm>

#include "json/json_spirit_value.h"
#include "json/json_spirit_writer_template.h"
#include "json/json_spirit_utils.h"

#include "commons/serialize.h"
#include "tx.h"
#include "txdb.h"
#include "crypto/hash.h"
#include "util.h"
#include "database.h"
#include "main.h"
#include "vm/vmrunenv.h"
#include "core.h"
#include "miner/miner.h"
#include "version.h"

using namespace json_spirit;

const string COperVoteFund::voteOperTypeArray[3] = {"NULL_OPER", "ADD_FUND", "MINUS_FUND"};

string GetTxType(unsigned char txType) {
    auto it = kTxTypeMap.find(txType);
    if (it != kTxTypeMap.end())
        return it->second;
    else
        return "";
}

static bool GetKeyId(const CAccountViewCache &view, const vector<unsigned char> &ret,
                     CKeyID &KeyId) {
    if (ret.size() == 6) {
        CRegID regId(ret);
        KeyId = regId.GetKeyId(view);
    } else if (ret.size() == 34) {
        string addr(ret.begin(), ret.end());
        KeyId = CKeyID(addr);
    } else {
        return false;
    }

    if (KeyId.IsEmpty()) return false;

    return true;
}

bool CBaseTx::IsValidHeight(int nCurrHeight, int nTxCacheHeight) const {
    if(REWARD_TX == nTxType)
        return true;

    if (nValidHeight > nCurrHeight + nTxCacheHeight / 2)
        return false;

    if (nValidHeight < nCurrHeight - nTxCacheHeight / 2)
        return false;

    return true;
}

uint64_t CBaseTx::GetFuel(int nfuelRate) {
    uint64_t llFuel = ceil(nRunStep/100.0f) * nfuelRate;
    if (REG_CONT_TX == nTxType) {
        if (llFuel < 1 * COIN) {
            llFuel = 1 * COIN;
        }
    }
    return llFuel;
}

int CBaseTx::GetFuelRate(CScriptDBViewCache &scriptDB) {
    if (nFuelRate > 0)
        return nFuelRate;

    CDiskTxPos postx;
    if (scriptDB.ReadTxIndex(GetHash(), postx)) {
        CAutoFile file(OpenBlockFile(postx, true), SER_DISK, CLIENT_VERSION);
        CBlockHeader header;
        try {
            file >> header;
        } catch (std::exception &e) {
            return ERRORMSG("%s : Deserialize or I/O error - %s", __func__, e.what());
        }
        nFuelRate = header.GetFuelRate();
    } else {
        nFuelRate = GetElementForBurn(chainActive.Tip());
    }

    return nFuelRate;
}

// check the fees must be more than nMinTxFee
bool CBaseTx::CheckMinTxFee(const uint64_t llFees) const {
    if (GetFeatureForkVersion(chainActive.Tip()->nHeight) == MAJOR_VER_R2 )
        return llFees >= nMinTxFee;

    return true;
}

// transactions should check the signagure size before verifying signature
bool CBaseTx::CheckSignatureSize(const vector<unsigned char> &signature) const {
    return signature.size() > 0 && signature.size() < MAX_BLOCK_SIGNATURE_SIZE;
}

bool CRegisterAccountTx::ExecuteTx(int nIndex, CAccountViewCache &view, CValidationState &state,
                                   CTxUndo &txundo, int nHeight, CTransactionDBCache &txCache,
                                   CScriptDBViewCache &scriptDB) {
    CAccount account;
    CRegID regId(nHeight, nIndex);
    CKeyID keyId = userId.get<CPubKey>().GetKeyId();
    if (!view.GetAccount(userId, account))
        return state.DoS(100, ERRORMSG("CRegisterAccountTx::ExecuteTx, read source keyId %s account info error",
            keyId.ToString()), UPDATE_ACCOUNT_FAIL, "bad-read-accountdb");

    CAccountLog acctLog(account);
    if (account.pubKey.IsFullyValid() && account.pubKey.GetKeyId() == keyId)
        return state.DoS(100, ERRORMSG("CRegisterAccountTx::ExecuteTx, read source keyId %s duplicate register",
            keyId.ToString()), UPDATE_ACCOUNT_FAIL, "duplicate-register-account");

    account.pubKey = userId.get<CPubKey>();
    if (llFees > 0)
        if (!account.OperateAccount(MINUS_FREE, llFees, nHeight))
            return state.DoS(100, ERRORMSG("CRegisterAccountTx::ExecuteTx, not sufficient funds in account, keyid=%s",
                keyId.ToString()), UPDATE_ACCOUNT_FAIL, "not-sufficiect-funds");

    account.regID = regId;
    if (typeid(CPubKey) == minerId.type()) {
        account.minerPubKey = minerId.get<CPubKey>();
        if (account.minerPubKey.IsValid() && !account.minerPubKey.IsFullyValid()) {
            return state.DoS(100, ERRORMSG("CRegisterAccountTx::ExecuteTx, minerPubKey:%s Is Invalid",
                account.minerPubKey.ToString()), UPDATE_ACCOUNT_FAIL, "MinerPKey Is Invalid");
        }
    }

    if (!view.SaveAccountInfo(regId, keyId, account))
        return state.DoS(100, ERRORMSG("CRegisterAccountTx::ExecuteTx, write source addr %s account info error",
            regId.ToString()), UPDATE_ACCOUNT_FAIL, "bad-read-accountdb");

    txundo.vAccountLog.push_back(acctLog);
    txundo.txHash = GetHash();
    if(SysCfg().GetAddressToTxFlag()) {
        CScriptDBOperLog operAddressToTxLog;
        CKeyID sendKeyId;
        if(!view.GetKeyId(userId, sendKeyId))
            return ERRORMSG("CRegisterAccountTx::ExecuteTx, get keyid by userId error!");

        if(!scriptDB.SetTxHashByAddress(sendKeyId, nHeight, nIndex+1, txundo.txHash.GetHex(), operAddressToTxLog))
            return false;

        txundo.vScriptOperLog.push_back(operAddressToTxLog);
    }

    return true;
}

bool CRegisterAccountTx::UndoExecuteTx(int nIndex, CAccountViewCache &view, CValidationState &state,
        CTxUndo &txundo, int nHeight, CTransactionDBCache &txCache, CScriptDBViewCache &scriptDB) {
    // drop account
    CRegID accountId(nHeight, nIndex);
    CAccount oldAccount;
    if (!view.GetAccount(accountId, oldAccount))
        return state.DoS(100, ERRORMSG("CRegisterAccountTx::UndoExecuteTx, read secure account=%s info error",
            accountId.ToString()), UPDATE_ACCOUNT_FAIL, "bad-read-accountdb");

    CKeyID keyId;
    view.GetKeyId(accountId, keyId);

    if (llFees > 0) {
        CAccountLog accountLog;
        if (!txundo.GetAccountOperLog(keyId, accountLog))
            return state.DoS(100, ERRORMSG("CRegisterAccountTx::UndoExecuteTx, read keyId=%s tx undo info error", keyId.GetHex()),
                    UPDATE_ACCOUNT_FAIL, "bad-read-txundoinfo");
        oldAccount.UndoOperateAccount(accountLog);
    }

    if (!oldAccount.IsEmptyValue()) {
        CPubKey empPubKey;
        oldAccount.pubKey = empPubKey;
        oldAccount.minerPubKey = empPubKey;
        oldAccount.regID.Clean();
        CUserID userId(keyId);
        view.SetAccount(userId, oldAccount);
    } else {
        view.EraseAccount(userId);
    }
    view.EraseId(accountId);
    return true;
}

bool CRegisterAccountTx::GetAddress(set<CKeyID> &vAddr, CAccountViewCache &view, CScriptDBViewCache &scriptDB) {
    if (!userId.get<CPubKey>().IsFullyValid())
        return false;

    vAddr.insert(userId.get<CPubKey>().GetKeyId());
    return true;
}

string CRegisterAccountTx::ToString(CAccountViewCache &view) const {
    string str;
    str += strprintf("txType=%s, hash=%s, ver=%d, pubkey=%s, llFees=%ld, keyid=%s, nValidHeight=%d\n",
        GetTxType(nTxType), GetHash().ToString().c_str(), nVersion, userId.get<CPubKey>().ToString(),
        llFees, userId.get<CPubKey>().GetKeyId().ToAddress(), nValidHeight);

    return str;
}

Object CRegisterAccountTx::ToJson(const CAccountViewCache &AccountView) const {
    assert(userId.type() == typeid(CPubKey));
    string address = userId.get<CPubKey>().GetKeyId().ToAddress();
    string userPubKey = userId.ToString();
    string userMinerPubKey = minerId.ToString();

    Object result;
    result.push_back(Pair("hash",           GetHash().GetHex()));
    result.push_back(Pair("tx_type",        GetTxType(nTxType)));
    result.push_back(Pair("ver",            nVersion));
    result.push_back(Pair("addr",           address));
    result.push_back(Pair("pubkey",         userPubKey));
    result.push_back(Pair("miner_pubkey",   userMinerPubKey));
    result.push_back(Pair("fees",           llFees));
    result.push_back(Pair("valid_height",   nValidHeight));
    return result;
}

bool CRegisterAccountTx::CheckTx(CValidationState &state, CAccountViewCache &view,
                                          CScriptDBViewCache &scriptDB) {
    if (userId.type() != typeid(CPubKey))
        return state.DoS(100, ERRORMSG("CRegisterAccountTx::CheckTx, userId must be CPubKey"),
            REJECT_INVALID, "userid-type-error");

    if ((minerId.type() != typeid(CPubKey)) && (minerId.type() != typeid(CNullID)))
        return state.DoS(100, ERRORMSG("CRegisterAccountTx::CheckTx, minerId must be CPubKey or CNullID"),
            REJECT_INVALID, "minerid-type-error");

    if (!userId.get<CPubKey>().IsFullyValid())
        return state.DoS(100, ERRORMSG("CRegisterAccountTx::CheckTx, register tx public key is invalid"),
            REJECT_INVALID, "bad-regtx-publickey");

    if (!CheckMoneyRange(llFees))
        return state.DoS(100, ERRORMSG("CRegisterAccountTx::CheckTx, register tx fee out of range"),
            REJECT_INVALID, "bad-regtx-fee-toolarge");

    if (!CheckMinTxFee(llFees)) {
        return state.DoS(100, ERRORMSG("CRegisterAccountTx::CheckTx, register tx fee smaller than MinTxFee"),
            REJECT_INVALID, "bad-tx-fee-toosmall");
    }

    if (!CheckSignatureSize(signature)) {
        return state.DoS(100, ERRORMSG("CRegisterAccountTx::CheckTx, signature size invalid"),
            REJECT_INVALID, "bad-tx-sig-size");
    }

    // check signature script
    uint256 sighash = SignatureHash();
    if (!CheckSignScript(sighash, signature, userId.get<CPubKey>()))
        return state.DoS(100, ERRORMSG("CRegisterAccountTx::CheckTx, register tx signature error "),
            REJECT_INVALID, "bad-regtx-signature");

    return true;
}

string CCommonTx::ToString(CAccountViewCache &view) const {
    string srcId;
    if (srcUserId.type() == typeid(CPubKey)) {
        srcId = srcUserId.get<CPubKey>().ToString();
    } else if (srcUserId.type() == typeid(CRegID)) {
        srcId = srcUserId.get<CRegID>().ToString();
    }

    string desId;
    if (desUserId.type() == typeid(CKeyID)) {
        desId = desUserId.get<CKeyID>().ToString();
    } else if (desUserId.type() == typeid(CRegID)) {
        desId = desUserId.get<CRegID>().ToString();
    }

    string str = strprintf(
        "txType=%s, hash=%s, ver=%d, srcId=%s, desId=%s, bcoins=%ld, llFees=%ld, memo=%s, "
        "nValidHeight=%d\n",
        GetTxType(nTxType), GetHash().ToString().c_str(), nVersion, srcId.c_str(), desId.c_str(),
        bcoins, llFees, HexStr(memo).c_str(), nValidHeight);

    return str;
}

Object CCommonTx::ToJson(const CAccountViewCache &AccountView) const {
    Object result;
    CAccountViewCache view(AccountView);

    auto GetRegIdString = [&](CUserID const &userId) {
        if (userId.type() == typeid(CRegID))
            return userId.get<CRegID>().ToString();
        return string("");
    };

    CKeyID srcKeyId, desKeyId;
    view.GetKeyId(srcUserId, srcKeyId);
    view.GetKeyId(desUserId, desKeyId);

    result.push_back(Pair("hash",           GetHash().GetHex()));
    result.push_back(Pair("tx_type",        GetTxType(nTxType)));
    result.push_back(Pair("ver",            nVersion));
    result.push_back(Pair("regid",          GetRegIdString(srcUserId)));
    result.push_back(Pair("addr",           srcKeyId.ToAddress()));
    result.push_back(Pair("dest_regid",     GetRegIdString(desUserId)));
    result.push_back(Pair("dest_addr",      desKeyId.ToAddress()));
    result.push_back(Pair("money",          bcoins));
    result.push_back(Pair("fees",           llFees));
    result.push_back(Pair("memo",           HexStr(memo)));
    result.push_back(Pair("valid_height",   nValidHeight));

    return result;
}

bool CCommonTx::GetAddress(set<CKeyID> &vAddr, CAccountViewCache &view,
                           CScriptDBViewCache &scriptDB) {
    CKeyID keyId;
    if (!view.GetKeyId(srcUserId, keyId))
        return false;
    vAddr.insert(keyId);
    CKeyID desKeyId;
    if (!view.GetKeyId(desUserId, desKeyId))
        return false;
    vAddr.insert(desKeyId);
    return true;
}

bool CCommonTx::ExecuteTx(int nIndex, CAccountViewCache &view, CValidationState &state,
                          CTxUndo &txundo, int nHeight, CTransactionDBCache &txCache,
                          CScriptDBViewCache &scriptDB) {
    CAccount srcAcct;
    CAccount desAcct;
    bool generateRegID = false;

    if (!view.GetAccount(srcUserId, srcAcct))
        return state.DoS(100, ERRORMSG("CCommonTx::ExecuteTx, read source addr account info error"),
                         READ_ACCOUNT_FAIL, "bad-read-accountdb");
    else {
        if (srcUserId.type() == typeid(CPubKey)) {
            srcAcct.pubKey = srcUserId.get<CPubKey>();

            CRegID regId;
            // If the source account does NOT have CRegID, need to generate a new CRegID.
            if (!view.GetRegId(srcUserId, regId)) {
                srcAcct.regID = CRegID(nHeight, nIndex);
                generateRegID = true;
            }
        }
    }

    CAccountLog srcAcctLog(srcAcct);
    CAccountLog desAcctLog;
    uint64_t minusValue = llFees + bcoins;
    if (!srcAcct.OperateAccount(MINUS_FREE, minusValue, nHeight))
        return state.DoS(100, ERRORMSG("CCommonTx::ExecuteTx, account has insufficient funds"),
                         UPDATE_ACCOUNT_FAIL, "operate-minus-account-failed");

    if (generateRegID) {
        if (!view.SaveAccountInfo(srcAcct.regID, srcAcct.keyID, srcAcct))
            return state.DoS(100, ERRORMSG("CCommonTx::ExecuteTx, save account info error"),
                             WRITE_ACCOUNT_FAIL, "bad-write-accountdb");
    } else {
        if (!view.SetAccount(CUserID(srcAcct.keyID), srcAcct))
            return state.DoS(100, ERRORMSG("CCommonTx::ExecuteTx, save account info error"),
                             WRITE_ACCOUNT_FAIL, "bad-write-accountdb");
    }

    uint64_t addValue = bcoins;
    if (!view.GetAccount(desUserId, desAcct)) {
        if (desUserId.type() == typeid(CKeyID)) {  // target account does NOT have CRegID
            desAcct.keyID    = desUserId.get<CKeyID>();
            desAcctLog.keyID = desAcct.keyID;
        } else {
            return state.DoS(100, ERRORMSG("CCommonTx::ExecuteTx, get account info failed"),
                             READ_ACCOUNT_FAIL, "bad-read-accountdb");
        }
    } else {  // target account has NO CAccount(first involved in transacion)
        desAcctLog.SetValue(desAcct);
    }

    if (!desAcct.OperateAccount(ADD_FREE, addValue, nHeight))
        return state.DoS(100, ERRORMSG("CCommonTx::ExecuteTx, operate accounts error"),
                         UPDATE_ACCOUNT_FAIL, "operate-add-account-failed");

    if (!view.SetAccount(desUserId, desAcct))
        return state.DoS(100,
                         ERRORMSG("CCommonTx::ExecuteTx, save account error, kyeId=%s",
                                  desAcct.keyID.ToString()),
                         UPDATE_ACCOUNT_FAIL, "bad-save-account");

    txundo.vAccountLog.push_back(srcAcctLog);
    txundo.vAccountLog.push_back(desAcctLog);
    txundo.txHash = GetHash();

    if (SysCfg().GetAddressToTxFlag()) {
        CScriptDBOperLog operAddressToTxLog;
        CKeyID sendKeyId;
        CKeyID revKeyId;
        if (!view.GetKeyId(srcUserId, sendKeyId))
            return ERRORMSG("CCommonTx::ExecuteTx, get keyid by srcUserId error!");

        if (!view.GetKeyId(desUserId, revKeyId))
            return ERRORMSG("CCommonTx::ExecuteTx, get keyid by desUserId error!");

        if (!scriptDB.SetTxHashByAddress(sendKeyId, nHeight, nIndex + 1, txundo.txHash.GetHex(),
                                         operAddressToTxLog))
            return false;
        txundo.vScriptOperLog.push_back(operAddressToTxLog);

        if (!scriptDB.SetTxHashByAddress(revKeyId, nHeight, nIndex + 1, txundo.txHash.GetHex(),
                                         operAddressToTxLog))
            return false;
        txundo.vScriptOperLog.push_back(operAddressToTxLog);
    }

    return true;
}

bool CCommonTx::UndoExecuteTx(int nIndex, CAccountViewCache &view, CValidationState &state,
                              CTxUndo &txundo, int nHeight, CTransactionDBCache &txCache,
                              CScriptDBViewCache &scriptDB) {
    vector<CAccountLog>::reverse_iterator rIterAccountLog = txundo.vAccountLog.rbegin();
    for (; rIterAccountLog != txundo.vAccountLog.rend(); ++rIterAccountLog) {
        CAccount account;
        CUserID userId = rIterAccountLog->keyID;
        if (!view.GetAccount(userId, account)) {
            return state.DoS(100, ERRORMSG("CCommonTx::UndoExecuteTx, read account info error"),
                             READ_ACCOUNT_FAIL, "bad-read-accountdb");
        }
        if (!account.UndoOperateAccount(*rIterAccountLog)) {
            return state.DoS(100, ERRORMSG("CCommonTx::UndoExecuteTx, undo operate account failed"),
                             UPDATE_ACCOUNT_FAIL, "undo-operate-account-failed");
        }

        if (account.IsEmptyValue() &&
            (!account.pubKey.IsFullyValid() || account.pubKey.GetKeyId() != account.keyID)) {
            view.EraseAccount(userId);
        } else if (account.regID == CRegID(nHeight, nIndex)) {
            // If the CRegID was generated by this COMMON_TX, need to remove CRegID.
            CPubKey empPubKey;
            account.pubKey      = empPubKey;
            account.minerPubKey = empPubKey;
            account.regID.Clean();
            if (!view.SetAccount(userId, account)) {
                return state.DoS(100,
                                 ERRORMSG("CCommonTx::UndoExecuteTx, write account info error"),
                                 UPDATE_ACCOUNT_FAIL, "bad-write-accountdb");
            }

            view.EraseId(CRegID(nHeight, nIndex));
        } else {
            if (!view.SetAccount(userId, account)) {
                return state.DoS(100,
                                 ERRORMSG("CCommonTx::UndoExecuteTx, write account info error"),
                                 UPDATE_ACCOUNT_FAIL, "bad-write-accountdb");
            }
        }
    }

    vector<CScriptDBOperLog>::reverse_iterator rIterScriptDBLog = txundo.vScriptOperLog.rbegin();
    for (; rIterScriptDBLog != txundo.vScriptOperLog.rend(); ++rIterScriptDBLog) {
        if (!scriptDB.UndoScriptData(rIterScriptDBLog->vKey, rIterScriptDBLog->vValue))
            return state.DoS(100, ERRORMSG("CCommonTx::UndoExecuteTx, undo scriptdb data error"),
                             UPDATE_ACCOUNT_FAIL, "bad-save-scriptdb");
    }

    return true;
}

bool CCommonTx::CheckTx(CValidationState &state, CAccountViewCache &view,
                        CScriptDBViewCache &scriptDB) {
    if (memo.size() > kCommonTxMemoMaxSize)
        return state.DoS(100, ERRORMSG("CCommonTx::CheckTx, memo's size too large"), REJECT_INVALID,
                         "memo-size-toolarge");

    if ((srcUserId.type() != typeid(CRegID)) && (srcUserId.type() != typeid(CPubKey)))
        return state.DoS(100, ERRORMSG("CCommonTx::CheckTx, srcaddr type error"), REJECT_INVALID,
                         "srcaddr-type-error");

    if ((desUserId.type() != typeid(CRegID)) && (desUserId.type() != typeid(CKeyID)))
        return state.DoS(100, ERRORMSG("CCommonTx::CheckTx, desaddr type error"), REJECT_INVALID,
                         "desaddr-type-error");

    if ((srcUserId.type() == typeid(CPubKey)) && !srcUserId.get<CPubKey>().IsFullyValid())
        return state.DoS(100, ERRORMSG("CCommonTx::CheckTx, public key is invalid"), REJECT_INVALID,
                         "bad-commontx-publickey");

    if (!CheckMoneyRange(llFees))
        return state.DoS(100, ERRORMSG("CCommonTx::CheckTx, tx fees out of money range"),
                         REJECT_INVALID, "bad-appeal-fees-toolarge");

    if (!CheckMinTxFee(llFees))
        return state.DoS(100, ERRORMSG("CCommonTx::CheckTx, tx fees smaller than MinTxFee"),
                         REJECT_INVALID, "bad-tx-fees-toosmall");

    CAccount srcAccount;
    if (!view.GetAccount(srcUserId, srcAccount))
        return state.DoS(100, ERRORMSG("CCommonTx::CheckTx, read account failed"), REJECT_INVALID,
                         "bad-getaccount");

    if ((srcUserId.type() == typeid(CRegID)) && !srcAccount.IsRegistered())
        return state.DoS(100, ERRORMSG("CCommonTx::CheckTx, account pubkey not registered"),
                         REJECT_INVALID, "bad-account-unregistered");

    if (!CheckSignatureSize(signature))
            return state.DoS(100, ERRORMSG("CCommonTx::CheckTx, signature size invalid"),
                             REJECT_INVALID, "bad-tx-sig-size");

    uint256 sighash = SignatureHash();
    CPubKey pubKey =
        srcUserId.type() == typeid(CPubKey) ? srcUserId.get<CPubKey>() : srcAccount.pubKey;
    if (!CheckSignScript(sighash, signature, pubKey))
        return state.DoS(100, ERRORMSG("CCommonTx::CheckTx, CheckSignScript failed"),
                         REJECT_INVALID, "bad-signscript-check");

    return true;
}

bool CContractTx::GetAddress(set<CKeyID> &vAddr, CAccountViewCache &view, CScriptDBViewCache &scriptDB) {
    CKeyID keyId;
    if (!view.GetKeyId(srcRegId, keyId))
        return false;

    vAddr.insert(keyId);
    CKeyID desKeyId;
    if (!view.GetKeyId(desUserId, desKeyId))
        return false;

    vAddr.insert(desKeyId);

    CVmRunEnv vmRunEnv;
    std::shared_ptr<CBaseTx> pTx = GetNewInstance();
    uint64_t fuelRate = GetFuelRate(scriptDB);
    CScriptDBViewCache scriptDBView(scriptDB);

    if (!pTxCacheTip->HaveTx(GetHash())) {
        CAccountViewCache accountView(view);
        tuple<bool, uint64_t, string> ret = vmRunEnv.ExecuteContract(pTx, accountView, scriptDBView,
            chainActive.Height() + 1, fuelRate, nRunStep);

        if (!std::get<0>(ret))
            return ERRORMSG("CContractTx::GetAddress, %s", std::get<2>(ret));

        vector<shared_ptr<CAccount> > vpAccount = vmRunEnv.GetNewAccont();

        for (auto & item : vpAccount)
            vAddr.insert(item->keyID);

        vector<std::shared_ptr<CAppUserAccount> > &vAppUserAccount = vmRunEnv.GetRawAppUserAccount();
        for (auto & itemUserAccount : vAppUserAccount) {
            CKeyID itemKeyID;
            bool bValid = GetKeyId(view, itemUserAccount.get()->GetAccUserId(), itemKeyID);
            if (bValid)
                vAddr.insert(itemKeyID);
        }
    } else {
        set<CKeyID> vTxRelAccount;
        if (!scriptDBView.GetTxRelAccount(GetHash(), vTxRelAccount))
            return false;

        vAddr.insert(vTxRelAccount.begin(), vTxRelAccount.end());
    }
    return true;
}

string CContractTx::ToString(CAccountViewCache &view) const {
    string desId;
    if (desUserId.type() == typeid(CKeyID)) {
        desId = desUserId.get<CKeyID>().ToString();
    } else if (desUserId.type() == typeid(CRegID)) {
        desId = desUserId.get<CRegID>().ToString();
    }

    string str = strprintf(
        "txType=%s, hash=%s, ver=%d, srcId=%s, desId=%s, bcoins=%ld, llFees=%ld, arguments=%s, "
        "nValidHeight=%d\n",
        GetTxType(nTxType), GetHash().ToString().c_str(), nVersion,
        srcRegId.get<CRegID>().ToString(), desId.c_str(), bcoins, llFees,
        HexStr(arguments).c_str(), nValidHeight);

    return str;
}

Object CContractTx::ToJson(const CAccountViewCache &AccountView) const {
    Object result;
    CAccountViewCache view(AccountView);

    auto GetRegIdString = [&](CUserID const &userId) {
        if (userId.type() == typeid(CRegID))
            return userId.get<CRegID>().ToString();
        return string("");
    };

    CKeyID srcKeyId, desKeyId;
    view.GetKeyId(srcRegId, srcKeyId);
    view.GetKeyId(desUserId, desKeyId);

    result.push_back(Pair("hash",       GetHash().GetHex()));
    result.push_back(Pair("tx_type",    GetTxType(nTxType)));
    result.push_back(Pair("ver",        nVersion));
    result.push_back(Pair("regid",      GetRegIdString(srcRegId)));
    result.push_back(Pair("addr",       srcKeyId.ToAddress()));
    result.push_back(Pair("dest_regid", GetRegIdString(desUserId)));
    result.push_back(Pair("dest_addr",  desKeyId.ToAddress()));
    result.push_back(Pair("money",      bcoins));
    result.push_back(Pair("fees",       llFees));
    result.push_back(Pair("arguments",  HexStr(arguments)));
    result.push_back(Pair("valid_height", nValidHeight));

    return result;
}

bool CContractTx::ExecuteTx(int nIndex, CAccountViewCache &view, CValidationState &state,
                            CTxUndo &txundo, int nHeight, CTransactionDBCache &txCache,
                            CScriptDBViewCache &scriptDB) {
    CAccount srcAcct;
    CAccount desAcct;
    CAccountLog desAcctLog;
    uint64_t minusValue = llFees + bcoins;
    if (!view.GetAccount(srcRegId, srcAcct))
        return state.DoS(100, ERRORMSG("CContractTx::ExecuteTx, read source addr %s account info error",
            srcRegId.get<CRegID>().ToString()), READ_ACCOUNT_FAIL, "bad-read-accountdb");

    CAccountLog srcAcctLog(srcAcct);
    if (!srcAcct.OperateAccount(MINUS_FREE, minusValue, nHeight))
        return state.DoS(100, ERRORMSG("CContractTx::ExecuteTx, accounts insufficient funds"),
            UPDATE_ACCOUNT_FAIL, "operate-minus-account-failed");

    CUserID userId = srcAcct.keyID;
    if (!view.SetAccount(userId, srcAcct))
        return state.DoS(100, ERRORMSG("CContractTx::ExecuteTx, save account%s info error",
            srcRegId.get<CRegID>().ToString()), WRITE_ACCOUNT_FAIL, "bad-write-accountdb");

    uint64_t addValue = bcoins;
    if (!view.GetAccount(desUserId, desAcct)) {
        return state.DoS(100, ERRORMSG("CContractTx::ExecuteTx, get account info failed by regid:%s",
            desUserId.get<CRegID>().ToString()), READ_ACCOUNT_FAIL, "bad-read-accountdb");
    } else {
        desAcctLog.SetValue(desAcct);
    }

    if (!desAcct.OperateAccount(ADD_FREE, addValue, nHeight))
        return state.DoS(100, ERRORMSG("CContractTx::ExecuteTx, operate accounts error"),
            UPDATE_ACCOUNT_FAIL, "operate-add-account-failed");

    if (!view.SetAccount(desUserId, desAcct))
        return state.DoS(100, ERRORMSG("CContractTx::ExecuteTx, save account error, kyeId=%s",
            desAcct.keyID.ToString()), UPDATE_ACCOUNT_FAIL, "bad-save-account");

    txundo.vAccountLog.push_back(srcAcctLog);
    txundo.vAccountLog.push_back(desAcctLog);

    vector<unsigned char> vScript;
    if (!scriptDB.GetScript(desUserId.get<CRegID>(), vScript))
        return state.DoS(100, ERRORMSG("CContractTx::ExecuteTx, read script faild, regId=%s",
            desUserId.get<CRegID>().ToString()), READ_ACCOUNT_FAIL, "bad-read-script");

    CVmRunEnv vmRunEnv;
    std::shared_ptr<CBaseTx> pTx = GetNewInstance();
    uint64_t fuelRate = GetFuelRate(scriptDB);

    int64_t llTime = GetTimeMillis();
    tuple<bool, uint64_t, string> ret = vmRunEnv.ExecuteContract(pTx, view, scriptDB, nHeight, fuelRate, nRunStep);
    if (!std::get<0>(ret))
        return state.DoS(100, ERRORMSG("CContractTx::ExecuteTx, txid=%s run script error:%s",
            GetHash().GetHex(), std::get<2>(ret)), UPDATE_ACCOUNT_FAIL, "run-script-error: " + std::get<2>(ret));

    LogPrint("vm", "execute contract elapse:%lld, txhash=%s\n", GetTimeMillis() - llTime, GetHash().GetHex());

    set<CKeyID> vAddress;
    vector<std::shared_ptr<CAccount> > &vAccount = vmRunEnv.GetNewAccont();
    // update accounts' info refered to the contract
    for (auto &itemAccount : vAccount) {
        vAddress.insert(itemAccount->keyID);
        userId = itemAccount->keyID;
        CAccount oldAcct;
        if (!view.GetAccount(userId, oldAcct)) {
            // The contract transfers money to an address for the first time.
            if (!itemAccount->keyID.IsNull()) {
                oldAcct.keyID = itemAccount->keyID;
            } else {
                return state.DoS(100, ERRORMSG("CContractTx::ExecuteTx, read account info error"),
                                 UPDATE_ACCOUNT_FAIL, "bad-read-accountdb");
            }
        }
        CAccountLog oldAcctLog(oldAcct);
        if (!view.SetAccount(userId, *itemAccount))
            return state.DoS(100, ERRORMSG("CContractTx::ExecuteTx, write account info error"),
                UPDATE_ACCOUNT_FAIL, "bad-write-accountdb");

        txundo.vAccountLog.push_back(oldAcctLog);
    }
    txundo.vScriptOperLog.insert(txundo.vScriptOperLog.end(), vmRunEnv.GetDbLog()->begin(),
                                 vmRunEnv.GetDbLog()->end());
    vector<std::shared_ptr<CAppUserAccount> > &vAppUserAccount = vmRunEnv.GetRawAppUserAccount();
    for (auto & itemUserAccount : vAppUserAccount) {
        CKeyID itemKeyID;
        bool bValid = GetKeyId(view, itemUserAccount.get()->GetAccUserId(), itemKeyID);
        if (bValid)
            vAddress.insert(itemKeyID);
    }

    if (!scriptDB.SetTxRelAccout(GetHash(), vAddress))
        return ERRORMSG("CContractTx::ExecuteTx, save tx relate account info to script db error");

    txundo.txHash = GetHash();

    if (SysCfg().GetAddressToTxFlag()) {
        CScriptDBOperLog operAddressToTxLog;
        CKeyID sendKeyId;
        CKeyID revKeyId;
        if (!view.GetKeyId(srcRegId, sendKeyId))
            return ERRORMSG("CContractTx::ExecuteTx, get keyid by srcRegId error!");

        if (!view.GetKeyId(desUserId, revKeyId))
            return ERRORMSG("CContractTx::ExecuteTx, get keyid by desUserId error!");

        if (!scriptDB.SetTxHashByAddress(sendKeyId, nHeight, nIndex + 1, txundo.txHash.GetHex(),
                                         operAddressToTxLog))
            return false;
        txundo.vScriptOperLog.push_back(operAddressToTxLog);

        if (!scriptDB.SetTxHashByAddress(revKeyId, nHeight, nIndex + 1, txundo.txHash.GetHex(),
                                         operAddressToTxLog))
            return false;
        txundo.vScriptOperLog.push_back(operAddressToTxLog);
    }

    return true;
}

bool CContractTx::UndoExecuteTx(int nIndex, CAccountViewCache &view, CValidationState &state,
                                CTxUndo &txundo, int nHeight, CTransactionDBCache &txCache,
                                CScriptDBViewCache &scriptDB) {
    vector<CAccountLog>::reverse_iterator rIterAccountLog = txundo.vAccountLog.rbegin();
    for (; rIterAccountLog != txundo.vAccountLog.rend(); ++rIterAccountLog) {
        CAccount account;
        CUserID userId = rIterAccountLog->keyID;
        if (!view.GetAccount(userId, account)) {
            return state.DoS(100, ERRORMSG("CContractTx::UndoExecuteTx, read account info error"),
                             READ_ACCOUNT_FAIL, "bad-read-accountdb");
        }

        if (!account.UndoOperateAccount(*rIterAccountLog)) {
            return state.DoS(100,
                             ERRORMSG("CContractTx::UndoExecuteTx, undo operate account failed"),
                             UPDATE_ACCOUNT_FAIL, "undo-operate-account-failed");
        }

        if (!view.SetAccount(userId, account)) {
            return state.DoS(100, ERRORMSG("CContractTx::UndoExecuteTx, write account info error"),
                             UPDATE_ACCOUNT_FAIL, "bad-write-accountdb");
        }
    }

    vector<CScriptDBOperLog>::reverse_iterator rIterScriptDBLog = txundo.vScriptOperLog.rbegin();
    for (; rIterScriptDBLog != txundo.vScriptOperLog.rend(); ++rIterScriptDBLog) {
        if (!scriptDB.UndoScriptData(rIterScriptDBLog->vKey, rIterScriptDBLog->vValue))
            return state.DoS(100, ERRORMSG("CContractTx::UndoExecuteTx, undo scriptdb data error"),
                             UPDATE_ACCOUNT_FAIL, "bad-save-scriptdb");
    }

    if (!scriptDB.EraseTxRelAccout(GetHash()))
        return state.DoS(100, ERRORMSG("CContractTx::UndoExecuteTx, erase tx rel account error"),
                         UPDATE_ACCOUNT_FAIL, "bad-save-scriptdb");

    return true;
}

bool CContractTx::CheckTx(CValidationState &state, CAccountViewCache &view,
                          CScriptDBViewCache &scriptDB) {
    if (arguments.size() > kContractArgumentMaxSize)
        return state.DoS(100, ERRORMSG("CContractTx::CheckTx, arguments's size too large"),
                         REJECT_INVALID, "arguments-size-toolarge");

    if (srcRegId.type() != typeid(CRegID))
        return state.DoS(100, ERRORMSG("CContractTx::CheckTx, srcRegId must be CRegID"),
                         REJECT_INVALID, "srcaddr-type-error");

    if (desUserId.type() != typeid(CRegID))
        return state.DoS(100, ERRORMSG("CContractTx::CheckTx, desUserId must be CRegID"),
                         REJECT_INVALID, "desaddr-type-error");

    if (!CheckMoneyRange(llFees))
        return state.DoS(100, ERRORMSG("CContractTx::CheckTx, tx fee out of money range"),
                         REJECT_INVALID, "bad-appeal-fee-toolarge");

    if (!CheckMinTxFee(llFees))
        return state.DoS(100, ERRORMSG("CContractTx::CheckTx, tx fee smaller than MinTxFee"),
                         REJECT_INVALID, "bad-tx-fee-toosmall");

    CAccount srcAccount;
    if (!view.GetAccount(srcRegId.get<CRegID>(), srcAccount))
        return state.DoS(100,
                         ERRORMSG("CContractTx::CheckTx, read account failed, regId=%s",
                                  srcRegId.get<CRegID>().ToString()),
                         REJECT_INVALID, "bad-getaccount");

    if (!srcAccount.IsRegistered())
        return state.DoS(100, ERRORMSG("CContractTx::CheckTx, account pubkey not registered"),
                         REJECT_INVALID, "bad-account-unregistered");

    vector<unsigned char> vScript;
    if (!scriptDB.GetScript(desUserId.get<CRegID>(), vScript))
        return state.DoS(100,
                         ERRORMSG("CContractTx::CheckTx, read script faild, regId=%s",
                                  desUserId.get<CRegID>().ToString()),
                         REJECT_INVALID, "bad-read-script");

    if (!CheckSignatureSize(signature))
        return state.DoS(100, ERRORMSG("CContractTx::CheckTx, signature size invalid"),
                         REJECT_INVALID, "bad-tx-sig-size");

    uint256 sighash = SignatureHash();
    if (!CheckSignScript(sighash, signature, srcAccount.pubKey))
        return state.DoS(100, ERRORMSG("CContractTx::CheckTx, CheckSignScript failed"),
                         REJECT_INVALID, "bad-signscript-check");

    return true;
}

string CRewardTx::ToString(CAccountViewCache &view) const {
    string str;
    CKeyID keyId;
    view.GetKeyId(account, keyId);
    CRegID regId;
    view.GetRegId(account, regId);
    str += strprintf("txType=%s, hash=%s, ver=%d, account=%s, keyid=%s, rewardValue=%ld\n",
        GetTxType(nTxType), GetHash().ToString().c_str(), nVersion, regId.ToString(), keyId.GetHex(), rewardValue);

    return str;
}

Object CRewardTx::ToJson(const CAccountViewCache &AccountView) const{
    Object result;
    CAccountViewCache view(AccountView);
    CKeyID keyid;
    result.push_back(Pair("hash", GetHash().GetHex()));
    result.push_back(Pair("tx_type", GetTxType(nTxType)));
    result.push_back(Pair("ver", nVersion));
    if (account.type() == typeid(CRegID)) {
        result.push_back(Pair("regid", account.get<CRegID>().ToString()));
    }
    if (account.type() == typeid(CPubKey)) {
        result.push_back(Pair("pubkey", account.get<CPubKey>().ToString()));
    }
    view.GetKeyId(account, keyid);
    result.push_back(Pair("addr", keyid.ToAddress()));
    result.push_back(Pair("money", rewardValue));
    result.push_back(Pair("valid_height", nHeight));
    return result;
}

bool CRewardTx::GetAddress(set<CKeyID> &vAddr, CAccountViewCache &view,
                           CScriptDBViewCache &scriptDB) {
    CKeyID keyId;
    if (account.type() == typeid(CRegID)) {
        if (!view.GetKeyId(account, keyId)) return false;
        vAddr.insert(keyId);
    } else if (account.type() == typeid(CPubKey)) {
        CPubKey pubKey = account.get<CPubKey>();
        if (!pubKey.IsFullyValid()) return false;
        vAddr.insert(pubKey.GetKeyId());
    }

    return true;
}

bool CRewardTx::ExecuteTx(int nIndex, CAccountViewCache &view, CValidationState &state,
                          CTxUndo &txundo, int nHeight, CTransactionDBCache &txCache,
                          CScriptDBViewCache &scriptDB) {
    if (account.type() != typeid(CRegID)) {
        return state.DoS(100,
            ERRORMSG("CRewardTx::ExecuteTx, account %s error, data type must be CRegID",
            account.ToString()), UPDATE_ACCOUNT_FAIL, "bad-account");
    }

    CAccount acctInfo;
    if (!view.GetAccount(account, acctInfo)) {
        return state.DoS(100, ERRORMSG("CRewardTx::ExecuteTx, read source addr %s account info error",
            account.ToString()), UPDATE_ACCOUNT_FAIL, "bad-read-accountdb");
    }
    // LogPrint("op_account", "before operate:%s\n", acctInfo.ToString());
    CAccountLog acctInfoLog(acctInfo);
    if (0 == nIndex) {
        // nothing to do here
    } else if (-1 == nIndex) {  // maturity reward tx, only update values
        acctInfo.bcoins += rewardValue;
    } else {  // never go into this step
        return ERRORMSG("nIndex type error!");
    }

    CUserID userId = acctInfo.keyID;
    if (!view.SetAccount(userId, acctInfo))
        return state.DoS(100, ERRORMSG("CRewardTx::ExecuteTx, write secure account info error"),
            UPDATE_ACCOUNT_FAIL, "bad-save-accountdb");

    txundo.Clear();
    txundo.vAccountLog.push_back(acctInfoLog);
    txundo.txHash = GetHash();
    if (SysCfg().GetAddressToTxFlag() && 0 == nIndex) {
        CScriptDBOperLog operAddressToTxLog;
        CKeyID sendKeyId;
        if (!view.GetKeyId(account, sendKeyId))
            return ERRORMSG("CRewardTx::ExecuteTx, get keyid by account error!");

        if (!scriptDB.SetTxHashByAddress(sendKeyId, nHeight, nIndex+1, txundo.txHash.GetHex(), operAddressToTxLog))
            return false;

        txundo.vScriptOperLog.push_back(operAddressToTxLog);
    }
    // LogPrint("op_account", "after operate:%s\n", acctInfo.ToString());
    return true;
}

bool CRewardTx::UndoExecuteTx(int nIndex, CAccountViewCache &view, CValidationState &state,
                              CTxUndo &txundo, int nHeight, CTransactionDBCache &txCache,
                              CScriptDBViewCache &scriptDB) {
    vector<CAccountLog>::reverse_iterator rIterAccountLog = txundo.vAccountLog.rbegin();
    for (; rIterAccountLog != txundo.vAccountLog.rend(); ++rIterAccountLog) {
        CAccount account;
        CUserID userId = rIterAccountLog->keyID;
        if (!view.GetAccount(userId, account)) {
            return state.DoS(100, ERRORMSG("CRewardTx::UndoExecuteTx, read account info error"),
                             READ_ACCOUNT_FAIL, "bad-read-accountdb");
        }

        if (!account.UndoOperateAccount(*rIterAccountLog)) {
            return state.DoS(100, ERRORMSG("CRewardTx::UndoExecuteTx, undo operate account failed"),
                             UPDATE_ACCOUNT_FAIL, "undo-operate-account-failed");
        }

        if (!view.SetAccount(userId, account)) {
            return state.DoS(100, ERRORMSG("CRewardTx::UndoExecuteTx, write account info error"),
                             UPDATE_ACCOUNT_FAIL, "bad-write-accountdb");
        }
    }

    vector<CScriptDBOperLog>::reverse_iterator rIterScriptDBLog = txundo.vScriptOperLog.rbegin();
    for (; rIterScriptDBLog != txundo.vScriptOperLog.rend(); ++rIterScriptDBLog) {
        if (!scriptDB.UndoScriptData(rIterScriptDBLog->vKey, rIterScriptDBLog->vValue))
            return state.DoS(100, ERRORMSG("CRewardTx::UndoExecuteTx, undo scriptdb data error"),
                             UPDATE_ACCOUNT_FAIL, "bad-save-scriptdb");
    }

    return true;
}

bool CRegisterContractTx::ExecuteTx(int nIndex, CAccountViewCache &view,CValidationState &state, CTxUndo &txundo,
        int nHeight, CTransactionDBCache &txCache, CScriptDBViewCache &scriptDB) {
    CAccount acctInfo;
    CScriptDBOperLog operLog;
    if (!view.GetAccount(regAcctId, acctInfo)) {
        return state.DoS(100, ERRORMSG("CRegisterContractTx::ExecuteTx, read regist addr %s account info error",
            regAcctId.ToString()), UPDATE_ACCOUNT_FAIL, "bad-read-accountdb");
    }
    CAccount acctInfoLog(acctInfo);
    uint64_t minusValue = llFees;
    if (minusValue > 0) {
        if(!acctInfo.OperateAccount(MINUS_FREE, minusValue, nHeight))
            return state.DoS(100, ERRORMSG("CRegisterContractTx::ExecuteTx, operate account failed ,regId=%s",
                regAcctId.ToString()), UPDATE_ACCOUNT_FAIL, "operate-account-failed");

        txundo.vAccountLog.push_back(acctInfoLog);
    }
    txundo.txHash = GetHash();

    CRegID regId(nHeight, nIndex);
    //create script account
    CKeyID keyId = Hash160(regId.GetVec6());
    CAccount account;
    account.keyID = keyId;
    account.regID = regId;
    //save new script content
    if(!scriptDB.SetScript(regId, script)){
        return state.DoS(100,
            ERRORMSG("CRegisterContractTx::ExecuteTx, save script id %s script info error",
                regId.ToString()), UPDATE_ACCOUNT_FAIL, "bad-save-scriptdb");
    }
    if (!view.SaveAccountInfo(regId, keyId, account)) {
        return state.DoS(100,
            ERRORMSG("CRegisterContractTx::ExecuteTx, create new account script id %s script info error",
                regId.ToString()), UPDATE_ACCOUNT_FAIL, "bad-save-scriptdb");
    }

    nRunStep = script.size();

    if(!operLog.vKey.empty()) {
        txundo.vScriptOperLog.push_back(operLog);
    }
    CUserID userId = acctInfo.keyID;
    if (!view.SetAccount(userId, acctInfo))
        return state.DoS(100, ERRORMSG("CRegisterContractTx::ExecuteTx, save account info error"),
            UPDATE_ACCOUNT_FAIL, "bad-save-accountdb");

    if(SysCfg().GetAddressToTxFlag()) {
        CScriptDBOperLog operAddressToTxLog;
        CKeyID sendKeyId;
        if(!view.GetKeyId(regAcctId, sendKeyId)) {
            return ERRORMSG("CRegisterContractTx::ExecuteTx, get regAcctId by account error!");
        }
        if(!scriptDB.SetTxHashByAddress(sendKeyId, nHeight, nIndex+1, txundo.txHash.GetHex(), operAddressToTxLog))
            return false;
        txundo.vScriptOperLog.push_back(operAddressToTxLog);
    }
    return true;
}

bool CRegisterContractTx::UndoExecuteTx(int nIndex, CAccountViewCache &view, CValidationState &state, CTxUndo &txundo,
        int nHeight, CTransactionDBCache &txCache, CScriptDBViewCache &scriptDB) {
    CAccount account;
    CUserID userId;
    if (!view.GetAccount(regAcctId, account)) {
        return state.DoS(100, ERRORMSG("CRegisterContractTx::UndoExecuteTx, read regist addr %s account info error", account.ToString()),
                         UPDATE_ACCOUNT_FAIL, "bad-read-accountdb");
    }

    CRegID scriptId(nHeight, nIndex);
    //delete script content
    if (!scriptDB.EraseScript(scriptId)) {
        return state.DoS(100, ERRORMSG("CRegisterContractTx::UndoExecuteTx, erase script id %s error", scriptId.ToString()),
                         UPDATE_ACCOUNT_FAIL, "erase-script-failed");
    }
    //delete account
    if (!view.EraseId(scriptId)) {
        return state.DoS(100, ERRORMSG("CRegisterContractTx::UndoExecuteTx, erase script account %s error", scriptId.ToString()),
                         UPDATE_ACCOUNT_FAIL, "erase-appkeyid-failed");
    }
    CKeyID keyId = Hash160(scriptId.GetVec6());
    userId       = keyId;
    if (!view.EraseAccount(userId)) {
        return state.DoS(100, ERRORMSG("CRegisterContractTx::UndoExecuteTx, erase script account %s error", scriptId.ToString()),
                         UPDATE_ACCOUNT_FAIL, "erase-appaccount-failed");
    }
    // LogPrint("INFO", "Delete regid %s app account\n", scriptId.ToString());

    for (auto &itemLog : txundo.vAccountLog) {
        if (itemLog.keyID == account.keyID) {
            if (!account.UndoOperateAccount(itemLog))
                return state.DoS(100, ERRORMSG("CRegisterContractTx::UndoExecuteTx, undo operate account error, keyId=%s", account.keyID.ToString()),
                                 UPDATE_ACCOUNT_FAIL, "undo-account-failed");
        }
    }

    vector<CScriptDBOperLog>::reverse_iterator rIterScriptDBLog = txundo.vScriptOperLog.rbegin();
    for (; rIterScriptDBLog != txundo.vScriptOperLog.rend(); ++rIterScriptDBLog) {
        if (!scriptDB.UndoScriptData(rIterScriptDBLog->vKey, rIterScriptDBLog->vValue))
            return state.DoS(100, ERRORMSG("CRegisterContractTx::UndoExecuteTx, undo scriptdb data error"), UPDATE_ACCOUNT_FAIL, "undo-scriptdb-failed");
    }
    userId = account.keyID;
    if (!view.SetAccount(userId, account))
        return state.DoS(100, ERRORMSG("CRegisterContractTx::UndoExecuteTx, save account error"), UPDATE_ACCOUNT_FAIL, "bad-save-accountdb");
    return true;
}

bool CRegisterContractTx::GetAddress(set<CKeyID> &vAddr, CAccountViewCache &view, CScriptDBViewCache &scriptDB) {
    CKeyID keyId;
    if (!view.GetKeyId(regAcctId, keyId))
        return false;
    vAddr.insert(keyId);
    return true;
}

string CRegisterContractTx::ToString(CAccountViewCache &view) const {
    string str;
    CKeyID keyId;
    view.GetKeyId(regAcctId, keyId);
    str += strprintf("txType=%s, hash=%s, ver=%d, accountId=%s, keyid=%s, llFees=%ld, nValidHeight=%d\n",
    GetTxType(nTxType), GetHash().ToString().c_str(), nVersion,regAcctId.get<CRegID>().ToString(), keyId.GetHex(), llFees, nValidHeight);
    return str;
}

Object CRegisterContractTx::ToJson(const CAccountViewCache &AccountView) const{
    Object result;
    CAccountViewCache view(AccountView);

    CKeyID keyid;
    view.GetKeyId(regAcctId, keyid);

    result.push_back(Pair("hash", GetHash().GetHex()));
    result.push_back(Pair("tx_type", GetTxType(nTxType)));
    result.push_back(Pair("ver", nVersion));
    result.push_back(Pair("regid",  regAcctId.get<CRegID>().ToString()));
    result.push_back(Pair("addr", keyid.ToAddress()));
    result.push_back(Pair("script", "script_content"));
    result.push_back(Pair("fees", llFees));
    result.push_back(Pair("valid_height", nValidHeight));
    return result;
}

bool CRegisterContractTx::CheckTx(CValidationState &state, CAccountViewCache &view,
                                           CScriptDBViewCache &scriptDB) {
    CDataStream stream(script, SER_DISK, CLIENT_VERSION);
    CVmScript vmScript;
    try {
        stream >> vmScript;
    } catch (exception &e) {
        return state.DoS(100,
                         ERRORMSG("CRegisterContractTx::CheckTx, unserialize to vmScript error"),
                         REJECT_INVALID, "unserialize-error");
    }

    if (!vmScript.IsValid()) {
        return state.DoS(100, ERRORMSG("CRegisterContractTx::CheckTx, vmScript is invalid"),
                         REJECT_INVALID, "vmscript-invalid");
    }

    if (regAcctId.type() != typeid(CRegID)) {
        return state.DoS(100, ERRORMSG("CRegisterContractTx::CheckTx, regAcctId must be CRegID"),
                         REJECT_INVALID, "regacctid-type-error");
    }

    if (!CheckMoneyRange(llFees)) {
        return state.DoS(100, ERRORMSG("CRegisterContractTx::CheckTx, tx fee out of range"),
                         REJECT_INVALID, "fee-too-large");
    }

    if (!CheckMinTxFee(llFees)) {
        return state.DoS(
            100, ERRORMSG("CRegisterContractTx::CheckTx, tx fee smaller than MinTxFee"),
            REJECT_INVALID, "bad-tx-fee-toosmall");
    }

    uint64_t llFuel = ceil(script.size() / 100) * GetFuelRate(scriptDB);
    if (llFuel < 1 * COIN) {
        llFuel = 1 * COIN;
    }

    if (llFees < llFuel) {
        return state.DoS(100,
                         ERRORMSG("CRegisterContractTx::CheckTx, register app tx fee too litter "
                                  "(actual:%lld vs need:%lld)",
                                  llFees, llFuel),
                         REJECT_INVALID, "fee-too-litter");
    }

    CAccount acctInfo;
    if (!view.GetAccount(regAcctId.get<CRegID>(), acctInfo)) {
        return state.DoS(100, ERRORMSG("CRegisterContractTx::CheckTx, get account falied"),
                         REJECT_INVALID, "bad-getaccount");
    }

    if (!acctInfo.IsRegistered()) {
        return state.DoS(
            100, ERRORMSG("CRegisterContractTx::CheckTx, account have not registed public key"),
            REJECT_INVALID, "bad-no-pubkey");
    }

    if (!CheckSignatureSize(signature)) {
        return state.DoS(100, ERRORMSG("CRegisterContractTx::CheckTx, signature size invalid"),
                         REJECT_INVALID, "bad-tx-sig-size");
    }

    uint256 signhash = SignatureHash();
    if (!CheckSignScript(signhash, signature, acctInfo.pubKey)) {
        return state.DoS(100, ERRORMSG("CRegisterContractTx::CheckTx, CheckSignScript failed"),
                         REJECT_INVALID, "bad-signscript-check");
    }
    return true;
}

string CDelegateTx::ToString(CAccountViewCache &view) const {
    string str;
    CKeyID keyId;
    view.GetKeyId(userId, keyId);
    str += strprintf("txType=%s, hash=%s, ver=%d, address=%s, keyid=%s\n", GetTxType(nTxType),
        GetHash().ToString().c_str(), nVersion, keyId.ToAddress(), keyId.ToString());
    str += "vote:\n";
    for (auto item = operVoteFunds.begin(); item != operVoteFunds.end(); ++item) {
        str += strprintf("%s", item->ToString());
    }
    return str;
}

Object CDelegateTx::ToJson(const CAccountViewCache &accountView) const {
    Object result;
    CAccountViewCache view(accountView);
    CKeyID keyId;
    result.push_back(Pair("hash", GetHash().GetHex()));
    result.push_back(Pair("txtype", GetTxType(nTxType)));
    result.push_back(Pair("ver", nVersion));
    result.push_back(Pair("regid", userId.get<CRegID>().ToString()));
    view.GetKeyId(userId, keyId);
    result.push_back(Pair("addr", keyId.ToAddress()));
    result.push_back(Pair("fees", llFees));
    Array operVoteFundArray;
    for (auto item = operVoteFunds.begin(); item != operVoteFunds.end(); ++item) {
        operVoteFundArray.push_back(item->ToJson());
    }
    result.push_back(Pair("operVoteFundList", operVoteFundArray));
    return result;
}

// FIXME: not useuful
bool CDelegateTx::GetAddress(set<CKeyID> &vAddr, CAccountViewCache &view,
                             CScriptDBViewCache &scriptDB) {
    // CKeyID keyId;
    // if (!view.GetKeyId(userId, keyId))
    //     return false;

    // vAddr.insert(keyId);
    // for (auto iter = operVoteFunds.begin(); iter != operVoteFunds.end(); ++iter) {
    //     vAddr.insert(iter->fund.GetVoteId());
    // }
    return true;
}

bool CDelegateTx::ExecuteTx(int nIndex, CAccountViewCache &view, CValidationState &state,
                            CTxUndo &txundo, int nHeight, CTransactionDBCache &txCache,
                            CScriptDBViewCache &scriptDB) {
    CAccount acctInfo;
    if (!view.GetAccount(userId, acctInfo)) {
        return state.DoS(100, ERRORMSG("CDelegateTx::ExecuteTx, read regist addr %s account info error", userId.ToString()),
            UPDATE_ACCOUNT_FAIL, "bad-read-accountdb");
    }
    CAccount acctInfoLog(acctInfo);
    uint64_t minusValue = llFees;
    if (minusValue > 0) {
        if(!acctInfo.OperateAccount(MINUS_FREE, minusValue, nHeight))
            return state.DoS(100, ERRORMSG("CDelegateTx::ExecuteTx, operate account failed ,regId=%s", userId.ToString()),
                UPDATE_ACCOUNT_FAIL, "operate-account-failed");
    }
    if (!acctInfo.ProcessDelegateVote(operVoteFunds, nHeight)) {
        return state.DoS(100, ERRORMSG("CDelegateTx::ExecuteTx, operate delegate vote failed ,regId=%s", userId.ToString()),
            UPDATE_ACCOUNT_FAIL, "operate-delegate-failed");
    }
    if (!view.SaveAccountInfo(acctInfo.regID, acctInfo.keyID, acctInfo)) {
            return state.DoS(100, ERRORMSG("CDelegateTx::ExecuteTx, create new account script id %s script info error", acctInfo.regID.ToString()),
                UPDATE_ACCOUNT_FAIL, "bad-save-scriptdb");
    }
    txundo.vAccountLog.push_back(acctInfoLog);
    txundo.txHash = GetHash();

    for (auto iter = operVoteFunds.begin(); iter != operVoteFunds.end(); ++iter) {
        CAccount delegate;
        const CUserID &delegateUId = iter->fund.GetVoteId();
        if (!view.GetAccount(delegateUId, delegate)) {
            return state.DoS(100, ERRORMSG("CDelegateTx::ExecuteTx, read KeyId(%s) account info error",
                            delegateUId.ToString()), UPDATE_ACCOUNT_FAIL, "bad-read-accountdb");
        }
        CAccount delegateAcctLog(delegate);
        if (!delegate.OperateVote(VoteOperType(iter->operType), iter->fund.GetVoteCount())) {
            return state.DoS(100, ERRORMSG("CDelegateTx::ExecuteTx, operate delegate address %s vote fund error",
                            delegateUId.ToString()), UPDATE_ACCOUNT_FAIL, "operate-vote-error");
        }
        txundo.vAccountLog.push_back(delegateAcctLog);
        // set the new value and erase the old value
        CScriptDBOperLog operDbLog;
        if (!scriptDB.SetDelegateData(delegate, operDbLog)) {
            return state.DoS(100, ERRORMSG("CDelegateTx::ExecuteTx, erase account id %s vote info error",
                            delegate.regID.ToString()), UPDATE_ACCOUNT_FAIL, "bad-save-scriptdb");
        }
        txundo.vScriptOperLog.push_back(operDbLog);

        CScriptDBOperLog eraseDbLog;
        if (delegateAcctLog.receivedVotes > 0) {
            if(!scriptDB.EraseDelegateData(delegateAcctLog, eraseDbLog)) {
                return state.DoS(100, ERRORMSG("CDelegateTx::ExecuteTx, erase account id %s vote info error", delegateAcctLog.regID.ToString()),
                    UPDATE_ACCOUNT_FAIL, "bad-save-scriptdb");
            }
        }
        txundo.vScriptOperLog.push_back(eraseDbLog);

        if (!view.SaveAccountInfo(delegate.regID, delegate.keyID, delegate)) {
            return state.DoS(100, ERRORMSG("CDelegateTx::ExecuteTx, create new account script id %s script info error", acctInfo.regID.ToString()),
                UPDATE_ACCOUNT_FAIL, "bad-save-scriptdb");
        }
    }

    if (SysCfg().GetAddressToTxFlag()) {
        CScriptDBOperLog operAddressToTxLog;
        CKeyID sendKeyId;
        if (!view.GetKeyId(userId, sendKeyId)) {
            return ERRORMSG("CDelegateTx::ExecuteTx, get regAcctId by account error!");
        }
        if (!scriptDB.SetTxHashByAddress(sendKeyId, nHeight, nIndex+1, txundo.txHash.GetHex(), operAddressToTxLog))
            return false;
        txundo.vScriptOperLog.push_back(operAddressToTxLog);
    }
    return true;
}

bool CDelegateTx::UndoExecuteTx(int nIndex, CAccountViewCache &view, CValidationState &state,
                                CTxUndo &txundo, int nHeight, CTransactionDBCache &txCache,
                                CScriptDBViewCache &scriptDB) {
    vector<CAccountLog>::reverse_iterator rIterAccountLog = txundo.vAccountLog.rbegin();
    for (; rIterAccountLog != txundo.vAccountLog.rend(); ++rIterAccountLog) {
        CAccount account;
        CUserID userId = rIterAccountLog->keyID;
        if (!view.GetAccount(userId, account)) {
            return state.DoS(100, ERRORMSG("CDelegateTx::UndoExecuteTx, read account info error"),
                             READ_ACCOUNT_FAIL, "bad-read-accountdb");
        }

        if (!account.UndoOperateAccount(*rIterAccountLog)) {
            return state.DoS(100,
                             ERRORMSG("CDelegateTx::UndoExecuteTx, undo operate account failed"),
                             UPDATE_ACCOUNT_FAIL, "undo-operate-account-failed");
        }

        if (!view.SetAccount(userId, account)) {
            return state.DoS(100, ERRORMSG("CDelegateTx::UndoExecuteTx, write account info error"),
                             UPDATE_ACCOUNT_FAIL, "bad-write-accountdb");
        }
    }

    vector<CScriptDBOperLog>::reverse_iterator rIterScriptDBLog = txundo.vScriptOperLog.rbegin();
    if (SysCfg().GetAddressToTxFlag() && txundo.vScriptOperLog.size() > 0) {
        if (!scriptDB.UndoScriptData(rIterScriptDBLog->vKey, rIterScriptDBLog->vValue))
            return state.DoS(100, ERRORMSG("CDelegateTx::UndoExecuteTx, undo scriptdb data error"),
                             UPDATE_ACCOUNT_FAIL, "bad-save-scriptdb");
        ++rIterScriptDBLog;
    }

    for (; rIterScriptDBLog != txundo.vScriptOperLog.rend(); ++rIterScriptDBLog) {
        // Recover the old value and erase the new value.
        if (!scriptDB.SetDelegateData(rIterScriptDBLog->vKey))
            return state.DoS(100, ERRORMSG("CDelegateTx::UndoExecuteTx, set delegate data error"),
                             UPDATE_ACCOUNT_FAIL, "bad-save-scriptdb");

        ++rIterScriptDBLog;
        if (!scriptDB.EraseDelegateData(rIterScriptDBLog->vKey))
            return state.DoS(100, ERRORMSG("CDelegateTx::UndoExecuteTx, erase delegate data error"),
                             UPDATE_ACCOUNT_FAIL, "bad-save-scriptdb");
    }

    return true;
}

bool CDelegateTx::CheckTx(CValidationState &state, CAccountViewCache &view,
                          CScriptDBViewCache &scriptDB) {
    if (userId.type() != typeid(CRegID)) {
        return state.DoS(100, ERRORMSG("CDelegateTx::CheckTx, send account is not CRegID type"),
            REJECT_INVALID, "deletegate-tx-error");
    }
    if (0 == operVoteFunds.size()) {
        return state.DoS(100, ERRORMSG("CDelegateTx::CheckTx, the deletegate oper fund empty"),
            REJECT_INVALID, "oper-fund-empty-error");
    }
    if (operVoteFunds.size() > IniCfg().GetDelegatesNum()) {
        return state.DoS(100, ERRORMSG("CDelegateTx::CheckTx, the deletegates number a transaction can't exceeds maximum"),
            REJECT_INVALID, "deletegates-number-error");
    }
    if (!CheckMoneyRange(llFees))
        return state.DoS(100, ERRORMSG("CDelegateTx::CheckTx, delegate tx fee out of range"),
            REJECT_INVALID, "bad-tx-fee-toolarge");

    if (!CheckMinTxFee(llFees)) {
        return state.DoS(100, ERRORMSG("CDelegateTx::CheckTx, tx fee smaller than MinTxFee"),
            REJECT_INVALID, "bad-tx-fee-toosmall");
    }

    CKeyID sendTxKeyID;
    if(!view.GetKeyId(userId, sendTxKeyID)) {
        return state.DoS(100, ERRORMSG("CDelegateTx::CheckTx, get keyId error by CUserID =%s", userId.ToString()), REJECT_INVALID, "");
    }

    CAccount sendAcct;
    if (!view.GetAccount(userId, sendAcct)) {
        return state.DoS(100, ERRORMSG("CDelegateTx::CheckTx, get account info error, userid=%s", userId.ToString()),
            REJECT_INVALID, "bad-read-accountdb");
    }
    if (!sendAcct.IsRegistered()) {
        return state.DoS(100, ERRORMSG("CDelegateTx::CheckTx, pubkey not registed"),
            REJECT_INVALID, "bad-no-pubkey");
    }

    if ( GetFeatureForkVersion(chainActive.Tip()->nHeight) == MAJOR_VER_R2 ) {
        if (!CheckSignatureSize(signature)) {
            return state.DoS(100, ERRORMSG("CDelegateTx::CheckTx, signature size invalid"),
                REJECT_INVALID, "bad-tx-sig-size");
        }

        uint256 signhash = SignatureHash();
        if (!CheckSignScript(signhash, signature, sendAcct.pubKey)) {
            return state.DoS(100, ERRORMSG("CDelegateTx::CheckTx, CheckSignScript failed"),
                REJECT_INVALID, "bad-signscript-check");
        }
    }

    // check delegate duplication
    set<string> setOperVoteKeyID;
    for (auto item = operVoteFunds.begin(); item != operVoteFunds.end(); ++item) {
        if (0 >= item->fund.GetVoteCount() || (uint64_t)GetMaxMoney() < item->fund.GetVoteCount())
            return ERRORMSG("CDelegateTx::CheckTx, votes: %lld not within (0 .. MaxVote)",
                            item->fund.GetVoteCount());

        setOperVoteKeyID.insert(item->fund.ToString());
        CAccount acctInfo;
        if (!view.GetAccount(item->fund.GetVoteId(), acctInfo))
            return state.DoS(100,
                             ERRORMSG("CDelegateTx::CheckTx, get account info error, address=%s",
                                      item->fund.ToString()),
                             REJECT_INVALID, "bad-read-accountdb");

        if (GetFeatureForkVersion(chainActive.Tip()->nHeight) == MAJOR_VER_R2) {
            if (!acctInfo.IsRegistered()) {
                return state.DoS(
                    100,
                    ERRORMSG("CDelegateTx::CheckTx, account is unregistered, address=%s",
                             item->fund.ToString()),
                    REJECT_INVALID, "bad-read-accountdb");
            }
        }
    }

    if (setOperVoteKeyID.size() != operVoteFunds.size()) {
        return state.DoS(100, ERRORMSG("CDelegateTx::CheckTx, duplication vote fund"),
                         REJECT_INVALID, "deletegates-duplication fund-error");
    }

    return true;
}

string CTxUndo::ToString() const {
    vector<CAccountLog>::const_iterator iterLog = vAccountLog.begin();
    string strTxHash("txHash:");
    strTxHash += txHash.GetHex();
    strTxHash += "\n";
    string str("  list account Log:\n");
    for (; iterLog != vAccountLog.end(); ++iterLog) {
        str += iterLog->ToString();
    }
    strTxHash += str;
    vector<CScriptDBOperLog>::const_iterator iterDbLog = vScriptOperLog.begin();
    string strDbLog(" list script db Log:\n");
    for (; iterDbLog !=  vScriptOperLog.end(); ++iterDbLog) {
        strDbLog += iterDbLog->ToString();
    }
    strTxHash += strDbLog;
    return strTxHash;
}

bool CTxUndo::GetAccountOperLog(const CKeyID &keyId, CAccountLog &accountLog) {
    vector<CAccountLog>::iterator iterLog = vAccountLog.begin();
    for (; iterLog != vAccountLog.end(); ++iterLog) {
        if (iterLog->keyID == keyId) {
            accountLog = *iterLog;
            return true;
        }
    }
    return false;
}

string CSignaturePair::ToString() const {
    string str = strprintf("regId=%s, signature=%s", regId.ToString(),
                           HexStr(signature.begin(), signature.end()));
    return str;
}

Object CSignaturePair::ToJson() const {
    Object obj;
    obj.push_back(Pair("regid", regId.ToString()));
    obj.push_back(Pair("signature", HexStr(signature.begin(), signature.end())));

    return obj;
}

string CMulsigTx::ToString(CAccountViewCache &view) const {
    string desId;
    if (desUserId.type() == typeid(CKeyID)) {
        desId = desUserId.get<CKeyID>().ToString();
    } else if (desUserId.type() == typeid(CRegID)) {
        desId = desUserId.get<CRegID>().ToString();
    }

    string signatures;
    signatures += "signatures: ";
    for (const auto &item : signaturePairs) {
        signatures += strprintf("%s, ", item.ToString());
    }
    string str = strprintf(
        "txType=%s, hash=%s, ver=%d, required=%d, %s, desId=%s, bcoins=%ld, llFees=%ld, "
        "memo=%s,  nValidHeight=%d\n",
        GetTxType(nTxType), GetHash().ToString(), nVersion, required, signatures, desId,
        bcoins, llFees, HexStr(memo), nValidHeight);

    return str;
}

Object CMulsigTx::ToJson(const CAccountViewCache &AccountView) const {
    Object result;
    CAccountViewCache view(AccountView);

    auto GetRegIdString = [&](CUserID const &userId) {
        if (userId.type() == typeid(CRegID)) {
            return userId.get<CRegID>().ToString();
        }
        return string("");
    };

    CKeyID desKeyId;
    view.GetKeyId(desUserId, desKeyId);

    result.push_back(Pair("hash", GetHash().GetHex()));
    result.push_back(Pair("tx_type", GetTxType(nTxType)));
    result.push_back(Pair("ver", nVersion));
    result.push_back(Pair("required_sigs", required));
    Array signatureArray;
    CAccount account;
    std::set<CPubKey> pubKeys;
    for (const auto &item : signaturePairs) {
        signatureArray.push_back(item.ToJson());
        if (!view.GetAccount(item.regId, account)) {
            LogPrint("ERROR", "CMulsigTx::ToJson, failed to get account info: %s\n",
                     item.regId.ToString());
            continue;
        }
        pubKeys.insert(account.pubKey);
    }
    CMulsigScript script;
    script.SetMultisig(required, pubKeys);
    CKeyID scriptId = script.GetID();

    result.push_back(Pair("addr", scriptId.ToAddress()));
    result.push_back(Pair("signatures", signatureArray));
    result.push_back(Pair("dest_regid", GetRegIdString(desUserId)));
    result.push_back(Pair("dest_addr", desKeyId.ToAddress()));
    result.push_back(Pair("money", bcoins));
    result.push_back(Pair("fees", llFees));
    result.push_back(Pair("memo", HexStr(memo)));
    result.push_back(Pair("valid_height", nValidHeight));

    return result;
}

bool CMulsigTx::GetAddress(set<CKeyID> &vAddr, CAccountViewCache &view,
                           CScriptDBViewCache &scriptDB) {
    CKeyID keyId;
    for (const auto &item : signaturePairs) {
        if (!view.GetKeyId(CUserID(item.regId), keyId)) return false;
        vAddr.insert(keyId);
    }

    CKeyID desKeyId;
    if (!view.GetKeyId(desUserId, desKeyId)) return false;
    vAddr.insert(desKeyId);

    return true;
}

bool CMulsigTx::ExecuteTx(int nIndex, CAccountViewCache &view, CValidationState &state,
                          CTxUndo &txundo, int nHeight, CTransactionDBCache &txCache,
                          CScriptDBViewCache &scriptDB) {
    CAccount srcAcct;
    CAccount desAcct;
    bool generateRegID = false;

    if (!view.GetAccount(CUserID(keyId), srcAcct)) {
        return state.DoS(100, ERRORMSG("CMulsigTx::ExecuteTx, read source addr account info error"),
                         READ_ACCOUNT_FAIL, "bad-read-accountdb");
    } else {
        CRegID regId;
        // If the source account does NOT have CRegID, need to generate a new CRegID.
        if (!view.GetRegId(CUserID(keyId), regId)) {
            srcAcct.regID = CRegID(nHeight, nIndex);
            generateRegID = true;
        }
    }

    CAccountLog srcAcctLog(srcAcct);
    CAccountLog desAcctLog;
    uint64_t minusValue = llFees + bcoins;
    if (!srcAcct.OperateAccount(MINUS_FREE, minusValue, nHeight))
        return state.DoS(100, ERRORMSG("CMulsigTx::ExecuteTx, account has insufficient funds"),
                         UPDATE_ACCOUNT_FAIL, "operate-minus-account-failed");

    if (generateRegID) {
        if (!view.SaveAccountInfo(srcAcct.regID, srcAcct.keyID, srcAcct))
            return state.DoS(100, ERRORMSG("CMulsigTx::ExecuteTx, save account info error"),
                             WRITE_ACCOUNT_FAIL, "bad-write-accountdb");
    } else {
        if (!view.SetAccount(CUserID(srcAcct.keyID), srcAcct))
            return state.DoS(100, ERRORMSG("CMulsigTx::ExecuteTx, save account info error"),
                             WRITE_ACCOUNT_FAIL, "bad-write-accountdb");
    }

    uint64_t addValue = bcoins;
    if (!view.GetAccount(desUserId, desAcct)) {
        if (desUserId.type() == typeid(CKeyID)) {  // target account does NOT have CRegID
            desAcct.keyID    = desUserId.get<CKeyID>();
            desAcctLog.keyID = desAcct.keyID;
        } else {
            return state.DoS(100, ERRORMSG("CMulsigTx::ExecuteTx, get account info failed"),
                             READ_ACCOUNT_FAIL, "bad-read-accountdb");
        }
    } else {  // target account has NO CAccount(first involved in transacion)
        desAcctLog.SetValue(desAcct);
    }

    if (!desAcct.OperateAccount(ADD_FREE, addValue, nHeight))
        return state.DoS(100, ERRORMSG("CMulsigTx::ExecuteTx, operate accounts error"),
                         UPDATE_ACCOUNT_FAIL, "operate-add-account-failed");

    if (!view.SetAccount(desUserId, desAcct))
        return state.DoS(100,
                         ERRORMSG("CMulsigTx::ExecuteTx, save account error, kyeId=%s",
                                  desAcct.keyID.ToString()),
                         UPDATE_ACCOUNT_FAIL, "bad-save-account");

    txundo.vAccountLog.push_back(srcAcctLog);
    txundo.vAccountLog.push_back(desAcctLog);
    txundo.txHash = GetHash();

    if (SysCfg().GetAddressToTxFlag()) {
        CScriptDBOperLog operAddressToTxLog;
        CKeyID sendKeyId;
        CKeyID revKeyId;

        for (const auto &item : signaturePairs) {
            if (!view.GetKeyId(CUserID(item.regId), sendKeyId))
                return ERRORMSG("CCommonTx::CMulsigTx, get keyid by srcUserId error!");

            if (!scriptDB.SetTxHashByAddress(sendKeyId, nHeight, nIndex + 1, txundo.txHash.GetHex(),
                                             operAddressToTxLog))
                return false;
            txundo.vScriptOperLog.push_back(operAddressToTxLog);
        }

        if (!view.GetKeyId(desUserId, revKeyId))
            return ERRORMSG("CCommonTx::CMulsigTx, get keyid by desUserId error!");

        if (!scriptDB.SetTxHashByAddress(revKeyId, nHeight, nIndex + 1, txundo.txHash.GetHex(),
                                         operAddressToTxLog))
            return false;
        txundo.vScriptOperLog.push_back(operAddressToTxLog);
    }

    return true;
}

bool CMulsigTx::UndoExecuteTx(int nIndex, CAccountViewCache &view, CValidationState &state,
                              CTxUndo &txundo, int nHeight, CTransactionDBCache &txCache,
                              CScriptDBViewCache &scriptDB) {
    vector<CAccountLog>::reverse_iterator rIterAccountLog = txundo.vAccountLog.rbegin();
    for (; rIterAccountLog != txundo.vAccountLog.rend(); ++rIterAccountLog) {
        CAccount account;
        CUserID userId = rIterAccountLog->keyID;

        if (!view.GetAccount(userId, account)) {
            return state.DoS(100, ERRORMSG("CMulsigTx::UndoExecuteTx, read account info error"),
                             READ_ACCOUNT_FAIL, "bad-read-accountdb");
        }

        if (!account.UndoOperateAccount(*rIterAccountLog)) {
            return state.DoS(100, ERRORMSG("CMulsigTx::UndoExecuteTx, undo operate account failed"),
                             UPDATE_ACCOUNT_FAIL, "undo-operate-account-failed");
        }

        if (account.IsEmptyValue() && account.regID.IsEmpty()) {
            view.EraseAccount(userId);
        } else if (account.regID == CRegID(nHeight, nIndex)) {
            // If the CRegID was generated by this MULSIG_TX, need to remove CRegID.
            CPubKey empPubKey;
            account.pubKey      = empPubKey;
            account.minerPubKey = empPubKey;
            account.regID.Clean();

            if (!view.SetAccount(userId, account)) {
                return state.DoS(100, ERRORMSG("CBaseTx::UndoExecuteTx, write account info error"),
                                 UPDATE_ACCOUNT_FAIL, "bad-write-accountdb");
            }

            view.EraseId(CRegID(nHeight, nIndex));
        } else {
            if (!view.SetAccount(userId, account)) {
                return state.DoS(100,
                                 ERRORMSG("CMulsigTx::UndoExecuteTx, write account info error"),
                                 UPDATE_ACCOUNT_FAIL, "bad-write-accountdb");
            }
        }
    }

    vector<CScriptDBOperLog>::reverse_iterator rIterScriptDBLog = txundo.vScriptOperLog.rbegin();
    for (; rIterScriptDBLog != txundo.vScriptOperLog.rend(); ++rIterScriptDBLog) {
        if (!scriptDB.UndoScriptData(rIterScriptDBLog->vKey, rIterScriptDBLog->vValue))
            return state.DoS(100, ERRORMSG("CMulsigTx::UndoExecuteTx, undo scriptdb data error"),
                             UPDATE_ACCOUNT_FAIL, "bad-save-scriptdb");
    }

    return true;
}

bool CMulsigTx::CheckTx(CValidationState &state, CAccountViewCache &view,
                          CScriptDBViewCache &scriptDB) {
    if (memo.size() > kCommonTxMemoMaxSize)
        return state.DoS(100, ERRORMSG("CMulsigTx::CheckTx, memo's size too large"),
                         REJECT_INVALID, "memo-size-toolarge");

    if (required < 1 || required > signaturePairs.size()) {
        return state.DoS(100, ERRORMSG("CMulsigTx::CheckTx, required keys invalid"),
                         REJECT_INVALID, "required-keys-invalid");
    }

    if (signaturePairs.size() > kMultisigNumberThreshold) {
        return state.DoS(100, ERRORMSG("CMulsigTx::CheckTx, signature's number out of range"),
                         REJECT_INVALID, "signature-number-out-of-range");
    }

    if ((desUserId.type() != typeid(CRegID)) && (desUserId.type() != typeid(CKeyID)))
        return state.DoS(100, ERRORMSG("CMulsigTx::CheckTx, desaddr type error"), REJECT_INVALID,
                         "desaddr-type-error");

    if (!CheckMoneyRange(llFees))
        return state.DoS(100, ERRORMSG("CMulsigTx::CheckTx, tx fees out of money range"),
                         REJECT_INVALID, "bad-appeal-fees-toolarge");

    if (!CheckMinTxFee(llFees)) {
        return state.DoS(100, ERRORMSG("CMulsigTx::CheckTx, tx fees smaller than MinTxFee"),
                         REJECT_INVALID, "bad-tx-fees-toosmall");
    }

    CAccount account;
    set<CPubKey> pubKeys;
    uint256 sighash = SignatureHash();
    uint8_t valid   = 0;
    for (const auto &item : signaturePairs) {
        if (!view.GetAccount(item.regId, account))
            return state.DoS(100,
                             ERRORMSG("CMulsigTx::CheckTx, account: %s, read account failed",
                                      item.regId.ToString()),
                             REJECT_INVALID, "bad-getaccount");

        if (!item.signature.empty()) {
            if (!CheckSignatureSize(item.signature)) {
                return state.DoS(100,
                                 ERRORMSG("CMulsigTx::CheckTx, account: %s, signature size invalid",
                                          item.regId.ToString()),
                                 REJECT_INVALID, "bad-tx-sig-size");
            }

            if (!CheckSignScript(sighash, item.signature, account.pubKey)) {
                return state.DoS(100,
                                 ERRORMSG("CMulsigTx::CheckTx, account: %s, CheckSignScript failed",
                                          item.regId.ToString()),
                                 REJECT_INVALID, "bad-signscript-check");
            } else {
                ++valid;
            }
        }

        pubKeys.insert(account.pubKey);
    }

    if (pubKeys.size() != signaturePairs.size()) {
        return state.DoS(100, ERRORMSG("CMulsigTx::CheckTx, duplicated account"), REJECT_INVALID,
                         "duplicated-account");
    }

    if (valid < required) {
        return state.DoS(
            100,
            ERRORMSG("CMulsigTx::CheckTx, not enough valid signatures, %u vs %u", valid, required),
            REJECT_INVALID, "not-enough-valid-signatures");
    }

    CMulsigScript script;
    script.SetMultisig(required, pubKeys);
    keyId = script.GetID();

    CAccount srcAccount;
    if (!view.GetAccount(CUserID(keyId), srcAccount))
        return state.DoS(100, ERRORMSG("CMulsigTx::CheckTx, read multisig account: %s failed", keyId.ToAddress()),
                         READ_ACCOUNT_FAIL, "bad-read-accountdb");

    return true;
}
