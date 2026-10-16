// ARC-004: standalone harness for ZipCrypto.{h,cpp} -- known-answer vectors for
// the primitives, archives written by an independent tool (bsdtar/libarchive:
// AES-128/256, AES stored, ZipCrypto), a ZipBuilder round trip, tamper and
// wrong-password paths, and (when Windows' tar.exe is present) interop: the
// archive ZipBuilder writes must open in libarchive with the same password.
//
// Build with MSVC from a Developer Command Prompt, repo root:
//   cl /nologo /std:c++20 /EHsc /MTd /utf-8 /FoBuild\ /FeBuild\ZipCryptoTests.exe
//      Source\ZipPlugin\ZipCryptoTests.cpp Source\ZipPlugin\ZipCrypto.cpp
//      Source\ZipPlugin\third_party\miniz\miniz.c
// Run: Build\ZipCryptoTests.exe [Source\ZipPlugin\testdata]   (exit 0 = all passed)

#include "ZipCrypto.h"

#include "third_party/miniz/miniz.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace {

int failures = 0, checks = 0;
void Check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", what);
    }
}

using Bytes = std::vector<uint8_t>;

Bytes LoadFile(const std::wstring& path) {
    Bytes out;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return out;
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD got = 0;
    if (!out.empty()) ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr);
    CloseHandle(h);
    return out;
}

bool WriteFileBytes(const std::wstring& path, const Bytes& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = data.empty() || WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &put, nullptr);
    CloseHandle(h);
    return ok;
}

Bytes Hex(const char* hex) {
    Bytes out;
    for (const char* p = hex; p[0] && p[1]; p += 2) out.push_back(static_cast<uint8_t>(std::strtoul(std::string(p, 2).c_str(), nullptr, 16)));
    return out;
}

Bytes TextBytes(const std::string& s) { return Bytes(s.begin(), s.end()); }

Bytes HelloContent() {
    std::string s;
    for (int i = 0; i < 50; ++i) s += "ARC-004 secret line\n";
    return TextBytes(s);
}
Bytes DataContent() {
    Bytes b(3000);
    for (size_t i = 0; i < b.size(); ++i) b[i] = static_cast<uint8_t>((i * 31 + 7) & 0xFF);
    return b;
}

// Reads every file entry of `archive` with `password` into name -> content,
// recording each entry's Status. Entries miniz can read itself (not encrypted,
// e.g. libarchive stores empty files plain) go through miniz.
struct Opened {
    std::map<std::string, Bytes> content;
    std::map<std::string, zipcrypto::Status> status;
    std::map<std::string, bool> encrypted;
    std::map<std::string, uint32_t> attrs;
    size_t directories = 0;
    bool valid = false;
};

Opened OpenAll(const Bytes& archive, const zipcrypto::Secret& password) {
    Opened r;
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, archive.data(), archive.size(), 0)) return r;
    r.valid = true;
    for (mz_uint i = 0, n = mz_zip_reader_get_num_files(&zip); i < n; ++i) {
        mz_zip_archive_file_stat st{};
        if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
        std::string name = st.m_filename;
        if (st.m_is_directory) {
            ++r.directories;
            continue;
        }
        r.encrypted[name] = st.m_is_encrypted != 0;
        r.attrs[name] = st.m_external_attr;
        if (!st.m_is_encrypted) {
            size_t sz = 0;
            void* p = mz_zip_reader_extract_to_heap(&zip, i, &sz, 0);
            r.status[name] = p ? zipcrypto::Status::Ok : zipcrypto::Status::Corrupt;
            if (p) {
                r.content[name] = Bytes(static_cast<uint8_t*>(p), static_cast<uint8_t*>(p) + sz);
                mz_free(p);
            }
            continue;
        }
        zipcrypto::EntryRef ref;
        ref.localHeaderOffset = st.m_local_header_ofs;
        ref.compressedSize = st.m_comp_size;
        ref.uncompressedSize = st.m_uncomp_size;
        ref.crc32 = st.m_crc32;
        ref.flags = st.m_bit_flag;
        ref.method = st.m_method;
        Bytes out;
        zipcrypto::Status s = zipcrypto::ExtractEncryptedEntry(archive.data(), archive.size(), ref, password, out);
        r.status[name] = s;
        if (s == zipcrypto::Status::Ok) r.content[name] = std::move(out);
    }
    mz_zip_reader_end(&zip);
    return r;
}

