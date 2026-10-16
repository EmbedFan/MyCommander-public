// ARC-004: encrypted ZIP support. See ZipCrypto.h.

#include "ZipCrypto.h"

#include "third_party/miniz/miniz.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstring>

#pragma comment(lib, "bcrypt.lib")

namespace zipcrypto {

// ---------------------------------------------------------------------------
// Secret
// ---------------------------------------------------------------------------

Secret::Secret(const std::wstring& password) { Set(password); }
Secret::~Secret() { Clear(); }

Secret::Secret(Secret&& other) noexcept : utf8_(other.utf8_) {
    // utf8_ was copied (not moved) above, then the source is wiped: a moved short std::string would leave its bytes behind
    other.Clear();
}

Secret& Secret::operator=(Secret&& other) noexcept {
    if (this != &other) {
        Clear();
        utf8_ = other.utf8_;
        other.Clear();
    }
    return *this;
}

void Secret::Clear() {
    if (!utf8_.empty()) SecureZeroMemory(&utf8_[0], utf8_.size());
    utf8_.clear();
}

void Secret::Set(const std::wstring& password) {
    Clear();
    if (password.empty()) return;
    int n = WideCharToMultiByte(CP_UTF8, 0, password.c_str(), static_cast<int>(password.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return;
    utf8_.assign(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, password.c_str(), static_cast<int>(password.size()), &utf8_[0], n, nullptr, nullptr);
}

// ---------------------------------------------------------------------------
// Little-endian helpers
// ---------------------------------------------------------------------------

namespace {

uint16_t Le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t Le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
void Put16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
}
void Put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}

constexpr uint32_t kLocalSig = 0x04034b50;
constexpr uint32_t kCentralSig = 0x02014b50;
constexpr uint32_t kEndSig = 0x06054b50;
constexpr uint16_t kAesMethod = 99;
constexpr uint16_t kAesExtraId = 0x9901;
constexpr uint16_t kFlagEncrypted = 0x0001;
constexpr uint16_t kFlagDataDescriptor = 0x0008;
constexpr uint16_t kFlagStrongEncryption = 0x0040;
constexpr uint16_t kFlagUtf8Names = 0x0800;
constexpr uint32_t kAesIterations = 1000;
constexpr size_t kAuthCodeSize = 10;
constexpr uint64_t kMaxEntrySize = 0x7FFFFFFF;  // entries are held in memory

struct Alg {
    BCRYPT_ALG_HANDLE h = nullptr;
    Alg() = default;
    Alg(const Alg&) = delete;
    Alg& operator=(const Alg&) = delete;
    ~Alg() {
        if (h) BCryptCloseAlgorithmProvider(h, 0);
    }
    bool Open(LPCWSTR id, ULONG flags = 0) { return BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&h, id, nullptr, flags)); }
};

bool Inflate(const uint8_t* src, size_t srcSize, size_t outSize, std::vector<uint8_t>& out) {
    out.assign(outSize ? outSize : 1, 0);
    uint8_t dummy = 0;
    size_t got = tinfl_decompress_mem_to_mem(out.data(), outSize, srcSize ? src : &dummy, srcSize, 0);
    if (got == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED || got != outSize) return false;
    out.resize(outSize);
    return true;
}

uint32_t Crc(const std::vector<uint8_t>& data) {
    return data.empty() ? 0 : static_cast<uint32_t>(mz_crc32(MZ_CRC32_INIT, data.data(), data.size()));
}

// ---- legacy PKWARE ZipCrypto ------------------------------------------------

const uint32_t* CrcTable() {
    static uint32_t table[256];
    static bool ready = [] {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        return true;
    }();
    (void)ready;
    return table;
}

class ZipCryptoKeys {
public:
    explicit ZipCryptoKeys(const std::string& password) {
        for (unsigned char c : password) Update(c);
    }
    uint8_t DecryptByte(uint8_t cipher) {
        uint32_t t = (k2_ | 2) & 0xFFFF;
        uint8_t plain = static_cast<uint8_t>(cipher ^ static_cast<uint8_t>((t * (t ^ 1)) >> 8));
        Update(plain);
        return plain;
    }

private:
    void Update(uint8_t c) {
        const uint32_t* t = CrcTable();
        k0_ = t[(k0_ ^ c) & 0xFF] ^ (k0_ >> 8);
        k1_ = (k1_ + (k0_ & 0xFF)) * 134775813u + 1;
        k2_ = t[(k2_ ^ (k1_ >> 24)) & 0xFF] ^ (k2_ >> 8);
    }
    uint32_t k0_ = 0x12345678, k1_ = 0x23456789, k2_ = 0x34567890;
};

