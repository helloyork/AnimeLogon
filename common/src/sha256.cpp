#include "animelogon/sha256.h"

#include <bcrypt.h>

#include <vector>

namespace animelogon {
namespace {

constexpr size_t kChunk = 1 << 20;
constexpr ULONG kDigestBytes = 32;

}  // namespace

Sha256::Sha256() {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
        failed_ = true;
        return;
    }
    algorithm_ = algorithm;
    // With no hash object buffer CNG allocates one itself (Windows 7 and later).
    if (!BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0))) {
        failed_ = true;
        return;
    }
    hash_ = hash;
}

Sha256::~Sha256() {
    if (hash_) BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(hash_));
    if (algorithm_) BCryptCloseAlgorithmProvider(static_cast<BCRYPT_ALG_HANDLE>(algorithm_), 0);
}

void Sha256::Update(const void *bytes, size_t size) {
    const auto *p = static_cast<const uint8_t *>(bytes);
    while (!failed_ && hash_ && size) {
        const ULONG n = (ULONG)(size > kChunk ? kChunk : size);
        if (!BCRYPT_SUCCESS(BCryptHashData(static_cast<BCRYPT_HASH_HANDLE>(hash_), const_cast<PUCHAR>(p), n, 0)))
            failed_ = true;
        p += n;
        size -= n;
    }
}

bool Sha256::UpdateFromHandle(HANDLE file) {
    std::vector<uint8_t> buffer(kChunk);
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(file, buffer.data(), (DWORD)buffer.size(), &got, nullptr)) {
            failed_ = true;
            return false;
        }
        if (!got) return !failed_;
        Update(buffer.data(), got);
    }
}

std::wstring Sha256::Finish() {
    if (failed_ || !hash_) return {};
    UCHAR digest[kDigestBytes];
    const bool ok = BCRYPT_SUCCESS(BCryptFinishHash(static_cast<BCRYPT_HASH_HANDLE>(hash_), digest, kDigestBytes, 0));
    failed_ = true;  // a finished hash takes nothing more
    if (!ok) return {};
    static const wchar_t kHex[] = L"0123456789abcdef";
    std::wstring out;
    out.reserve(kDigestBytes * 2);
    for (UCHAR b : digest) {
        out += kHex[b >> 4];
        out += kHex[b & 15];
    }
    return out;
}

std::wstring Sha256Of(const void *bytes, size_t size) {
    Sha256 hash;
    hash.Update(bytes, size);
    return hash.Finish();
}

}  // namespace animelogon