void TestPrimitives() {
    // RFC 6070 PBKDF2-HMAC-SHA1
    uint8_t dk[20];
    const uint8_t salt[] = {'s', 'a', 'l', 't'};
    Check(zipcrypto::Pbkdf2HmacSha1("password", salt, 4, 1, dk, 20) && Bytes(dk, dk + 20) == Hex("0c60c80f961f0e71f3a9b524af6012062fe037a6"),
          "PBKDF2-HMAC-SHA1 RFC 6070 c=1");
    Check(zipcrypto::Pbkdf2HmacSha1("password", salt, 4, 2, dk, 20) && Bytes(dk, dk + 20) == Hex("ea6c014dc72d6f8ccd1ed92ace1d41f0d8de8957"),
          "PBKDF2-HMAC-SHA1 RFC 6070 c=2");
    Check(zipcrypto::Pbkdf2HmacSha1("password", salt, 4, 4096, dk, 20) && Bytes(dk, dk + 20) == Hex("4b007901b765489abead49d926f721d065a429c1"),
          "PBKDF2-HMAC-SHA1 RFC 6070 c=4096");
    Check(!zipcrypto::Pbkdf2HmacSha1("", salt, 4, 1, dk, 20), "PBKDF2 refuses an empty password");

    // RFC 2202 test case 1
    uint8_t mac[20];
    Bytes key(20, 0x0b);
    const uint8_t hi[] = {'H', 'i', ' ', 'T', 'h', 'e', 'r', 'e'};
    Check(zipcrypto::HmacSha1(key.data(), key.size(), hi, sizeof(hi), mac) && Bytes(mac, mac + 20) == Hex("b617318655057264e28bc0b6fb378c8ef146be00"),
          "HMAC-SHA1 RFC 2202 case 1");

    // AES-CTR: symmetric, keystream differs per block, no repetition across the 64 KB chunk boundary.
    Bytes k(32, 0x42), data(200000, 0), copy;
    Check(zipcrypto::AesCtrWinZip(k.data(), k.size(), data.data(), data.size()), "AES-CTR encrypts");
    Check(data[0] != 0 || data[1] != 0, "AES-CTR output is not the plaintext");
    Check(Bytes(data.begin(), data.begin() + 16) != Bytes(data.begin() + 16, data.begin() + 32), "AES-CTR blocks differ");
    Check(Bytes(data.begin(), data.begin() + 16) != Bytes(data.begin() + 65536, data.begin() + 65552), "AES-CTR continues past a chunk");
    copy = data;
    zipcrypto::AesCtrWinZip(k.data(), k.size(), copy.data(), copy.size());
    Check(copy == Bytes(200000, 0), "AES-CTR decrypts back");
    Check(!zipcrypto::AesCtrWinZip(k.data(), 7, copy.data(), copy.size()), "AES-CTR rejects a bad key size");
}

void TestSecret() {
    zipcrypto::Secret a(L"S3cretPw!");
    const char* p = a.Utf8().data();
    zipcrypto::Secret b(std::move(a));
    // cppcheck-suppress accessMoved ; the moved-from state is exactly what this checks
    Check(a.Empty() && !b.Empty() && b.Utf8() == "S3cretPw!", "Secret move transfers the value");
    bool zero = true;
    for (int i = 0; i < 9; ++i) zero &= p[i] == 0;
    Check(zero, "a moved-from Secret's bytes are wiped");

    zipcrypto::Secret c(L"a-much-longer-password-that-does-not-fit-the-small-string-buffer");
    p = c.Utf8().data();
    c.Clear();
    zero = true;
    for (int i = 0; i < 40; ++i) zero &= p[i] == 0;
    Check(c.Empty() && zero, "Clear wipes a heap-sized password");

    zipcrypto::Secret u(L"pässwörd-日本");
    Check(u.Utf8() == "p\xC3\xA4ssw\xC3\xB6rd-\xE6\x97\xA5\xE6\x9C\xAC", "Secret stores UTF-8");
}