// The password encodings a ZipCrypto archive may have been made with: other
// tools use the ANSI or OEM code page for non-ASCII passwords, not UTF-8.
std::vector<std::string> ZipCryptoPasswordCandidates(const std::string& utf8) {
    std::vector<std::string> out{utf8};
    bool ascii = std::all_of(utf8.begin(), utf8.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; });
    if (ascii) return out;
    int wn = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (wn <= 0) return out;
    std::wstring wide(static_cast<size_t>(wn), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), &wide[0], wn);
    for (UINT cp : {static_cast<UINT>(CP_ACP), static_cast<UINT>(CP_OEMCP)}) {
        int n = WideCharToMultiByte(cp, 0, wide.data(), wn, nullptr, 0, nullptr, nullptr);
        if (n <= 0) continue;
        std::string s(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(cp, 0, wide.data(), wn, &s[0], n, nullptr, nullptr);
        if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(std::move(s));
    }
    return out;
}

Status ReadZipCrypto(const uint8_t* data, size_t size, const EntryRef& entry, uint16_t dosTime, const Secret& password,
                     std::vector<uint8_t>& out) {
    if (size < 12) return Status::Corrupt;
    if (entry.method != 0 && entry.method != 8) return Status::Unsupported;
    for (const std::string& candidate : ZipCryptoPasswordCandidates(password.Utf8())) {
        ZipCryptoKeys keys(candidate);
        uint8_t last = 0;
        for (int i = 0; i < 12; ++i) last = keys.DecryptByte(data[i]);
        uint8_t byCrc = static_cast<uint8_t>(entry.crc32 >> 24);
        bool match = last == byCrc;
        if ((entry.flags & kFlagDataDescriptor) && last == static_cast<uint8_t>(dosTime >> 8)) match = true;
        if (!match) continue;  // 255 of 256 wrong passwords stop here

        std::vector<uint8_t> plain(size - 12);
        for (size_t i = 0; i < plain.size(); ++i) plain[i] = keys.DecryptByte(data[12 + i]);
        std::vector<uint8_t> result;
        if (entry.method == 0) {
            if (plain.size() != entry.uncompressedSize) continue;
            result = std::move(plain);
        } else if (!Inflate(plain.data(), plain.size(), static_cast<size_t>(entry.uncompressedSize), result)) {
            continue;
        }
        if (Crc(result) != entry.crc32) continue;  // a lucky check byte, still the wrong password
        out = std::move(result);
        return Status::Ok;
    }
    return Status::WrongPassword;
}

// ---- WinZip AES -------------------------------------------------------------

struct AesParams {
    size_t keySize = 0;
    size_t saltSize = 0;
    int version = 0;
    uint16_t realMethod = 0;
};

bool ParseAesExtra(const uint8_t* extra, size_t size, AesParams& out) {
    size_t pos = 0;
    while (pos + 4 <= size) {
        uint16_t id = Le16(extra + pos), len = Le16(extra + pos + 2);
        pos += 4;
        if (pos + len > size) return false;
        if (id == kAesExtraId) {
            if (len < 7 || extra[pos + 2] != 'A' || extra[pos + 3] != 'E') return false;
            out.version = Le16(extra + pos);
            switch (extra[pos + 4]) {
                case 1: out.keySize = 16; out.saltSize = 8; break;
                case 2: out.keySize = 24; out.saltSize = 12; break;
                case 3: out.keySize = 32; out.saltSize = 16; break;
                default: return false;
            }
            out.realMethod = Le16(extra + pos + 5);
            return true;
        }
        pos += len;
    }
    return false;
}

bool ConstantTimeEqual(const uint8_t* a, const uint8_t* b, size_t n) {
    uint8_t diff = 0;
    for (size_t i = 0; i < n; ++i) diff |= static_cast<uint8_t>(a[i] ^ b[i]);
    return diff == 0;
}

