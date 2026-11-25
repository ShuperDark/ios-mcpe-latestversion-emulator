// Security.framework helpers for the guest (guest/Foundation/CF.m): P-256 keys and ECDSA
// (the Xbox sign-in signs its requests with a device key), random bytes and the server
// certificate checks of SecTrust, on top of Windows CNG / CryptoAPI.
//
// Keys travel in Apple's external representation: public 04|X|Y (65 bytes), private
// 04|X|Y|D (97 bytes), which is also what SecKeyCopyExternalRepresentation returns.
#include "hle.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>

#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

namespace {

BCRYPT_ALG_HANDLE ecdsa() {
    static BCRYPT_ALG_HANDLE a = [] {
        BCRYPT_ALG_HANDLE h = nullptr;
        BCryptOpenAlgorithmProvider(&h, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0);
        return h;
    }();
    return a;
}

BCRYPT_KEY_HANDLE import_key(const u8* key, u64 len) {
    if (!ecdsa() || !key || (len != 65 && len != 97) || key[0] != 4) return nullptr;
    bool priv = len == 97;
    std::vector<u8> blob(sizeof(BCRYPT_ECCKEY_BLOB) + (priv ? 96 : 64));
    auto* h = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
    h->dwMagic = priv ? BCRYPT_ECDSA_PRIVATE_P256_MAGIC : BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    h->cbKey = 32;
    memcpy(blob.data() + sizeof *h, key + 1, priv ? 96 : 64);
    BCRYPT_KEY_HANDLE k = nullptr;
    if (BCryptImportKeyPair(ecdsa(), nullptr, priv ? BCRYPT_ECCPRIVATE_BLOB : BCRYPT_ECCPUBLIC_BLOB, &k, blob.data(), (ULONG)blob.size(),
                            0) < 0)
        return nullptr;
    return k;
}

// r|s (32 + 32) <-> DER SEQUENCE { INTEGER r, INTEGER s } (what Apple's ECDSA X9.62 functions use)
void der_int(std::vector<u8>& out, const u8* v) {
    int i = 0;
    while (i < 31 && v[i] == 0) i++;
    bool pad = v[i] & 0x80;
    out.push_back(0x02);
    out.push_back((u8)(32 - i + pad));
    if (pad) out.push_back(0);
    out.insert(out.end(), v + i, v + 32);
}
std::vector<u8> raw_to_der(const u8* rs) {
    std::vector<u8> body;
    der_int(body, rs);
    der_int(body, rs + 32);
    std::vector<u8> out{0x30, (u8)body.size()};
    out.insert(out.end(), body.begin(), body.end());
    return out;
}
bool der_to_raw(const u8* d, u64 len, u8* rs) {
    if (len < 8 || d[0] != 0x30) return false;
    u64 p = 2;
    if (d[1] & 0x80) p = 2 + (d[1] & 0x7f);
    memset(rs, 0, 64);
    for (int k = 0; k < 2; k++) {
        if (p + 2 > len || d[p] != 0x02) return false;
        u64 n = d[p + 1];
        p += 2;
        if (p + n > len) return false;
        const u8* v = d + p;
        u64 m = n;
        while (m > 32 && *v == 0) { v++; m--; }
        if (m > 32) return false;
        memcpy(rs + k * 32 + (32 - m), v, m);
        p += n;
    }
    return true;
}

}  // namespace

// a new P-256 key pair: out = 04|X|Y|D (97 bytes). Returns 0.
HLE_FUNC(emu_ec_generate, int, (u8* out)) {
    BCRYPT_KEY_HANDLE k = nullptr;
    if (!ecdsa() || BCryptGenerateKeyPair(ecdsa(), &k, 256, 0) < 0 || BCryptFinalizeKeyPair(k, 0) < 0) return -1;
    u8 blob[sizeof(BCRYPT_ECCKEY_BLOB) + 96];
    ULONG got = 0;
    NTSTATUS st = BCryptExportKey(k, nullptr, BCRYPT_ECCPRIVATE_BLOB, blob, sizeof blob, &got, 0);
    BCryptDestroyKey(k);
    if (st < 0) return -1;
    out[0] = 4;
    memcpy(out + 1, blob + sizeof(BCRYPT_ECCKEY_BLOB), 96);
    return 0;
}