void TestFixtures(const std::wstring& dir) {
    const zipcrypto::Secret good(L"S3cretPw!"), bad(L"Wr0ngPw!"), empty;
    struct Fixture { const wchar_t* file; };
    for (const Fixture f : {Fixture{L"aes256.zip"}, Fixture{L"aes128.zip"}, Fixture{L"zipcrypt.zip"},
                            Fixture{L"aes256-store.zip"}}) {
        std::string label;
        for (const wchar_t* c = f.file; *c; ++c) label += static_cast<char>(*c);
        Bytes archive = LoadFile(dir + L"\\" + f.file);
        if (archive.empty()) {
            Check(false, ("fixture present: " + label).c_str());
            continue;
        }
        Opened ok = OpenAll(archive, good);
        Check(ok.valid, ("opens: " + label).c_str());
        Check(ok.content["hello.txt"] == HelloContent(), ("hello.txt decrypts: " + label).c_str());
        Check(ok.content["sub/data.bin"] == DataContent(), ("data.bin decrypts: " + label).c_str());
        Check(ok.status["hello.txt"] == zipcrypto::Status::Ok && ok.encrypted["hello.txt"], ("hello.txt is an encrypted entry: " + label).c_str());

        Opened wrong = OpenAll(archive, bad);
        Check(wrong.status["hello.txt"] == zipcrypto::Status::WrongPassword, ("wrong password is reported: " + label).c_str());
        Check(wrong.status["sub/data.bin"] == zipcrypto::Status::WrongPassword, ("wrong password, binary entry: " + label).c_str());
        Check(wrong.content.count("hello.txt") == 0, ("no output for a wrong password: " + label).c_str());

        Opened none = OpenAll(archive, empty);
        Check(none.status["hello.txt"] == zipcrypto::Status::WrongPassword, ("empty password is reported as wrong: " + label).c_str());
    }
}

void TestRoundTrip() {
    const zipcrypto::Secret pw(L"pässwörd-日本-\U0001F642");
    Bytes big(1500000);
    for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<uint8_t>((i % 4099 < 3000) ? (i * 7) : (i >> 3));
    Bytes hello = HelloContent(), data = DataContent(), zeros(70000, 0);

    zipcrypto::ZipBuilder builder(pw, 6);
    Check(builder.Add("hello.txt", hello.data(), hello.size(), FILE_ATTRIBUTE_ARCHIVE, false), "builder adds text");
    Check(builder.Add("sub/data.bin", data.data(), data.size(), FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_ARCHIVE, false), "builder adds binary");
    Check(builder.Add("empty.txt", nullptr, 0, FILE_ATTRIBUTE_ARCHIVE, false), "builder adds an empty file");
    Check(builder.Add("\xC3\xBC-\xE6\x97\xA5\xE6\x9C\xAC.txt", hello.data(), hello.size(), FILE_ATTRIBUTE_HIDDEN, false), "builder adds a Unicode name");
    Check(builder.Add("emptydir/", nullptr, 0, FILE_ATTRIBUTE_DIRECTORY, true), "builder adds a directory marker");
    Check(builder.Add("big.bin", big.data(), big.size(), FILE_ATTRIBUTE_ARCHIVE, false), "builder adds a 1.5 MB file");
    Check(builder.Add("zeros.bin", zeros.data(), zeros.size(), FILE_ATTRIBUTE_ARCHIVE, false), "builder adds a compressible file");
    Bytes archive;
    Check(builder.Finish(archive), "builder finishes");

    // ARC-004 / confidentiality: neither the plaintext nor the password is visible in the archive.
    std::string image(archive.begin(), archive.end());
    Check(image.find("ARC-004 secret line") == std::string::npos, "plaintext does not appear in the archive");
    Check(image.find("p\xC3\xA4ssw") == std::string::npos, "the password does not appear in the archive");

    Opened r = OpenAll(archive, pw);
    Check(r.valid, "miniz can open the encrypted archive");
    Check(r.content["hello.txt"] == hello, "round trip: text");
    Check(r.content["sub/data.bin"] == data, "round trip: binary");
    Check(r.content["big.bin"] == big, "round trip: 1.5 MB across CTR chunks");
    Check(r.content["zeros.bin"] == zeros, "round trip: compressed entry");
    Check(r.content.count("empty.txt") == 1 && r.content["empty.txt"].empty() && r.status["empty.txt"] == zipcrypto::Status::Ok,
          "round trip: empty file");
    Check(r.content["\xC3\xBC-\xE6\x97\xA5\xE6\x9C\xAC.txt"] == hello, "round trip: Unicode name");
    Check(r.directories == 1, "directory marker survives");
    Check(r.attrs["sub/data.bin"] == (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_ARCHIVE), "attributes are stored");
    Check(r.encrypted["hello.txt"] && r.encrypted["big.bin"], "file entries are encrypted");

    Opened wrong = OpenAll(archive, zipcrypto::Secret(L"not the password"));
    Check(wrong.status["hello.txt"] == zipcrypto::Status::WrongPassword, "round trip: wrong password");
    Check(wrong.content.empty() || wrong.content.count("hello.txt") == 0, "round trip: nothing leaks for a wrong password");

    // Tamper: flip one ciphertext byte of hello.txt -> authentication failure, no output.
    {
        mz_zip_archive zip{};
        mz_zip_reader_init_mem(&zip, archive.data(), archive.size(), 0);
        mz_zip_archive_file_stat st{};
        mz_zip_reader_file_stat(&zip, 0, &st);
        mz_zip_reader_end(&zip);
        Bytes damaged = archive;
        size_t nameLen = damaged[st.m_local_header_ofs + 26] | (damaged[st.m_local_header_ofs + 27] << 8);
        size_t extraLen = damaged[st.m_local_header_ofs + 28] | (damaged[st.m_local_header_ofs + 29] << 8);
        size_t dataStart = static_cast<size_t>(st.m_local_header_ofs) + 30 + nameLen + extraLen;
        damaged[dataStart + 16 + 2 + 5] ^= 0x01;
        Opened t = OpenAll(damaged, pw);
        Check(t.status[st.m_filename] == zipcrypto::Status::Corrupt && t.content.count(st.m_filename) == 0,
              "a flipped ciphertext byte fails authentication");
    }
}