Status ReadAes(const uint8_t* data, size_t size, const EntryRef& entry, const AesParams& p, const Secret& password,
               std::vector<uint8_t>& out) {
    if (p.version != 1 && p.version != 2) return Status::Unsupported;
    if (p.realMethod != 0 && p.realMethod != 8) return Status::Unsupported;
    if (size < p.saltSize + 2 + kAuthCodeSize) return Status::Corrupt;

    const uint8_t* salt = data;
    const uint8_t* verifier = data + p.saltSize;
    const uint8_t* cipher = verifier + 2;
    size_t cipherSize = size - p.saltSize - 2 - kAuthCodeSize;
    const uint8_t* mac = cipher + cipherSize;

    std::vector<uint8_t> derived(2 * p.keySize + 2);
    if (!Pbkdf2HmacSha1(password.Utf8(), salt, p.saltSize, kAesIterations, derived.data(), derived.size()))
        return Status::Unsupported;
    if (!ConstantTimeEqual(derived.data() + 2 * p.keySize, verifier, 2)) return Status::WrongPassword;

    uint8_t expected[20];
    if (!HmacSha1(derived.data() + p.keySize, p.keySize, cipher, cipherSize, expected)) return Status::Unsupported;
    if (!ConstantTimeEqual(expected, mac, kAuthCodeSize)) return Status::Corrupt;  // verifier matched: damaged data

    std::vector<uint8_t> plain(cipher, cipher + cipherSize);
    if (!plain.empty() && !AesCtrWinZip(derived.data(), p.keySize, plain.data(), plain.size())) return Status::Unsupported;
    SecureZeroMemory(derived.data(), derived.size());

    std::vector<uint8_t> result;
    if (p.realMethod == 0) {
        if (plain.size() != entry.uncompressedSize) return Status::Corrupt;
        result = std::move(plain);
    } else if (!Inflate(plain.data(), plain.size(), static_cast<size_t>(entry.uncompressedSize), result)) {
        return Status::Corrupt;
    }
    if (p.version == 1 && Crc(result) != entry.crc32) return Status::Corrupt;  // AE-2 carries no CRC
    out = std::move(result);
    return Status::Ok;
}

// DOS times are local time; `modifiedUtc` 0 means "now".
void DosDateTime(uint16_t& date, uint16_t& time, uint64_t modifiedUtc) {
    date = 0;
    time = 0;
    FILETIME ft{};
    if (modifiedUtc != 0) {
        FILETIME utc{static_cast<DWORD>(modifiedUtc), static_cast<DWORD>(modifiedUtc >> 32)};
        if (!FileTimeToLocalFileTime(&utc, &ft)) return;
    } else {
        SYSTEMTIME st;
        GetLocalTime(&st);
        if (!SystemTimeToFileTime(&st, &ft)) return;
    }
    FileTimeToDosDateTime(&ft, &date, &time);
}

}  // namespace

// ---------------------------------------------------------------------------
// CNG primitives
// ---------------------------------------------------------------------------

bool Pbkdf2HmacSha1(const std::string& password, const uint8_t* salt, size_t saltSize, uint32_t iterations, uint8_t* out,
                    size_t outSize) {
    if (password.empty()) return false;
    Alg alg;
    if (!alg.Open(BCRYPT_SHA1_ALGORITHM, BCRYPT_ALG_HANDLE_HMAC_FLAG)) return false;
    return BCRYPT_SUCCESS(BCryptDeriveKeyPBKDF2(alg.h, reinterpret_cast<PUCHAR>(const_cast<char*>(password.data())),
                                                static_cast<ULONG>(password.size()), const_cast<PUCHAR>(salt),
                                                static_cast<ULONG>(saltSize), iterations, out, static_cast<ULONG>(outSize), 0));
}

bool HmacSha1(const uint8_t* key, size_t keySize, const uint8_t* data, size_t dataSize, uint8_t out[20]) {
    Alg alg;
    if (!alg.Open(BCRYPT_SHA1_ALGORITHM, BCRYPT_ALG_HANDLE_HMAC_FLAG)) return false;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (!BCRYPT_SUCCESS(BCryptCreateHash(alg.h, &hash, nullptr, 0, const_cast<PUCHAR>(key), static_cast<ULONG>(keySize), 0)))
        return false;
    bool ok = true;
    if (dataSize) ok = BCRYPT_SUCCESS(BCryptHashData(hash, const_cast<PUCHAR>(data), static_cast<ULONG>(dataSize), 0));
    if (ok) ok = BCRYPT_SUCCESS(BCryptFinishHash(hash, out, 20, 0));
    BCryptDestroyHash(hash);
    return ok;
}

