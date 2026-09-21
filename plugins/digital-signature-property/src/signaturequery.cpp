#include "signaturequery.h"

#include <windows.h>
#include <wintrust.h>
#include <softpub.h>
#include <wincrypt.h>
#include <mscat.h>
#include <wchar.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>


namespace {

// Trust/certificate status codes returned by WinVerifyTrust. WinVerifyTrust
// returns a LONG status, not an HRESULT, but these constants use the same
// numeric values.
constexpr unsigned kCertExpired             = 0x800B0101;
constexpr unsigned kCertUntrustedRoot       = 0x800B0109;
constexpr unsigned kCertChaining            = 0x800B010A;
constexpr unsigned kCertRevoked             = 0x800B010C;
constexpr unsigned kCertRevocationFailure   = 0x800B010E;
constexpr unsigned kCertWrongUsage          = 0x800B0110;
constexpr unsigned kTrustExplicitDistrust   = 0x800B0111;
constexpr unsigned kTrustSubjectNotTrusted  = 0x800B0004;

// CRYPT_E_FILE_ERROR and the wrapped Win32 file-access errors report that the
// trust provider could not read the subject file. They are read failures, not a
// statement about the signature, so they must not fall through to
// "UnsupportedOrMalformed".
constexpr unsigned kCryptFileError            = 0x80092003;
constexpr unsigned kWin32FileNotFound         = 0x80070002;
constexpr unsigned kWin32PathNotFound         = 0x80070003;
constexpr unsigned kWin32AccessDenied         = 0x80070005;
constexpr unsigned kWin32SharingViolation     = 0x80070020;
constexpr unsigned kWin32LockViolation        = 0x80070021;

// Authenticode counter-signature and RFC 3161 timestamp attribute OIDs.
constexpr const char *kAuthenticodeCounterSignatureOid = "1.2.840.113549.1.9.6";
constexpr const char *kRfc3161TimestampOid             = "1.3.6.1.4.1.311.3.3.1";

std::string fromWide(const wchar_t *s)
{
    if (!s) {
        return std::string();
    }
    const int len  = static_cast<int>(std::wcslen(s));
    const int size = WideCharToMultiByte(CP_UTF8, 0, s, len, nullptr, 0,
                                         nullptr, nullptr);
    if (size <= 0) {
        return std::string();
    }
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, len, &out[0], size, nullptr, nullptr);
    return out;
}

std::string formatFileTime(const FILETIME &ft)
{
    SYSTEMTIME st{};
    if (!FileTimeToSystemTime(&ft, &st)) {
        return std::string();
    }
    char buf[64] = {};
    snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u", st.wYear,
             st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return std::string(buf);
}

std::string hexBytes(const BYTE *data, DWORD len)
{
    static const char *kDigits = "0123456789ABCDEF";
    std::string hex;
    hex.reserve(static_cast<std::size_t>(len) * 2);
    for (DWORD i = 0; i < len; ++i) {
        hex.push_back(kDigits[(data[i] >> 4) & 0xF]);
        hex.push_back(kDigits[data[i] & 0xF]);
    }
    return hex;
}

// Maps a signing digest OID to its usual name. Unrecognized OIDs are reported
// verbatim so the field is never silently empty.
std::string digestAlgorithmName(const char *oid)
{
    if (!oid) {
        return std::string();
    }
    struct Entry {
        const char *oid;
        const char *name;
    };
    static const Entry kEntries[] = {
        {"1.2.840.113549.1.1.4", "MD5"},     {"1.2.840.113549.1.1.5", "SHA-1"},
        {"1.2.840.113549.1.1.11", "SHA-256"}, {"1.2.840.113549.1.1.12", "SHA-384"},
        {"1.2.840.113549.1.1.13", "SHA-512"}, {"1.3.14.3.2.26", "SHA-1"},
        {"2.16.840.1.101.3.4.2.1", "SHA-256"},
        {"2.16.840.1.101.3.4.2.2", "SHA-384"},
        {"2.16.840.1.101.3.4.2.3", "SHA-512"},
    };
    for (const auto &entry : kEntries) {
        if (std::strcmp(entry.oid, oid) == 0) {
            return std::string(entry.name);
        }
    }
    return std::string(oid);
}