// libarchive must open what ZipBuilder wrote, with the same password.
void TestInteropWithTar() {
    wchar_t sysDir[MAX_PATH];
    GetSystemDirectoryW(sysDir, MAX_PATH);
    std::wstring tar = std::wstring(sysDir) + L"\\tar.exe";
    if (GetFileAttributesW(tar.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::printf("interop: tar.exe not found, skipped\n");
        return;
    }
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring work = std::wstring(tmp) + L"mc_zipcrypto_interop_" + std::to_wstring(GetTickCount64());
    CreateDirectoryW(work.c_str(), nullptr);
    std::wstring zipPath = work + L"\\built.zip", outDir = work + L"\\out";
    CreateDirectoryW(outDir.c_str(), nullptr);

    zipcrypto::Secret pw(L"S3cretPw!");
    Bytes hello = HelloContent(), data = DataContent();
    zipcrypto::ZipBuilder builder(pw, 6);
    builder.Add("hello.txt", hello.data(), hello.size(), FILE_ATTRIBUTE_ARCHIVE, false);
    builder.Add("sub/data.bin", data.data(), data.size(), FILE_ATTRIBUTE_ARCHIVE, false);
    Bytes archive;
    Check(builder.Finish(archive) && WriteFileBytes(zipPath, archive), "interop: archive written");

    std::wstring cmd = L"\"\"" + tar + L"\" -xf \"" + zipPath + L"\" --passphrase \"S3cretPw!\" -C \"" + outDir + L"\" >nul 2>nul\"";
    int rc = _wsystem(cmd.c_str());
    Check(rc == 0, "interop: libarchive extracts the archive with the password");
    Check(LoadFile(outDir + L"\\hello.txt") == hello, "interop: hello.txt identical");
    Check(LoadFile(outDir + L"\\sub\\data.bin") == data, "interop: data.bin identical");

    std::wstring cleanup = L"rmdir /s /q \"" + work + L"\" >nul 2>nul";
    _wsystem(cleanup.c_str());
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring dir = argc > 1 ? argv[1] : L"Source\\ZipPlugin\\testdata";
    TestPrimitives();
    TestSecret();
    TestFixtures(dir);
    TestRoundTrip();
    TestInteropWithTar();
    std::printf("%d check(s), %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