bool AesCtrWinZip(const uint8_t* key, size_t keySize, uint8_t* data, size_t size) {
    if (keySize != 16 && keySize != 24 && keySize != 32) return false;
    Alg alg;
    if (!alg.Open(BCRYPT_AES_ALGORITHM)) return false;
    if (!BCRYPT_SUCCESS(BCryptSetProperty(alg.h, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_ECB)),
                                          sizeof(BCRYPT_CHAIN_MODE_ECB), 0)))
        return false;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    if (!BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(alg.h, &hKey, nullptr, 0, const_cast<PUCHAR>(key), static_cast<ULONG>(keySize), 0)))
        return false;

    constexpr size_t kChunkBlocks = 4096;  // 64 KB of keystream per CNG call
    std::vector<uint8_t> counters(kChunkBlocks * 16), stream(kChunkBlocks * 16);
    uint64_t counter = 1;  // WinZip: little-endian, starts at 1
    bool ok = true;
    for (size_t pos = 0; pos < size && ok;) {
        size_t n = std::min(size - pos, kChunkBlocks * 16);
        size_t blocks = (n + 15) / 16;
        for (size_t b = 0; b < blocks; ++b) {
            uint8_t* block = counters.data() + b * 16;
            std::memset(block, 0, 16);
            for (int i = 0; i < 8; ++i) block[i] = static_cast<uint8_t>(counter >> (8 * i));
            ++counter;
        }
        ULONG produced = 0;
        ok = BCRYPT_SUCCESS(BCryptEncrypt(hKey, counters.data(), static_cast<ULONG>(blocks * 16), nullptr, nullptr, 0,
                                          stream.data(), static_cast<ULONG>(blocks * 16), &produced, 0));
        for (size_t i = 0; ok && i < n; ++i) data[pos + i] ^= stream[i];
        pos += n;
    }
    SecureZeroMemory(stream.data(), stream.size());
    BCryptDestroyKey(hKey);
    return ok;
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

Status ExtractEncryptedEntry(const uint8_t* archive, size_t archiveSize, const EntryRef& entry, const Secret& password,
                             std::vector<uint8_t>& out) {
    if (password.Empty()) return Status::WrongPassword;
    if (entry.flags & kFlagStrongEncryption) return Status::Unsupported;  // PKWARE strong encryption
    if (entry.uncompressedSize > kMaxEntrySize || entry.compressedSize > kMaxEntrySize) return Status::Unsupported;

    uint64_t ofs = entry.localHeaderOffset;
    if (ofs + 30 > archiveSize || Le32(archive + ofs) != kLocalSig) return Status::Corrupt;
    const uint8_t* hdr = archive + ofs;
    uint16_t dosTime = Le16(hdr + 10);
    uint16_t nameLen = Le16(hdr + 26), extraLen = Le16(hdr + 28);
    uint64_t dataStart = ofs + 30 + nameLen + extraLen;
    if (dataStart + entry.compressedSize > archiveSize) return Status::Corrupt;
    const uint8_t* extra = hdr + 30 + nameLen;
    const uint8_t* data = archive + dataStart;
    size_t size = static_cast<size_t>(entry.compressedSize);

    if (entry.method == kAesMethod) {
        AesParams params;
        if (!ParseAesExtra(extra, extraLen, params)) return Status::Unsupported;
        return ReadAes(data, size, entry, params, password, out);
    }
    return ReadZipCrypto(data, size, entry, dosTime, password, out);
}

// ---------------------------------------------------------------------------
// Writing (AES-256, AE-2)
// ---------------------------------------------------------------------------

ZipBuilder::ZipBuilder(const Secret& password, int compressionLevel)
    : password_(password), level_(std::clamp(compressionLevel, 0, 10)) {}

