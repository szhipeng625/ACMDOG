// crypto.h —— AES-256-CBC 加密/解密（对应 ojcore 的 AesCrypt + Base64）
// 用 Linux OpenSSL 替代 Windows CryptoAPI，密钥/IV 与客户端硬编码值完全一致。
#pragma once

#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/rand.h>

#include <string>
#include <vector>
#include <stdexcept>
#include <cstring>

namespace mw {

// 与 ojcore/mysql_dao.cpp 硬编码一致
static const unsigned char kAesKey[32] = {
    0x6f,0x6a,0x5f,0x63,0x6f,0x72,0x65,0x5f,0x32,0x30,0x32,0x36,0x6b,0x65,0x79,0x21,
    0x4f,0x4a,0x2d,0x45,0x4e,0x43,0x52,0x59,0x50,0x54,0x2d,0x4b,0x45,0x59,0x00,0x31
};
static const unsigned char kAesIv[16] = {
    0x6f,0x6a,0x2d,0x69,0x76,0x2d,0x31,0x36,0x62,0x79,0x74,0x65,0x73,0x21,0x21,0x21
};

// Base64 编码/解码（标准，带 padding）
inline std::string base64_encode(const std::string& in) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    while (i + 2 < in.size()) {
        unsigned n = (unsigned char)in[i] << 16 | (unsigned char)in[i+1] << 8 | (unsigned char)in[i+2];
        out += tbl[(n >> 18) & 63]; out += tbl[(n >> 12) & 63];
        out += tbl[(n >> 6) & 63];  out += tbl[n & 63];
        i += 3;
    }
    if (i + 1 == in.size()) {
        unsigned n = (unsigned char)in[i] << 16;
        out += tbl[(n >> 18) & 63]; out += tbl[(n >> 12) & 63]; out += "==";
    } else if (i + 2 == in.size()) {
        unsigned n = (unsigned char)in[i] << 16 | (unsigned char)in[i+1] << 8;
        out += tbl[(n >> 18) & 63]; out += tbl[(n >> 12) & 63]; out += tbl[(n >> 6) & 63]; out += "=";
    }
    return out;
}

inline std::string base64_decode(const std::string& in) {
    static int rev[256];
    static bool init = false;
    if (!init) {
        memset(rev, -1, sizeof(rev));
        for (int i = 0; i < 64; ++i) rev[(unsigned char)"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"[i]] = i;
        init = true;
    }
    std::string out;
    int val = 0, bits = 0;
    for (char c : in) {
        if (c == '=' || c == '\n' || c == '\r') continue;
        int v = rev[(unsigned char)c];
        if (v < 0) continue;
        val = (val << 6) | v; bits += 6;
        if (bits >= 8) { bits -= 8; out += (char)((val >> bits) & 0xff); }
    }
    return out;
}

// AES-256-CBC 加密（PKCS7 padding）
inline std::string aes_encrypt(const std::string& plain) {
    if (plain.empty()) return "";
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    std::vector<unsigned char> out(plain.size() + 16);
    int len = 0, total = 0;
    EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), nullptr, kAesKey, kAesIv);
    EVP_EncryptUpdate(ctx, out.data(), &len, (const unsigned char*)plain.data(), (int)plain.size());
    total = len;
    EVP_EncryptFinal_ex(ctx, out.data() + total, &len);
    total += len;
    EVP_CIPHER_CTX_free(ctx);
    return std::string((char*)out.data(), total);
}

// AES-256-CBC 解密
inline std::string aes_decrypt(const std::string& cipher) {
    if (cipher.empty()) return "";
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    std::vector<unsigned char> out(cipher.size() + 16);
    int len = 0, total = 0;
    EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), nullptr, kAesKey, kAesIv);
    EVP_DecryptUpdate(ctx, out.data(), &len, (const unsigned char*)cipher.data(), (int)cipher.size());
    total = len;
    if (EVP_DecryptFinal_ex(ctx, out.data() + total, &len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return "";  // 解密失败（padding 错误）
    }
    total += len;
    EVP_CIPHER_CTX_free(ctx);
    return std::string((char*)out.data(), total);
}

// 对外：与 ojcore encrypt_blob/decrypt_blob 语义一致
inline std::string encrypt_blob(const std::string& plain) {
    return base64_encode(aes_encrypt(plain));
}
inline std::string decrypt_blob(const std::string& enc) {
    if (enc.empty()) return "";
    return aes_decrypt(base64_decode(enc));
}

// HashString 与 ojcore 一致（DJB2：h = h*33 + c，初值 5381）
inline unsigned long long hash_string(const std::string& s) {
    unsigned long long h = 5381;
    for (unsigned char c : s) h = h * 33 + c;
    return h;
}

} // namespace mw