std::string certificateThumbprint(PCCERT_CONTEXT context)
{
    // Prefer SHA-256 and fall back to SHA-1 so the reported algorithm always
    // matches the reported value.
    struct Choice {
        DWORD property;
        const char *algorithm;
    };
    static const Choice kChoices[] = {
        {CERT_SHA256_HASH_PROP_ID, "SHA-256"},
        {CERT_SHA1_HASH_PROP_ID, "SHA-1"},
    };
    for (const auto &choice : kChoices) {
        DWORD size = 0;
        if (!CertGetCertificateContextProperty(
                context, choice.property, nullptr, &size)
            || size == 0) {
            continue;
        }
        std::vector<BYTE> hash(size);
        if (!CertGetCertificateContextProperty(context, choice.property,
                                               hash.data(), &size)) {
            continue;
        }
        return std::string(choice.algorithm) + " " + hexBytes(hash.data(), size);
    }
    return std::string();
}

// Closes the two handles a CryptQueryObject call produces on every path. A
// certificate file opens a store without a message, so both handles are
// optional; a null handle must not reach CryptMsgGetParam.
class CryptQueryGuard {
public:
    CryptQueryGuard(HCRYPTMSG message, HCERTSTORE store)
        : message_(message), store_(store)
    {
    }
    ~CryptQueryGuard()
    {
        if (message_) {
            CryptMsgClose(message_);
        }
        if (store_) {
            CertCloseStore(store_, 0);
        }
    }
    CryptQueryGuard(const CryptQueryGuard &)            = delete;
    CryptQueryGuard &operator=(const CryptQueryGuard &) = delete;

    HCRYPTMSG message() const { return message_; }
    HCERTSTORE store() const { return store_; }

private:
    HCRYPTMSG message_;
    HCERTSTORE store_;
};

