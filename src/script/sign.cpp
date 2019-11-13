// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <script/sign.h>

#include <key.h>
#include <policy/policy.h>
#include <primitives/transaction.h>
#include <script/standard.h>
#include <uint256.h>

typedef std::vector<uint8_t> valtype;

MutableTransactionSignatureCreator::MutableTransactionSignatureCreator(
    const CMutableTransaction *txToIn, unsigned int nInIn,
    const Amount &amountIn, SigHashType sigHashTypeIn)
    : txTo(txToIn), nIn(nInIn), amount(amountIn), sigHashType(sigHashTypeIn),
      checker(txTo, nIn, amountIn) {}

bool MutableTransactionSignatureCreator::CreateSig(
    const SigningProvider &provider, std::vector<uint8_t> &vchSig,
    const CKeyID &address, const CScript &scriptCode) const {
    CKey key;
    if (!provider.GetKey(address, key)) {
        return false;
    }

    uint256 hash = SignatureHash(scriptCode, *txTo, nIn, sigHashType, amount);
    if (!key.SignECDSA(hash, vchSig)) {
        return false;
    }

    vchSig.push_back(uint8_t(sigHashType.getRawSigHashType()));
    return true;
}

static bool GetCScript(const SigningProvider &provider,
                       const SignatureData &sigdata, const CScriptID &scriptid,
                       CScript &script) {
    if (provider.GetCScript(scriptid, script)) {
        return true;
    }
    // Look for scripts in SignatureData
    if (CScriptID(sigdata.redeem_script) == scriptid) {
        script = sigdata.redeem_script;
        return true;
    }
    return false;
}

static bool GetPubKey(const SigningProvider &provider, SignatureData &sigdata,
                      const CKeyID &address, CPubKey &pubkey) {
    if (provider.GetPubKey(address, pubkey)) {
        sigdata.misc_pubkeys.emplace(pubkey.GetID(), pubkey);
        return true;
    }
    // Look for pubkey in all partial sigs
    const auto it = sigdata.signatures.find(address);
    if (it != sigdata.signatures.end()) {
        pubkey = it->second.first;
        return true;
    }
    // Look for pubkey in pubkey list
    const auto &pk_it = sigdata.misc_pubkeys.find(address);
    if (pk_it != sigdata.misc_pubkeys.end()) {
        pubkey = pk_it->second;
        return true;
    }
    return false;
}

static bool CreateSig(const BaseSignatureCreator &creator,
                      SignatureData &sigdata, const SigningProvider &provider,
                      std::vector<uint8_t> &sig_out, const CKeyID &keyid,
                      const CScript &scriptcode) {
    const auto it = sigdata.signatures.find(keyid);
    if (it != sigdata.signatures.end()) {
        sig_out = it->second.second;
        return true;
    }
    CPubKey pubkey;
    GetPubKey(provider, sigdata, keyid, pubkey);
    if (creator.CreateSig(provider, sig_out, keyid, scriptcode)) {
        auto i = sigdata.signatures.emplace(keyid, SigPair(pubkey, sig_out));
        assert(i.second);
        return true;
    }
    return false;
}

/**
 * Sign scriptPubKey using signature made with creator.
 * Signatures are returned in scriptSigRet (or returns false if scriptPubKey
 * can't be signed), unless whichTypeRet is TX_SCRIPTHASH, in which case
 * scriptSigRet is the redemption script.
 * Returns false if scriptPubKey could not be completely satisfied.
 */
static bool SignStep(const SigningProvider &provider,
                     const BaseSignatureCreator &creator,
                     const CScript &scriptPubKey, std::vector<valtype> &ret,
                     txnouttype &whichTypeRet, SignatureData &sigdata) {
    CScript scriptRet;
    uint160 h160;
    ret.clear();
    std::vector<uint8_t> sig;

    std::vector<valtype> vSolutions;
    if (!Solver(scriptPubKey, whichTypeRet, vSolutions)) {
        return false;
    }

    switch (whichTypeRet) {
        case TX_NONSTANDARD:
        case TX_NULL_DATA:
            return false;
        case TX_PUBKEY:
            if (!CreateSig(creator, sigdata, provider, sig,
                           CPubKey(vSolutions[0]).GetID(), scriptPubKey)) {
                return false;
            }
            ret.push_back(std::move(sig));
            return true;
        case TX_PUBKEYHASH: {
            CKeyID keyID = CKeyID(uint160(vSolutions[0]));
            if (!CreateSig(creator, sigdata, provider, sig, keyID,
                           scriptPubKey)) {
                return false;
            }
            ret.push_back(std::move(sig));
            CPubKey pubkey;
            provider.GetPubKey(keyID, pubkey);
            ret.push_back(ToByteVector(pubkey));
            return true;
        }
        case TX_SCRIPTHASH:
            if (GetCScript(provider, sigdata, uint160(vSolutions[0]),
                           scriptRet)) {
                ret.push_back(
                    std::vector<uint8_t>(scriptRet.begin(), scriptRet.end()));
                return true;
            }
            return false;
        case TX_MULTISIG: {
            size_t required = vSolutions.front()[0];
            // workaround CHECKMULTISIG bug
            ret.push_back(valtype());
            for (size_t i = 1; i < vSolutions.size() - 1; ++i) {
                CPubKey pubkey = CPubKey(vSolutions[i]);
                if (ret.size() < required + 1 &&
                    CreateSig(creator, sigdata, provider, sig, pubkey.GetID(),
                              scriptPubKey)) {
                    ret.push_back(std::move(sig));
                }
            }
            bool ok = ret.size() == required + 1;
            for (size_t i = 0; i + ret.size() < required + 1; ++i) {
                ret.push_back(valtype());
            }
            return ok;
        }
        default:
            return false;
    }
}

