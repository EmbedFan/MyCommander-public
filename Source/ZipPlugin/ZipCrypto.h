// ARC-004: encrypted ZIP support for the ZIP plugin.
//
//   * Read: WinZip AES (method 99, AE-1/AE-2, 128/192/256-bit) and the legacy
//     PKWARE "ZipCrypto" stream cipher (weak -- supported for old archives only).
//   * Write: WinZip AES-256 AE-2 only (ZipBuilder). ZipCrypto is never written.
//
// miniz refuses every encrypted entry, so this module reads the entry's raw
// bytes itself, decrypts them with CNG (bcrypt) and inflates with miniz's tinfl.
//
// ARC-004 / FTP-010 rule: a password lives only in a Secret (memory, zeroed on
// destruction/Clear). It is never logged, written to a file, a history or a
// config, and no function here takes a password as anything that could be
// formatted into a log line.
//
// Deliberately independent of the plugin SDK and the host (pure + CNG), so the
// standalone ZipCryptoTests harness can compile it on its own.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace zipcrypto {

// A password in memory only. UTF-8 bytes, wiped on Clear()/destruction/move.
class Secret {
public:
    Secret() = default;
    explicit Secret(const std::wstring& password);
    ~Secret();
    Secret(Secret&& other) noexcept;
    Secret& operator=(Secret&& other) noexcept;
    Secret(const Secret&) = delete;
    Secret& operator=(const Secret&) = delete;

    bool Empty() const { return utf8_.empty(); }
    void Clear();
    void Set(const std::wstring& password);
    const std::string& Utf8() const { return utf8_; }

private:
    std::string utf8_;
};

enum class Status {
    Ok,
    WrongPassword,  // password verifier / check byte / authentication mismatch
    Corrupt,        // structurally broken entry (bad header, bad sizes, bad CRC after a right password)
    Unsupported,    // an encryption/compression variant this module does not read
};

// What the central directory says about one entry (from miniz's file_stat).
struct EntryRef {
    uint64_t localHeaderOffset = 0;
    uint64_t compressedSize = 0;    // as stored: includes salt/verifier/MAC for AES, 12-byte header for ZipCrypto
    uint64_t uncompressedSize = 0;
    uint32_t crc32 = 0;
    uint16_t flags = 0;             // general-purpose bit flags
    uint16_t method = 0;            // 0, 8, or 99 (WinZip AES)
};

// Reads, decrypts and inflates one encrypted entry from the whole archive image.
// `out` is untouched unless the result is Status::Ok.
Status ExtractEncryptedEntry(const uint8_t* archive, size_t archiveSize, const EntryRef& entry, const Secret& password,
                             std::vector<uint8_t>& out);

// Writes a complete archive whose every file entry is WinZip AES-256 (AE-2)
// encrypted; directory markers are stored plain. Whole archive built in memory.
// Classic 32-bit ZIP only: Finish() fails (false) past 65,535 entries or 4 GiB.
class ZipBuilder {
public:
    ZipBuilder(const Secret& password, int compressionLevel);
    // `attributes` are the Win32 attributes stored as the entry's external attributes.
    // `modifiedUtc` (FILETIME ticks, 0 = now) becomes the entry's DOS time.
    bool Add(const std::string& nameUtf8, const uint8_t* data, size_t size, uint32_t attributes, bool isDirectoryMarker,
             uint64_t modifiedUtc = 0);
    bool Finish(std::vector<uint8_t>& archive) const;

private:
    struct Central {
        std::string name;
        uint16_t flags, method, dosTime, dosDate, versionNeeded;
        uint32_t crc, compressedSize, uncompressedSize, localOffset, attributes;
        std::vector<uint8_t> extra;
    };
    const Secret& password_;
    int level_;
    std::vector<uint8_t> body_;
    std::vector<Central> central_;
    bool failed_ = false;
};

// Exposed for the known-answer tests.
bool Pbkdf2HmacSha1(const std::string& password, const uint8_t* salt, size_t saltSize, uint32_t iterations, uint8_t* out,
                    size_t outSize);
bool HmacSha1(const uint8_t* key, size_t keySize, const uint8_t* data, size_t dataSize, uint8_t out[20]);
// AES-CTR in WinZip's flavour: little-endian counter starting at 1. Encrypt == decrypt.
bool AesCtrWinZip(const uint8_t* key, size_t keySize, uint8_t* data, size_t size);

}  // namespace zipcrypto