// Examines an embedded Authenticode signature. Extracts the signer count and the
// metadata of the first signer only, so callers can state exactly what was
// examined instead of generalizing one signer to every signer.
//
// Never crashes: on any failure the extracted fields are left unset. Trust
// results are produced separately by WinVerifyTrust, keeping cryptographic
// results and certificate metadata independent.
void inspectEmbeddedSignature(const std::wstring &path,
                              SignatureQueryResult &out)
{
    DWORD encoding = 0, contentType = 0, formatType = 0;
    HCRYPTMSG message = nullptr;
    HCERTSTORE store  = nullptr;

    const BOOL opened = CryptQueryObject(
        CERT_QUERY_OBJECT_FILE, path.c_str(), CERT_QUERY_CONTENT_FLAG_ALL,
        CERT_QUERY_FORMAT_FLAG_ALL, 0, &encoding, &contentType, &formatType,
        &store, &message, nullptr);
    // A query can succeed with a store but no message (a plain certificate
    // file); without a message there is no signer to examine.
    const CryptQueryGuard guard(message, store);
    if (!opened || guard.message() == nullptr) {
        return;
    }
    if (formatType == CERT_QUERY_FORMAT_BINARY
        && (contentType == CERT_QUERY_CONTENT_PKCS7_SIGNED
            || contentType == CERT_QUERY_CONTENT_PKCS7_SIGNED_EMBED)) {
        out.signatureKind = "Authenticode PKCS#7 (embedded)";
    }

    DWORD signerCount = 0;
    DWORD countSize   = sizeof(signerCount);
    if (CryptMsgGetParam(guard.message(), CMSG_SIGNER_COUNT_PARAM, 0,
                         &signerCount, &countSize)
        && signerCount > 0) {
        out.signersExamined = (signerCount == 1)
                                  ? std::string("1")
                                  : ("first of " + std::to_string(signerCount));
    }

    DWORD signerSize = 0;
    if (!CryptMsgGetParam(guard.message(), CMSG_SIGNER_INFO_PARAM, 0, nullptr,
                          &signerSize)
        || signerSize == 0) {
        return;
    }

    std::vector<BYTE> signerBuffer(signerSize);
    if (!CryptMsgGetParam(guard.message(), CMSG_SIGNER_INFO_PARAM, 0,
                          signerBuffer.data(), &signerSize)) {
        return;
    }
    const auto *signer
        = reinterpret_cast<const CMSG_SIGNER_INFO *>(signerBuffer.data());

    if (signer->HashAlgorithm.pszObjId) {
        out.signingDigestAlgorithm
            = digestAlgorithmName(signer->HashAlgorithm.pszObjId);
    }

    // CERT_FIND_SUBJECT_CERT matches on the issuer and serial number of a
    // CERT_INFO, so the signer's issuer blob alone is not a valid search key.
    CERT_INFO signerIdentity{};
    signerIdentity.Issuer       = signer->Issuer;
    signerIdentity.SerialNumber = signer->SerialNumber;

    PCCERT_CONTEXT signerCertificate = CertFindCertificateInStore(
        guard.store(), X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
        CERT_FIND_SUBJECT_CERT, &signerIdentity, nullptr);
    if (signerCertificate) {
        WCHAR name[MAX_PATH] = {};
        if (CertGetNameStringW(signerCertificate, CERT_NAME_SIMPLE_DISPLAY_TYPE,
                               0, nullptr, name, MAX_PATH)) {
            out.publisher = fromWide(name);
        }
        if (CertGetNameStringW(signerCertificate, CERT_NAME_SIMPLE_DISPLAY_TYPE,
                               CERT_NAME_ISSUER_FLAG, nullptr, name, MAX_PATH)) {
            out.issuer = fromWide(name);
        }
        const std::string fingerprint = certificateThumbprint(signerCertificate);
        if (!fingerprint.empty()) {
            const std::size_t space = fingerprint.find(' ');
            out.thumbprintAlgorithm  = fingerprint.substr(0, space);
            out.thumbprint
                = space == std::string::npos ? std::string()
                                             : fingerprint.substr(space + 1);
        }
        if (signerCertificate->pCertInfo) {
            out.validFrom
                = formatFileTime(signerCertificate->pCertInfo->NotBefore);
            out.validTo = formatFileTime(signerCertificate->pCertInfo->NotAfter);
        }
        CertFreeCertificateContext(signerCertificate);
    }

    // Timestamp presence is detected from the signer's unauthenticated
    // attributes. Only presence is reported: the signing time itself is not
    // invented, and no timestamp verification is claimed in this offline scope.
    for (DWORD i = 0; i < signer->UnauthAttrs.cAttr; ++i) {
        const CRYPT_ATTRIBUTE &attribute = signer->UnauthAttrs.rgAttr[i];
        const char *oid                  = attribute.pszObjId;
        if (oid
            && (std::strcmp(oid, kAuthenticodeCounterSignatureOid) == 0
                || std::strcmp(oid, kRfc3161TimestampOid) == 0)) {
            out.hasTimestamp = true;
            break;
        }
    }
    if (out.hasTimestamp) {
        out.timestampStatus
            = "timestamp attribute present; not verified offline, signing time "
              "not reported";
    }
}

// Bounded, fully local catalog-signature fallback.
//
// WinVerifyTrust with the generic verify policy and WTD_CHOICE_FILE only
// considers the embedded signature. Files that are signed through the installed
// catalog database therefore need an explicit lookup: hash the file with the
// Catalog API, enumerate the local catalogs for that hash, and verify the member
// against the catalog that contains it. No network access and no catalog
// download is involved.
std::string trustStatusText(LONG status);

enum class CatalogState { NotQueried, Signed, NotSigned, QueryFailed };

struct CatalogResult {
    CatalogState state = CatalogState::NotQueried;
    std::string detail;
    std::string statusText;
};

std::string quoteWin32Error(DWORD error)
{
    return "Win32 error " + std::to_string(error);
}

bool isFileReadFailure(unsigned status)
{
    switch (status) {
    case kCryptFileError:
    case kWin32FileNotFound:
    case kWin32PathNotFound:
    case kWin32AccessDenied:
    case kWin32SharingViolation:
    case kWin32LockViolation:
        return true;
    default:
        return false;
    }
}