bool ZipBuilder::Add(const std::string& nameUtf8, const uint8_t* data, size_t size, uint32_t attributes, bool isDirectoryMarker,
                     uint64_t modifiedUtc) {
    if (failed_) return false;
    if (central_.size() >= 0xFFFF || nameUtf8.size() > 0xFFFF || size > 0xFFFFFFFFu || body_.size() > 0xFFFFFFFFu) {
        failed_ = true;
        return false;
    }

    Central c{};
    c.name = nameUtf8;
    c.attributes = attributes;
    c.localOffset = static_cast<uint32_t>(body_.size());
    DosDateTime(c.dosDate, c.dosTime, modifiedUtc);
    bool ascii = std::all_of(nameUtf8.begin(), nameUtf8.end(), [](char ch) { return static_cast<unsigned char>(ch) < 0x80; });
    uint16_t nameFlag = ascii ? 0 : kFlagUtf8Names;

    std::vector<uint8_t> record;  // what goes after the local header + name + extra
    if (isDirectoryMarker) {
        c.flags = nameFlag;
        c.method = 0;
        c.versionNeeded = 20;
    } else {
        // Deflate first (raw), store when that doesn't help.
        uint16_t realMethod = 0;
        std::vector<uint8_t> payload(data, data + size);
        if (level_ > 0 && size > 0) {
            int flags = static_cast<int>(tdefl_create_comp_flags_from_zip_params(level_, -MZ_DEFAULT_WINDOW_BITS, MZ_DEFAULT_STRATEGY));
            size_t clen = 0;
            void* heap = tdefl_compress_mem_to_heap(data, size, &clen, flags);
            if (heap && clen < size) {
                payload.assign(static_cast<uint8_t*>(heap), static_cast<uint8_t*>(heap) + clen);
                realMethod = 8;
            }
            if (heap) mz_free(heap);
        }

        constexpr size_t kKey = 32, kSalt = 16;
        uint8_t salt[kSalt];
        if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, salt, kSalt, BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
            failed_ = true;
            return false;
        }
        std::vector<uint8_t> derived(2 * kKey + 2);
        if (!Pbkdf2HmacSha1(password_.Utf8(), salt, kSalt, kAesIterations, derived.data(), derived.size())) {
            failed_ = true;
            return false;
        }
        if (!payload.empty() && !AesCtrWinZip(derived.data(), kKey, payload.data(), payload.size())) {
            failed_ = true;
            return false;
        }
        uint8_t mac[20];
        if (!HmacSha1(derived.data() + kKey, kKey, payload.data(), payload.size(), mac)) {
            failed_ = true;
            return false;
        }
        record.insert(record.end(), salt, salt + kSalt);
        record.push_back(derived[2 * kKey]);
        record.push_back(derived[2 * kKey + 1]);
        record.insert(record.end(), payload.begin(), payload.end());
        record.insert(record.end(), mac, mac + kAuthCodeSize);
        SecureZeroMemory(derived.data(), derived.size());
        if (record.size() > 0xFFFFFFFFu) {
            failed_ = true;
            return false;
        }

        c.flags = static_cast<uint16_t>(kFlagEncrypted | nameFlag);
        c.method = kAesMethod;
        c.versionNeeded = 51;
        c.crc = 0;  // AE-2: the CRC is not stored, the MAC authenticates
        c.extra = {0x01, 0x99, 0x07, 0x00, 0x02, 0x00, 'A', 'E', 0x03, static_cast<uint8_t>(realMethod), static_cast<uint8_t>(realMethod >> 8)};
    }
    c.compressedSize = static_cast<uint32_t>(record.size());
    c.uncompressedSize = isDirectoryMarker ? 0 : static_cast<uint32_t>(size);

    Put32(body_, kLocalSig);
    Put16(body_, c.versionNeeded);
    Put16(body_, c.flags);
    Put16(body_, c.method);
    Put16(body_, c.dosTime);
    Put16(body_, c.dosDate);
    Put32(body_, c.crc);
    Put32(body_, c.compressedSize);
    Put32(body_, c.uncompressedSize);
    Put16(body_, static_cast<uint16_t>(c.name.size()));
    Put16(body_, static_cast<uint16_t>(c.extra.size()));
    body_.insert(body_.end(), c.name.begin(), c.name.end());
    body_.insert(body_.end(), c.extra.begin(), c.extra.end());
    body_.insert(body_.end(), record.begin(), record.end());

    central_.push_back(std::move(c));
    return true;
}

bool ZipBuilder::Finish(std::vector<uint8_t>& archive) const {
    if (failed_ || body_.size() > 0xFFFFFFFFu) return false;
    std::vector<uint8_t> result = body_;
    size_t centralStart = result.size();
    for (const Central& c : central_) {
        Put32(result, kCentralSig);
        Put16(result, 20);  // version made by: DOS/FAT, so the external attributes are Win32 attributes
        Put16(result, c.versionNeeded);
        Put16(result, c.flags);
        Put16(result, c.method);
        Put16(result, c.dosTime);
        Put16(result, c.dosDate);
        Put32(result, c.crc);
        Put32(result, c.compressedSize);
        Put32(result, c.uncompressedSize);
        Put16(result, static_cast<uint16_t>(c.name.size()));
        Put16(result, static_cast<uint16_t>(c.extra.size()));
        Put16(result, 0);  // comment
        Put16(result, 0);  // disk number
        Put16(result, 0);  // internal attributes
        Put32(result, c.attributes);
        Put32(result, c.localOffset);
        result.insert(result.end(), c.name.begin(), c.name.end());
        result.insert(result.end(), c.extra.begin(), c.extra.end());
    }
    size_t centralSize = result.size() - centralStart;
    if (result.size() > 0xFFFFFFFFu) return false;
    Put32(result, kEndSig);
    Put16(result, 0);
    Put16(result, 0);
    Put16(result, static_cast<uint16_t>(central_.size()));
    Put16(result, static_cast<uint16_t>(central_.size()));
    Put32(result, static_cast<uint32_t>(centralSize));
    Put32(result, static_cast<uint32_t>(centralStart));
    Put16(result, 0);
    archive = std::move(result);
    return true;
}

}  // namespace zipcrypto
