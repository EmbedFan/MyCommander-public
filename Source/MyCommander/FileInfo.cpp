#include "FileInfo.h"

#include <wintrust.h>
#include <Softpub.h>
#include <wincrypt.h>
#include <mscat.h>

#include <algorithm>
#include <cwctype>
#include <vector>

#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "version.lib")

namespace mc {

namespace {

std::wstring ToLowerCopy(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

// Extracts the signing certificate's subject display name from a file
// WinVerifyTrust has already confirmed is signed. Best-effort only: any
// failure along the way just leaves the result empty (the caller already
// knows the file is signed regardless of whether this succeeds).
std::wstring ExtractSignerName(const std::filesystem::path& path) {
    HCERTSTORE hStore = nullptr;
    HCRYPTMSG hMsg = nullptr;
    std::wstring result;

    std::wstring widePath = path.wstring();
    BOOL ok = CryptQueryObject(CERT_QUERY_OBJECT_FILE, widePath.c_str(), CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
                               CERT_QUERY_FORMAT_FLAG_BINARY, 0, nullptr, nullptr, nullptr, &hStore, &hMsg, nullptr);
    if (ok) {
        DWORD signerInfoSize = 0;
        if (CryptMsgGetParam(hMsg, CMSG_SIGNER_INFO_PARAM, 0, nullptr, &signerInfoSize) && signerInfoSize > 0) {
            std::vector<BYTE> signerInfoBuf(signerInfoSize);
            auto* signerInfo = reinterpret_cast<CMSG_SIGNER_INFO*>(signerInfoBuf.data());
            if (CryptMsgGetParam(hMsg, CMSG_SIGNER_INFO_PARAM, 0, signerInfo, &signerInfoSize)) {
                CERT_INFO certInfo{};
                certInfo.Issuer = signerInfo->Issuer;
                certInfo.SerialNumber = signerInfo->SerialNumber;
                PCCERT_CONTEXT certContext = CertFindCertificateInStore(
                    hStore, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_SUBJECT_CERT, &certInfo, nullptr);
                if (certContext) {
                    DWORD nameLen =
                        CertGetNameStringW(certContext, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, nullptr, 0);
                    if (nameLen > 1) {
                        std::wstring name(nameLen, L'\0');
                        CertGetNameStringW(certContext, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name.data(),
                                           nameLen);
                        while (!name.empty() && name.back() == L'\0') name.pop_back();
                        result = name;
                    }
                    CertFreeCertificateContext(certContext);
                }
            }
        }
    }

    if (hMsg) CryptMsgClose(hMsg);
    if (hStore) CertCloseStore(hStore, 0);
    return result;
}

// Most Windows system binaries (notepad.exe, explorer.exe, ...) aren't
// signed with an *embedded* Authenticode signature at all -- they're
// verified against a separate system catalog (.cat) file instead, a
// well-documented WinVerifyTrust limitation: the plain WTD_CHOICE_FILE
// check above only ever sees an embedded signature, so it reports
// TRUST_E_NOSIGNATURE for a catalog-signed file even though the file is
// genuinely trusted. This hashes `path` and asks the system's own catalog
// database whether any installed catalog covers that hash -- if one does,
// the file is signed via that catalog. Returns the catalog file's own path
// (itself Authenticode-signed, reused to extract a signer name the same
// way as an embedded signature) if found, or an empty path otherwise.
std::filesystem::path FindCoveringCatalog(const std::filesystem::path& path) {
    std::filesystem::path catalogPath;
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return catalogPath;

    HCATADMIN hCatAdmin = nullptr;
    // A null subsystem GUID (rather than e.g. DRIVER_ACTION_VERIFY, despite
    // being the GUID most catalog-verification samples pass generically)
    // is what actually lets an ordinary, non-elevated user succeed here —
    // confirmed live: DRIVER_ACTION_VERIFY specifically triggers an
    // ERROR_ACCESS_DENIED (driver-signing-policy-related access checks)
    // this app must never need, matching SEC-001/002's "never require
    // elevation for ordinary operation" the same way every other feature
    // in this app already does.
    if (CryptCATAdminAcquireContext(&hCatAdmin, nullptr, 0)) {
        BYTE hash[100];
        DWORD hashSize = sizeof(hash);
        if (CryptCATAdminCalcHashFromFileHandle(hFile, &hashSize, hash, 0)) {
            HCATINFO hCatInfo = CryptCATAdminEnumCatalogFromHash(hCatAdmin, hash, hashSize, 0, nullptr);
            if (hCatInfo) {
                CATALOG_INFO catInfo{};
                catInfo.cbStruct = sizeof(catInfo);
                if (CryptCATCatalogInfoFromContext(hCatInfo, &catInfo, 0)) {
                    catalogPath = catInfo.wszCatalogFile;
                }
                CryptCATAdminReleaseCatalogContext(hCatAdmin, hCatInfo, 0);
            }
        }
        CryptCATAdminReleaseContext(hCatAdmin, 0);
    }
    CloseHandle(hFile);
    return catalogPath;
}

}  // namespace

bool IsRunnableExtension(const std::wstring& extension) {
    std::wstring ext = ToLowerCopy(extension);
    if (!ext.empty() && ext.front() != L'.') ext = L"." + ext;
    static const std::vector<std::wstring> kRunnableExtensions = {
        L".exe", L".dll", L".ocx", L".sys", L".cpl", L".scr", L".drv",
    };
    return std::find(kRunnableExtensions.begin(), kRunnableExtensions.end(), ext) != kRunnableExtensions.end();
}

FileVersionFields ReadFileVersionFields(const std::filesystem::path& path) {
    FileVersionFields fields;

    std::wstring widePath = path.wstring();
    DWORD ignoredHandle = 0;
    DWORD size = GetFileVersionInfoSizeW(widePath.c_str(), &ignoredHandle);
    if (size == 0) return fields;

    std::vector<BYTE> buffer(size);
    if (!GetFileVersionInfoW(widePath.c_str(), ignoredHandle, size, buffer.data())) return fields;

    // The StringFileInfo block's language/codepage sub-block isn't a fixed
    // value -- it's whatever the file was built with (e.g. a Hungarian-
    // localized system binary uses "040E04B0", not English's "040904B0").
    // Help.cpp's own identical-looking query against the *running app's
    // own* exe can safely hardcode "040904B0" because this project only
    // ever builds that one English block (MyCommander.rc's own
    // StringFileInfo); an arbitrary file on disk carries no such
    // guarantee, so this reads \VarFileInfo\Translation first to find the
    // block the file actually has, the standard technique for this API
    // (falls back to the fixed "040904B0" guess only if the translation
    // block itself is missing/empty, rather than giving up outright).
    struct LangAndCodePage {
        WORD language;
        WORD codePage;
    };
    LangAndCodePage* translations = nullptr;
    UINT translationsBytes = 0;
    std::wstring block = L"040904B0";
    if (VerQueryValueW(buffer.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<LPVOID*>(&translations),
                       &translationsBytes) &&
        translations && translationsBytes >= sizeof(LangAndCodePage)) {
        wchar_t buf[9];
        swprintf_s(buf, L"%04x%04x", translations[0].language, translations[0].codePage);
        block = buf;
    }

    auto queryString = [&](const wchar_t* field) -> std::wstring {
        wchar_t subBlock[64];
        swprintf_s(subBlock, L"\\StringFileInfo\\%s\\%s", block.c_str(), field);
        LPVOID value = nullptr;
        UINT valueLen = 0;
        if (VerQueryValueW(buffer.data(), subBlock, &value, &valueLen) && value && valueLen > 0) {
            return std::wstring(static_cast<const wchar_t*>(value));
        }
        return L"";
    };

    fields.description = queryString(L"FileDescription");
    fields.fileVersion = queryString(L"FileVersion");
    fields.productName = queryString(L"ProductName");
    fields.productVersion = queryString(L"ProductVersion");
    fields.copyright = queryString(L"LegalCopyright");
    fields.available = true;
    return fields;
}

SignatureInfo CheckDigitalSignature(const std::filesystem::path& path) {
    SignatureInfo info;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        info.status = SignatureStatus::CheckFailed;
        return info;
    }