// Closes the WinVerifyTrust state on every path. The provider keeps
// certificate, catalog and provider state alive until WTD_STATEACTION_CLOSE
// runs, so an early return without it leaks that state.
class WinTrustStateGuard {
public:
    WinTrustStateGuard(WINTRUST_DATA &data, const GUID &action)
        : data_(data), action_(action)
    {
    }

    ~WinTrustStateGuard() { close(); }

    WinTrustStateGuard(const WinTrustStateGuard &)            = delete;
    WinTrustStateGuard &operator=(const WinTrustStateGuard &) = delete;

private:
    void close()
    {
        if (!data_.hWVTStateData) {
            return;
        }
        WINTRUST_DATA closeData{};
        closeData.cbStruct      = sizeof(closeData);
        closeData.dwUnionChoice = data_.dwUnionChoice;
        // One union slot: whichever member is in use occupies the same storage
        // as pFile, and dwUnionChoice above selects how the provider reads it.
        closeData.pFile         = data_.pFile;
        closeData.dwUIChoice    = data_.dwUIChoice;
        closeData.dwStateAction = WTD_STATEACTION_CLOSE;
        closeData.hWVTStateData = data_.hWVTStateData;
        WinVerifyTrust(nullptr, &action_, &closeData);
        data_.hWVTStateData = nullptr;
    }

    WINTRUST_DATA &data_;
    GUID action_;
};

// Resource guards: every cleanup path in the catalog query used to repeat the
// same release calls, and a missing one leaks a kernel handle.
class FileHandleGuard {
public:
    explicit FileHandleGuard(HANDLE handle) : handle_(handle) {}
    ~FileHandleGuard()
    {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }
    FileHandleGuard(const FileHandleGuard &)            = delete;
    FileHandleGuard &operator=(const FileHandleGuard &) = delete;

    HANDLE get() const { return handle_; }
    bool valid() const
    {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE handle_;
};

class CatalogAdminGuard {
public:
    explicit CatalogAdminGuard(HCATADMIN admin) : admin_(admin) {}
    ~CatalogAdminGuard()
    {
        if (admin_) {
            CryptCATAdminReleaseContext(admin_, 0);
        }
    }
    CatalogAdminGuard(const CatalogAdminGuard &)            = delete;
    CatalogAdminGuard &operator=(const CatalogAdminGuard &) = delete;

private:
    HCATADMIN admin_;
};

class CatalogInfoGuard {
public:
    CatalogInfoGuard(HCATADMIN admin, HCATINFO info)
        : admin_(admin), info_(info)
    {
    }
    ~CatalogInfoGuard()
    {
        if (info_) {
            CryptCATAdminReleaseCatalogContext(admin_, info_, 0);
        }
    }
    CatalogInfoGuard(const CatalogInfoGuard &)            = delete;
    CatalogInfoGuard &operator=(const CatalogInfoGuard &) = delete;