static CScript PushAll(const std::vector<valtype> &values) {
    CScript result;
    for (const valtype &v : values) {
        if (v.size() == 0) {
            result << OP_0;
        } else if (v.size() == 1 && v[0] >= 1 && v[0] <= 16) {
            result << CScript::EncodeOP_N(v[0]);
        } else {
            result << v;
        }
    }

    return result;
}

bool ProduceSignature(const SigningProvider &provider,
                      const BaseSignatureCreator &creator,
                      const CScript &fromPubKey, SignatureData &sigdata) {
    if (sigdata.complete) {
        return true;
    }

    std::vector<valtype> result;
    txnouttype whichType;
    bool solved =
        SignStep(provider, creator, fromPubKey, result, whichType, sigdata);
    CScript subscript;

    if (solved && whichType == TX_SCRIPTHASH) {
        // Solver returns the subscript that needs to be evaluated; the final
        // scriptSig is the signatures from that and then the serialized
        // subscript:
        subscript = CScript(result[0].begin(), result[0].end());
        sigdata.redeem_script = subscript;

        solved = solved &&
                 SignStep(provider, creator, subscript, result, whichType,
                          sigdata) &&
                 whichType != TX_SCRIPTHASH;
        result.push_back(
            std::vector<uint8_t>(subscript.begin(), subscript.end()));
    }

    sigdata.scriptSig = PushAll(result);

    // Test solution
    sigdata.complete =
        solved && VerifyScript(sigdata.scriptSig, fromPubKey,
                               STANDARD_SCRIPT_VERIFY_FLAGS, creator.Checker());
    return sigdata.complete;
}

bool SignPSBTInput(const SigningProvider &provider,
                   const CMutableTransaction &tx, PSBTInput &input,
                   SignatureData &sigdata, int index, SigHashType sighash) {
    // If this input has a final scriptsig, don't do anything with it.
    if (!input.final_script_sig.empty()) {
        return true;
    }

    // Fill SignatureData with input info
    input.FillSignatureData(sigdata);

    // Get UTXO
    CTxOut utxo;
    if (input.utxo.IsNull()) {
        return false;
    }

    utxo = input.utxo;
    MutableTransactionSignatureCreator creator(&tx, index, utxo.nValue,
                                               sighash);
    bool sig_complete =
        ProduceSignature(provider, creator, utxo.scriptPubKey, sigdata);
    input.FromSignatureData(sigdata);
    return sig_complete;
}

class SignatureExtractorChecker final : public BaseSignatureChecker {
private:
    SignatureData &sigdata;
    BaseSignatureChecker &checker;

public:
    SignatureExtractorChecker(SignatureData &sigdata_,
                              BaseSignatureChecker &checker_)
        : sigdata(sigdata_), checker(checker_) {}
    bool CheckSig(const std::vector<uint8_t> &scriptSig,
                  const std::vector<uint8_t> &vchPubKey,
                  const CScript &scriptCode, uint32_t flags) const override;
};

bool SignatureExtractorChecker::CheckSig(const std::vector<uint8_t> &scriptSig,
                                         const std::vector<uint8_t> &vchPubKey,
                                         const CScript &scriptCode,
                                         uint32_t flags) const {
    if (checker.CheckSig(scriptSig, vchPubKey, scriptCode, flags)) {
        CPubKey pubkey(vchPubKey);
        sigdata.signatures.emplace(pubkey.GetID(), SigPair(pubkey, scriptSig));
        return true;
    }
    return false;
}

namespace {
struct Stacks {
    std::vector<valtype> script;

    Stacks() {}
    explicit Stacks(const std::vector<valtype> &scriptSigStack_)
        : script(scriptSigStack_) {}
    explicit Stacks(const SignatureData &data) {
        EvalScript(script, data.scriptSig, MANDATORY_SCRIPT_VERIFY_FLAGS,
                   BaseSignatureChecker());
    }