// ECDSA of a digest with a private key (97 bytes): a DER signature in out. Returns its
// length, or -1.
HLE_FUNC(emu_ec_sign, s64, (const u8* key, u64 keylen, const u8* digest, u64 dlen, u8* out, u64 cap)) {
    BCRYPT_KEY_HANDLE k = import_key(key, keylen);
    if (!k || keylen != 97) {
        if (k) BCryptDestroyKey(k);
        return -1;
    }
    u8 rs[64];
    ULONG got = 0;
    NTSTATUS st = BCryptSignHash(k, nullptr, (PUCHAR)digest, (ULONG)dlen, rs, sizeof rs, &got, 0);
    BCryptDestroyKey(k);
    if (st < 0 || got != 64) return -1;
    std::vector<u8> der = raw_to_der(rs);
    if (der.size() > cap) return -1;
    memcpy(out, der.data(), der.size());
    return (s64)der.size();
}

// 1 = the DER signature of the digest is valid for the key (65 or 97 bytes)
HLE_FUNC(emu_ec_verify, int, (const u8* key, u64 keylen, const u8* digest, u64 dlen, const u8* sig, u64 slen)) {
    u8 rs[64];
    if (!der_to_raw(sig, slen, rs)) return 0;
    BCRYPT_KEY_HANDLE k = import_key(key, keylen);
    if (!k) return 0;
    NTSTATUS st = BCryptVerifySignature(k, nullptr, (PUCHAR)digest, (ULONG)dlen, rs, 64, 0);
    BCryptDestroyKey(k);
    return st >= 0 ? 1 : 0;
}

HLE_FUNC(emu_random_bytes, int, (void* out, u64 n)) {
    return BCryptGenRandom(nullptr, (PUCHAR)out, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0 ? 0 : -1;
}

// SecTrust: is the certificate chain (blob = [u32 length][DER]..., the leaf first) a valid TLS
// server chain for host (UTF-8; empty = any name)? Windows' trusted roots and revocation checks.
// Returns 1 trusted, 0 not.
HLE_FUNC(emu_cert_verify_ssl, int, (const u8* blob, u64 len, const char* host)) {
    HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, nullptr);
    if (!store) return 0;
    PCCERT_CONTEXT leaf = nullptr;
    for (u64 p = 0; p + 4 <= len;) {
        u32 n;
        memcpy(&n, blob + p, 4);
        p += 4;
        if (p + n > len) break;
        PCCERT_CONTEXT c = nullptr;
        if (CertAddEncodedCertificateToStore(store, X509_ASN_ENCODING, blob + p, n, CERT_STORE_ADD_ALWAYS, &c)) {
            if (!leaf) leaf = c;
            else CertFreeCertificateContext(c);
        }
        p += n;
    }
    int ok = 0;
    if (leaf) {
        CERT_CHAIN_PARA para{sizeof para};
        LPSTR usage[] = {(LPSTR)szOID_PKIX_KP_SERVER_AUTH};
        para.RequestedUsage.dwType = USAGE_MATCH_TYPE_OR;
        para.RequestedUsage.Usage.cUsageIdentifier = 1;
        para.RequestedUsage.Usage.rgpszUsageIdentifier = usage;
        PCCERT_CHAIN_CONTEXT chain = nullptr;
        if (CertGetCertificateChain(nullptr, leaf, nullptr, store, &para, CERT_CHAIN_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT, nullptr, &chain)) {
            std::wstring whost = widen(host ? host : "");
            HTTPSPolicyCallbackData ssl{};
            ssl.cbStruct = sizeof ssl;
            ssl.dwAuthType = AUTHTYPE_SERVER;
            ssl.pwszServerName = whost.empty() ? nullptr : const_cast<wchar_t*>(whost.c_str());
            // an unreachable revocation server is not a reason to fail (like Apple's soft-fail checks)
            ssl.fdwChecks = 0x00000080 /* SECURITY_FLAG_IGNORE_REVOCATION */ | (whost.empty() ? 0x00001000 /* ..._CERT_CN_INVALID */ : 0);
            CERT_CHAIN_POLICY_PARA pp{sizeof pp};
            pp.pvExtraPolicyPara = &ssl;
            CERT_CHAIN_POLICY_STATUS ps{sizeof ps};
            if (CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain, &pp, &ps) && ps.dwError == 0) ok = 1;
            else LOGW("tls: certificate of %s not trusted (0x%lx)", host ? host : "?", ps.dwError);
            CertFreeCertificateChain(chain);
        }
        CertFreeCertificateContext(leaf);
    }
    CertCloseStore(store, 0);
    return ok;
}