    HCATINFO get() const { return info_; }
    bool valid() const { return info_ != nullptr; }

private:
    HCATADMIN admin_;
    HCATINFO info_;
};

CatalogResult queryCatalogSignature(const std::wstring &path)
{
    CatalogResult result;
    result.state = CatalogState::QueryFailed;

    // Declared in reverse release order: the catalog context goes first, then
    // the file handle, then the administrator context.
    const FileHandleGuard file(CreateFileW(
        path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.valid()) {
        result.detail = "cannot open the file for hashing ("
                        + quoteWin32Error(GetLastError()) + ")";
        result.statusText = "query unavailable: " + result.detail;
        return result;
    }

    HCATADMIN admin = nullptr;
    if (!CryptCATAdminAcquireContext(&admin, nullptr, 0)) {
        result.detail = "catalog administrator context unavailable ("
                        + quoteWin32Error(GetLastError()) + ")";
        result.statusText = "query unavailable: " + result.detail;
        return result;
    }
    const CatalogAdminGuard adminGuard(admin);

    DWORD hashSize = 0;
    if (!CryptCATAdminCalcHashFromFileHandle(file.get(), &hashSize, nullptr, 0)
        || hashSize == 0) {
        result.detail = "file hash for catalog lookup failed ("
                        + quoteWin32Error(GetLastError()) + ")";
        result.statusText = "query unavailable: " + result.detail;
        return result;
    }

    std::vector<BYTE> hash(hashSize);
    if (!CryptCATAdminCalcHashFromFileHandle(file.get(), &hashSize,
                                             hash.data(), 0)) {
        result.detail = "file hash for catalog lookup failed ("
                        + quoteWin32Error(GetLastError()) + ")";
        result.statusText = "query unavailable: " + result.detail;
        return result;
    }

    const CatalogInfoGuard catalogInfo(
        admin, CryptCATAdminEnumCatalogFromHash(admin, hash.data(), hashSize, 0,
                                                nullptr));
    if (!catalogInfo.valid()) {
        result.state      = CatalogState::NotSigned;
        result.detail     = "no installed catalog contains this file hash";
        result.statusText = "absent";
        return result;
    }

    CATALOG_INFO catalogDetails{};
    catalogDetails.cbStruct = sizeof(catalogDetails);
    std::wstring catalogPath;
    if (CryptCATCatalogInfoFromContext(catalogInfo.get(), &catalogDetails, 0)) {
        catalogPath = catalogDetails.wszCatalogFile;
    }

    WINTRUST_CATALOG_INFO verifyCatalog{};
    verifyCatalog.cbStruct             = sizeof(verifyCatalog);
    verifyCatalog.pcwszCatalogFilePath = catalogPath.c_str();
    verifyCatalog.pcwszMemberFilePath  = path.c_str();
    verifyCatalog.hMemberFile          = file.get();
    verifyCatalog.pbCalculatedFileHash = hash.data();
    verifyCatalog.cbCalculatedFileHash = hashSize;

    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;

    WINTRUST_DATA trustData{};
    trustData.cbStruct            = sizeof(trustData);
    trustData.dwUnionChoice       = WTD_CHOICE_CATALOG;
    trustData.pCatalog            = &verifyCatalog;
    trustData.dwUIChoice          = WTD_UI_NONE;
    trustData.dwUIContext         = WTD_UICONTEXT_EXECUTE;
    trustData.fdwRevocationChecks = WTD_REVOKE_NONE;
    // Same provider policy as the embedded query: cache-only and with MD2/MD4
    // digests rejected.
    trustData.dwProvFlags   = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_DISABLE_MD2_MD4;
    trustData.dwStateAction = WTD_STATEACTION_VERIFY;

    const LONG status = WinVerifyTrust(nullptr, &action, &trustData);
    const WinTrustStateGuard stateGuard(trustData, action);

    if (status == 0) {
        result.state      = CatalogState::Signed;
        result.detail     = "member of an installed local catalog";
        result.statusText = catalogPath.empty()
                                ? std::string("present")
                                : ("present: " + fromWide(catalogPath.c_str()));
    }
    else {
        result.state      = CatalogState::QueryFailed;
        result.detail = isFileReadFailure(static_cast<unsigned>(status))
                            ? ("cannot read the file to verify the catalog "
                               "member: "
                               + trustStatusText(status))
                            : ("catalog member verification failed: "
                               + trustStatusText(status));
        result.statusText = "query unavailable: " + result.detail;
    }
    return result;
}

std::string trustStatusText(LONG status)
{
    const unsigned value = static_cast<unsigned>(status);
    switch (status) {
    case 0:
        return "Verified";
    case TRUST_E_NOSIGNATURE:
        return "NoSignature";
    case TRUST_E_BAD_DIGEST:
        return "HashMismatch";
    case kTrustExplicitDistrust:
        return "ExplicitDistrust";
    case kTrustSubjectNotTrusted:
        return "SubjectNotTrusted";
    case TRUST_E_PROVIDER_UNKNOWN:
        return "ProviderUnknown";
    case TRUST_E_ACTION_UNKNOWN:
        return "ActionUnknown";
    case TRUST_E_SUBJECT_FORM_UNKNOWN:
        return "SubjectFormUnknown";
    case kCertExpired:
        return "Expired";
    case kCertUntrustedRoot:
        return "UntrustedRoot";
    case kCertRevoked:
        return "Revoked";
    case kCertWrongUsage:
        return "WrongUsage";
    case kCertChaining:
        return "ChainingError";
    case kCertRevocationFailure:
        return "RevocationFailure";
    default: {
        char buffer[32] = {};
        snprintf(buffer, sizeof(buffer), "Error0x%08X", value);
        return std::string(buffer);
    }
    }
}

bool fileIdentityChanged(const WIN32_FILE_ATTRIBUTE_DATA &before,
                         const WIN32_FILE_ATTRIBUTE_DATA &after)
{
    return after.nFileSizeLow != before.nFileSizeLow
           || after.nFileSizeHigh != before.nFileSizeHigh
           || after.ftLastWriteTime.dwLowDateTime
                  != before.ftLastWriteTime.dwLowDateTime
           || after.ftLastWriteTime.dwHighDateTime
                  != before.ftLastWriteTime.dwHighDateTime;
}

// Reads only the DOS/NT headers to decide whether Authenticode is applicable at
// all, and whether the image carries an embedded certificate table. The file is
// never mapped, loaded or executed.
enum class ImageProbe { PortableExecutable, NotPortableExecutable, Unreadable };

struct ImageHeaders {
    ImageProbe probe = ImageProbe::Unreadable;
    // Size of IMAGE_DIRECTORY_ENTRY_SECURITY in the optional header. Non-zero
    // means the image carries an embedded signature blob.
    DWORD certificateTableSize = 0;
};

bool readAt(HANDLE file, LONGLONG offset, void *buffer, DWORD size)
{
    LARGE_INTEGER position{};
    position.QuadPart = offset;
    if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) {
        return false;
    }
    DWORD bytesRead = 0;
    return ReadFile(file, buffer, size, &bytesRead, nullptr) == TRUE
           && bytesRead == size;
}

ImageHeaders probePortableExecutable(const std::wstring &path, DWORD *win32Error)
{
    ImageHeaders headers;

    const HANDLE file = CreateFileW(
        path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (win32Error) {
            *win32Error = GetLastError();
        }
        return headers;
    }

    headers.probe = ImageProbe::NotPortableExecutable;

    IMAGE_DOS_HEADER dosHeader{};
    DWORD ntSignature = 0;
    if (readAt(file, 0, &dosHeader, sizeof(dosHeader))
        && dosHeader.e_magic == IMAGE_DOS_SIGNATURE) {
        const LONGLONG ntHeaders = dosHeader.e_lfanew;
        if (readAt(file, ntHeaders, &ntSignature, sizeof(ntSignature))
            && ntSignature == IMAGE_NT_SIGNATURE) {
            // The optional header follows the 4-byte signature and the 20-byte
            // file header; its first field is the magic that selects the PE32
            // or PE32+ data directory layout.
            WORD magic = 0;
            if (readAt(file, ntHeaders + 4 + 20, &magic, sizeof(magic))) {
                LONGLONG rvaAndSizesOffset   = 0;
                LONGLONG dataDirectoryOffset = 0;
                if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {  // PE32+
                    rvaAndSizesOffset   = 108;
                    dataDirectoryOffset = 112;
                }
                else if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {  // PE32
                    rvaAndSizesOffset   = 92;
                    dataDirectoryOffset = 96;
                }
                if (dataDirectoryOffset != 0) {
                    headers.probe = ImageProbe::PortableExecutable;
                    // NumberOfRvaAndSizes sits just before the data directory.
                    // A security entry beyond the declared count would read
                    // section headers or garbage, so the count must cover
                    // index 4 before the entry is read.
                    DWORD numberOfRvaAndSizes = 0;
                    if (readAt(file, ntHeaders + 4 + 20 + rvaAndSizesOffset,
                               &numberOfRvaAndSizes,
                               sizeof(numberOfRvaAndSizes))
                        && numberOfRvaAndSizes
                               > IMAGE_DIRECTORY_ENTRY_SECURITY) {
                        const LONGLONG securityEntry
                            = ntHeaders + 4 + 20 + dataDirectoryOffset
                              + static_cast<LONGLONG>(
                                    IMAGE_DIRECTORY_ENTRY_SECURITY)
                                    * 8;
                        DWORD virtualAddress = 0;
                        DWORD size           = 0;
                        if (readAt(file, securityEntry, &virtualAddress,
                                   sizeof(virtualAddress))
                            && readAt(file, securityEntry + 4, &size,
                                      sizeof(size))) {
                            headers.certificateTableSize = size;
                        }
                    }
                }
            }
        }
    }

    CloseHandle(file);
    return headers;
}

}  // namespace