    SignatureData Output() const {
        SignatureData result;
        result.scriptSig = PushAll(script);
        return result;
    }
};
} // namespace

// Extracts signatures and scripts from incomplete scriptSigs. Please do not
// extend this, use PSBT instead
SignatureData DataFromTransaction(const CMutableTransaction &tx,
                                  unsigned int nIn, const CTxOut &txout) {
    SignatureData data;
    assert(tx.vin.size() > nIn);
    data.scriptSig = tx.vin[nIn].scriptSig;
    Stacks stack(data);

    // Get signatures
    MutableTransactionSignatureChecker tx_checker(&tx, nIn, txout.nValue);
    SignatureExtractorChecker extractor_checker(data, tx_checker);
    if (VerifyScript(data.scriptSig, txout.scriptPubKey,
                     STANDARD_SCRIPT_VERIFY_FLAGS, extractor_checker)) {
        data.complete = true;
        return data;
    }

    // Get scripts
    txnouttype script_type;
    std::vector<std::vector<uint8_t>> solutions;
    Solver(txout.scriptPubKey, script_type, solutions);
    CScript next_script = txout.scriptPubKey;

    if (script_type == TX_SCRIPTHASH && !stack.script.empty() &&
        !stack.script.back().empty()) {
        // Get the redeemScript
        CScript redeem_script(stack.script.back().begin(),
                              stack.script.back().end());
        data.redeem_script = redeem_script;
        next_script = std::move(redeem_script);

        // Get redeemScript type
        Solver(next_script, script_type, solutions);
        stack.script.pop_back();
    }
    if (script_type == TX_MULTISIG && !stack.script.empty()) {
        // Build a map of pubkey -> signature by matching sigs to pubkeys:
        assert(solutions.size() > 1);
        unsigned int num_pubkeys = solutions.size() - 2;
        unsigned int last_success_key = 0;
        for (const valtype &sig : stack.script) {
            for (unsigned int i = last_success_key; i < num_pubkeys; ++i) {
                const valtype &pubkey = solutions[i + 1];
                // We either have a signature for this pubkey, or we have found
                // a signature and it is valid
                if (data.signatures.count(CPubKey(pubkey).GetID()) ||
                    extractor_checker.CheckSig(sig, pubkey, next_script,
                                               STANDARD_SCRIPT_VERIFY_FLAGS)) {
                    last_success_key = i + 1;
                    break;
                }
            }
        }
    }

    return data;
}

void UpdateInput(CTxIn &input, const SignatureData &data) {
    input.scriptSig = data.scriptSig;
}

void SignatureData::MergeSignatureData(SignatureData sigdata) {
    if (complete) {
        return;
    }
    if (sigdata.complete) {
        *this = std::move(sigdata);
        return;
    }
    if (redeem_script.empty() && !sigdata.redeem_script.empty()) {
        redeem_script = sigdata.redeem_script;
    }
    signatures.insert(std::make_move_iterator(sigdata.signatures.begin()),
                      std::make_move_iterator(sigdata.signatures.end()));
}

bool SignSignature(const SigningProvider &provider, const CScript &fromPubKey,
                   CMutableTransaction &txTo, unsigned int nIn,
                   const Amount amount, SigHashType sigHashType) {
    assert(nIn < txTo.vin.size());

    MutableTransactionSignatureCreator creator(&txTo, nIn, amount, sigHashType);

    SignatureData sigdata;
    bool ret = ProduceSignature(provider, creator, fromPubKey, sigdata);
    UpdateInput(txTo.vin.at(nIn), sigdata);
    return ret;
}

bool SignSignature(const SigningProvider &provider, const CTransaction &txFrom,
                   CMutableTransaction &txTo, unsigned int nIn,
                   SigHashType sigHashType) {
    assert(nIn < txTo.vin.size());
    CTxIn &txin = txTo.vin[nIn];
    assert(txin.prevout.GetN() < txFrom.vout.size());
    const CTxOut &txout = txFrom.vout[txin.prevout.GetN()];

    return SignSignature(provider, txout.scriptPubKey, txTo, nIn, txout.nValue,
                         sigHashType);
}