    std::wstring widePath = path.wstring();
    WINTRUST_FILE_INFO fileData{};
    fileData.cbStruct = sizeof(fileData);
    fileData.pcwszFilePath = widePath.c_str();

    GUID policyGUID = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA winTrustData{};
    winTrustData.cbStruct = sizeof(winTrustData);
    winTrustData.dwUIChoice = WTD_UI_NONE;
    // No revocation/network check -- this is a quick, local, best-effort
    // presence check (the request's own "try to show" framing), not a full
    // trust verdict.
    winTrustData.fdwRevocationChecks = WTD_REVOKE_NONE;
    winTrustData.dwUnionChoice = WTD_CHOICE_FILE;
    winTrustData.pFile = &fileData;
    winTrustData.dwStateAction = WTD_STATEACTION_VERIFY;
    winTrustData.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;

    LONG result = WinVerifyTrust(nullptr, &policyGUID, &winTrustData);
    if (result == ERROR_SUCCESS) {
        info.status = SignatureStatus::Signed;
        info.signerName = ExtractSignerName(path);
    }

    // WinVerifyTrust requires a matching WTD_STATEACTION_CLOSE call to
    // release the state its own VERIFY action allocated.
    winTrustData.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &policyGUID, &winTrustData);

    if (result != ERROR_SUCCESS) {
        // No embedded signature -- most Windows system binaries are
        // catalog-signed instead (see FindCoveringCatalog's own comment).
        std::filesystem::path catalogPath = FindCoveringCatalog(path);
        if (!catalogPath.empty()) {
            info.status = SignatureStatus::Signed;
            info.signerName = ExtractSignerName(catalogPath);
        } else {
            info.status = SignatureStatus::NotSigned;
        }
    }

    return info;
}

}  // namespace mc