SignatureQueryResult queryFileSignature(const std::wstring &filePath)
{
    SignatureQueryResult out;
    out.catalogStatus = "not queried";
    if (filePath.empty()) {
        out.status = "QueryError";
        out.reason = "empty input path";
        return out;
    }

    const DWORD attributes = GetFileAttributesW(filePath.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES
        || (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        out.status = "ReadError";
        out.reason = "file does not exist or is not a regular file";
        return out;
    }

    WIN32_FILE_ATTRIBUTE_DATA before{};
    if (!GetFileAttributesExW(filePath.c_str(), GetFileExInfoStandard, &before)) {
        out.status = "ReadError";
        out.reason = "unable to stat input file";
        return out;
    }

    // Read-only header probe. Authenticode only applies to PE images, so a
    // malformed or unsupported input is classified as such instead of being
    // reported as "unsigned". The certificate table entry also tells us whether
    // an embedded signature blob is present at all.
    DWORD openError = 0;
    const ImageHeaders headers
        = probePortableExecutable(filePath, &openError);
    if (headers.probe == ImageProbe::Unreadable) {
        out.status = "ReadError";
        out.reason = "cannot open the file for reading (Win32 error "
                     + std::to_string(openError) + ")";
        return out;
    }
    if (headers.probe == ImageProbe::NotPortableExecutable) {
        out.status = "UnsupportedOrMalformed";
        out.source = "none";
        out.reason
            = "not a PE image; no Authenticode signature can be present";
        return out;
    }
    const bool hasEmbeddedCertificateTable = headers.certificateTableSize > 0;

    // Embedded signature metadata is examined first so that it survives
    // whatever the trust verdict turns out to be.
    if (hasEmbeddedCertificateTable) {
        inspectEmbeddedSignature(filePath, out);
    }
    if (out.signersExamined.empty()) {
        out.signersExamined = "none";
    }

    WINTRUST_FILE_INFO fileInfo{};
    fileInfo.cbStruct      = sizeof(WINTRUST_FILE_INFO);
    fileInfo.pcwszFilePath = filePath.c_str();

    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;

    WINTRUST_DATA trustData{};
    trustData.cbStruct            = sizeof(WINTRUST_DATA);
    trustData.dwUnionChoice       = WTD_CHOICE_FILE;
    trustData.pFile               = &fileInfo;
    trustData.dwUIChoice          = WTD_UI_NONE;
    trustData.dwUIContext         = WTD_UICONTEXT_EXECUTE;
    trustData.fdwRevocationChecks = WTD_REVOKE_NONE;
    // Cache-only: no certificate download, no online revocation request and no
    // trust prompt. WTD_REVOKE_NONE alone does not guarantee this, so the
    // provider flag is always set together with it.
    trustData.dwProvFlags   = WTD_CACHE_ONLY_URL_RETRIEVAL
                              | WTD_DISABLE_MD2_MD4;
    trustData.dwStateAction = WTD_STATEACTION_VERIFY;
    trustData.hWVTStateData = nullptr;

    const LONG status         = WinVerifyTrust(nullptr, &action, &trustData);
    const LONG verifiedStatus = status;
    // State cleanup runs on every path, including the failing ones.
    const WinTrustStateGuard stateGuard(trustData, action);

    WIN32_FILE_ATTRIBUTE_DATA after{};
    if (GetFileAttributesExW(filePath.c_str(), GetFileExInfoStandard, &after)
        && fileIdentityChanged(before, after)) {
        out.status = "ReadError";
        out.reason = "input file changed during inspection; metadata from "
                     "different file versions is not mixed";
        out.publisher.clear();
        out.issuer.clear();
        out.thumbprint.clear();
        out.thumbprintAlgorithm.clear();
        return out;
    }

    out.scope
        = "WinVerifyTrust(WINTRUST_ACTION_GENERIC_VERIFY_V2), WTD_UI_NONE; "
          "offline and cache-only; embedded signature first, then a bounded "
          "local catalog lookup";

    if (verifiedStatus == 0) {
        out.status        = "Valid";
        out.reason        = "embedded signature verified";
        out.source        = "embedded";
        out.catalogStatus = "not needed: the embedded signature verified";
        return out;
    }

    // No usable embedded signature: the installed local catalog database is the
    // next case to consider. "No embedded signature" alone never becomes
    // "unsigned".
    if (verifiedStatus == TRUST_E_NOSIGNATURE) {
        const CatalogResult catalog = queryCatalogSignature(filePath);
        out.catalogStatus           = catalog.statusText;

        if (catalog.state == CatalogState::Signed) {
            out.status        = "Valid";
            out.source        = "catalog";
            out.signatureKind = "local system catalog member";
            out.signersExamined
                = "not applicable: a catalog member has no embedded signer to "
                  "examine";
            out.reason = "verified as a member of an installed local catalog; "
                         "no usable embedded signature was found";
            return out;
        }

        if (hasEmbeddedCertificateTable) {
            // A certificate table exists but no usable signature could be
            // located. That is a malformed/unsupported signature, not the same
            // thing as a file that never carried one.
            out.status        = "UnsupportedOrMalformed";
            out.source        = "embedded";
            out.signatureKind = "certificate table present but unusable";
            out.reason        = "an embedded certificate table is present, but "
                                "no usable Authenticode signature could be "
                                "located (a signature must terminate the file) "
                                "and no catalog entry matches this file";
            return out;
        }

        if (catalog.state == CatalogState::QueryFailed) {
            out.status = "QueryError";
            out.source = "none";
            out.reason = "no embedded signature, and the local catalog lookup "
                         "could not complete (" + catalog.detail
                         + "); the file is not reported as unsigned";
            return out;
        }

        out.status = "Unsigned";
        out.source = "none";
        out.reason = "no embedded signature and no matching entry in the "
                     "installed local catalogs";
        return out;
    }

    // A signature exists but is not trusted; the verdict strictly comes from
    // the file-based embedded verification. No catalog lookup has run at this
    // point: the catalog is only consulted for TRUST_E_NOSIGNATURE.
    out.source        = "embedded";
    out.catalogStatus = "not queried: the failing verdict came from the "
                        "embedded-signature verification; no catalog lookup "
                        "has run";

    const unsigned unsignedStatus = static_cast<unsigned>(verifiedStatus);

    // The trust provider could not read the file (deleted, replaced or locked
    // while being inspected). That is a read failure, not a verdict about the
    // signature, so it must not be reported as UnsupportedOrMalformed.
    if (isFileReadFailure(unsignedStatus)) {
        out.status = "ReadError";
        out.source = "none";
        out.reason = "the trust provider could not read the file ("
                     + trustStatusText(verifiedStatus)
                     + "); it may have been changed, locked or removed while "
                       "being inspected";
        return out;
    }

    switch (unsignedStatus) {
    case TRUST_E_BAD_DIGEST:
        out.status = "HashMismatch";
        out.reason = "signature digest does not match the file content";
        break;
    case kCertRevoked:
        out.status = "Revoked";
        out.reason = "certificate is revoked according to available local "
                     "revocation evidence";
        break;
    case kCertRevocationFailure:
        out.status = "RevocationUnavailable";
        out.reason = "revocation information is unavailable offline; the "
                     "certificate is not reported as not-revoked";
        break;
    case kCertUntrustedRoot:
    case kCertChaining:
    case kCertExpired:
    case kCertWrongUsage:
    case kTrustExplicitDistrust:
    case kTrustSubjectNotTrusted:
        out.status = "UntrustedChain";
        out.reason = std::string("trust verification failed: ")
                     + trustStatusText(verifiedStatus);
        break;
    default:
        out.status = "UnsupportedOrMalformed";
        out.reason = std::string("unsupported or malformed signature: ")
                     + trustStatusText(verifiedStatus);
        break;
    }
    return out;
}