namespace {
/** Dummy signature checker which accepts all signatures. */
class DummySignatureChecker final : public BaseSignatureChecker {
public:
    DummySignatureChecker() {}
    bool CheckSig(const std::vector<uint8_t> &scriptSig,
                  const std::vector<uint8_t> &vchPubKey,
                  const CScript &scriptCode, uint32_t flags) const override {
        return true;
    }
};
const DummySignatureChecker DUMMY_CHECKER;

class DummySignatureCreator final : public BaseSignatureCreator {
public:
    DummySignatureCreator() {}
    const BaseSignatureChecker &Checker() const override {
        return DUMMY_CHECKER;
    }
    bool CreateSig(const SigningProvider &provider,
                   std::vector<uint8_t> &vchSig, const CKeyID &keyid,
                   const CScript &scriptCode) const override {
        // Create a dummy signature that is a valid DER-encoding
        vchSig.assign(72, '\000');
        vchSig[0] = 0x30;
        vchSig[1] = 69;
        vchSig[2] = 0x02;
        vchSig[3] = 33;
        vchSig[4] = 0x01;
        vchSig[4 + 33] = 0x02;
        vchSig[5 + 33] = 32;
        vchSig[6 + 33] = 0x01;
        vchSig[6 + 33 + 32] = SIGHASH_ALL | SIGHASH_FORKID;
        return true;
    }
};
} // namespace

const BaseSignatureCreator &DUMMY_SIGNATURE_CREATOR = DummySignatureCreator();
const SigningProvider &DUMMY_SIGNING_PROVIDER = SigningProvider();

bool PartiallySignedTransaction::IsNull() const {
    return !tx && inputs.empty() && outputs.empty() && unknown.empty();
}

void PartiallySignedTransaction::Merge(const PartiallySignedTransaction &psbt) {
    for (size_t i = 0; i < inputs.size(); ++i) {
        inputs[i].Merge(psbt.inputs[i]);
    }
    for (size_t i = 0; i < outputs.size(); ++i) {
        outputs[i].Merge(psbt.outputs[i]);
    }
}

bool PartiallySignedTransaction::IsSane() const {
    for (PSBTInput input : inputs) {
        if (!input.IsSane()) {
            return false;
        }
    }
    return true;
}

bool PSBTInput::IsNull() const {
    return utxo.IsNull() && partial_sigs.empty() && unknown.empty() &&
           hd_keypaths.empty() && redeem_script.empty();
}

void PSBTInput::FillSignatureData(SignatureData &sigdata) const {
    if (!final_script_sig.empty()) {
        sigdata.scriptSig = final_script_sig;
        sigdata.complete = true;
    }
    if (sigdata.complete) {
        return;
    }

    sigdata.signatures.insert(partial_sigs.begin(), partial_sigs.end());
    if (!redeem_script.empty()) {
        sigdata.redeem_script = redeem_script;
    }
    for (const auto &key_pair : hd_keypaths) {
        sigdata.misc_pubkeys.emplace(key_pair.first.GetID(), key_pair.first);
    }
}

void PSBTInput::FromSignatureData(const SignatureData &sigdata) {
    if (sigdata.complete) {
        partial_sigs.clear();
        hd_keypaths.clear();
        redeem_script.clear();

        if (!sigdata.scriptSig.empty()) {
            final_script_sig = sigdata.scriptSig;
        }
        return;
    }

    partial_sigs.insert(sigdata.signatures.begin(), sigdata.signatures.end());
    if (redeem_script.empty() && !sigdata.redeem_script.empty()) {
        redeem_script = sigdata.redeem_script;
    }
}

void PSBTInput::Merge(const PSBTInput &input) {
    if (utxo.IsNull() && !input.utxo.IsNull()) {
        utxo = input.utxo;
    }

    partial_sigs.insert(input.partial_sigs.begin(), input.partial_sigs.end());
    hd_keypaths.insert(input.hd_keypaths.begin(), input.hd_keypaths.end());
    unknown.insert(input.unknown.begin(), input.unknown.end());

    if (redeem_script.empty() && !input.redeem_script.empty()) {
        redeem_script = input.redeem_script;
    }
    if (final_script_sig.empty() && !input.final_script_sig.empty()) {
        final_script_sig = input.final_script_sig;
    }
}

bool PSBTInput::IsSane() const {
    return true;
}

void PSBTOutput::FillSignatureData(SignatureData &sigdata) const {
    if (!redeem_script.empty()) {
        sigdata.redeem_script = redeem_script;
    }
    for (const auto &key_pair : hd_keypaths) {
        sigdata.misc_pubkeys.emplace(key_pair.first.GetID(), key_pair.first);
    }
}

void PSBTOutput::FromSignatureData(const SignatureData &sigdata) {
    if (redeem_script.empty() && !sigdata.redeem_script.empty()) {
        redeem_script = sigdata.redeem_script;
    }
}

bool PSBTOutput::IsNull() const {
    return redeem_script.empty() && hd_keypaths.empty() && unknown.empty();
}

void PSBTOutput::Merge(const PSBTOutput &output) {
    hd_keypaths.insert(output.hd_keypaths.begin(), output.hd_keypaths.end());
    unknown.insert(output.unknown.begin(), output.unknown.end());

    if (redeem_script.empty() && !output.redeem_script.empty()) {
        redeem_script = output.redeem_script;
    }
}
