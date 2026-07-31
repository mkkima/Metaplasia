#include "metaplasia/trust/authenticode.hpp"

#include "metaplasia/base/unique_handle.hpp"

#include <Windows.h>
#include <Softpub.h>
#include <Wincrypt.h>
#include <Wintrust.h>

#include <algorithm>
#include <array>
#include <iomanip>
#include <sstream>
#include <utility>

namespace metaplasia::trust {
namespace {

using HelperProvDataFunction = CRYPT_PROVIDER_DATA*(WINAPI*)(HANDLE);
using HelperSignerFunction = CRYPT_PROVIDER_SGNR*(WINAPI*)(
    CRYPT_PROVIDER_DATA*,
    DWORD,
    BOOL,
    DWORD);
using HelperCertificateFunction = CRYPT_PROVIDER_CERT*(WINAPI*)(
    CRYPT_PROVIDER_SGNR*,
    DWORD);

bool IsZeroThumbprint(const PublisherThumbprint& thumbprint) noexcept {
    return std::all_of(
        thumbprint.begin(),
        thumbprint.end(),
        [](const std::uint8_t value) { return value == 0; });
}

Result<void> ReadSigner(
    const HANDLE state_data,
    AuthenticodeResult& result) {
    const HMODULE wintrust = ::GetModuleHandleW(L"wintrust.dll");
    if (wintrust == nullptr) {
        return Status::FromWin32(
            "GetModuleHandleW(wintrust.dll)",
            ::GetLastError());
    }
    const auto helper_data = reinterpret_cast<HelperProvDataFunction>(
        ::GetProcAddress(wintrust, "WTHelperProvDataFromStateData"));
    const auto helper_signer = reinterpret_cast<HelperSignerFunction>(
        ::GetProcAddress(wintrust, "WTHelperGetProvSignerFromChain"));
    const auto helper_certificate =
        reinterpret_cast<HelperCertificateFunction>(
            ::GetProcAddress(wintrust, "WTHelperGetProvCertFromChain"));
    if (helper_data == nullptr || helper_signer == nullptr ||
        helper_certificate == nullptr) {
        return Status(
            ErrorCode::not_found,
            "WinTrust signer helper API is unavailable");
    }

    CRYPT_PROVIDER_DATA* provider_data = helper_data(state_data);
    CRYPT_PROVIDER_SGNR* signer =
        provider_data != nullptr
            ? helper_signer(provider_data, 0, FALSE, 0)
            : nullptr;
    CRYPT_PROVIDER_CERT* certificate =
        signer != nullptr ? helper_certificate(signer, 0) : nullptr;
    if (certificate == nullptr || certificate->pCert == nullptr) {
        return Status(
            ErrorCode::invalid_data,
            "Authenticode signer certificate is unavailable");
    }

    DWORD thumbprint_size =
        static_cast<DWORD>(result.publisher_thumbprint.size());
    if (!::CertGetCertificateContextProperty(
            certificate->pCert,
            CERT_SHA256_HASH_PROP_ID,
            result.publisher_thumbprint.data(),
            &thumbprint_size) ||
        thumbprint_size != result.publisher_thumbprint.size()) {
        return Status::FromWin32(
            "CertGetCertificateContextProperty(SHA-256)",
            ::GetLastError());
    }

    const DWORD subject_size = ::CertGetNameStringW(
        certificate->pCert,
        CERT_NAME_SIMPLE_DISPLAY_TYPE,
        0,
        nullptr,
        nullptr,
        0);
    constexpr DWORD maximum_subject_characters = 1024;
    if (subject_size == 0 || subject_size > maximum_subject_characters) {
        return Status(
            ErrorCode::invalid_data,
            "Authenticode publisher name is invalid");
    }
    std::wstring subject(subject_size, L'\0');
    if (::CertGetNameStringW(
            certificate->pCert,
            CERT_NAME_SIMPLE_DISPLAY_TYPE,
            0,
            nullptr,
            subject.data(),
            subject_size) != subject_size) {
        return Status::FromWin32(
            "CertGetNameStringW(publisher)",
            ::GetLastError());
    }
    if (!subject.empty() && subject.back() == L'\0') {
        subject.pop_back();
    }
    if (subject.empty()) {
        return Status(
            ErrorCode::invalid_data,
            "Authenticode publisher name is empty");
    }
    result.publisher_name = std::move(subject);
    return {};
}

}  // namespace

Result<AuthenticodeResult> VerifyAuthenticode(
    const std::filesystem::path& path) {
    if (path.empty() || !path.is_absolute()) {
        return Status(
            ErrorCode::invalid_argument,
            "Authenticode path must be absolute");
    }
    std::error_code filesystem_error;
    auto canonical = std::filesystem::weakly_canonical(path, filesystem_error);
    if (filesystem_error ||
        !std::filesystem::is_regular_file(canonical, filesystem_error) ||
        filesystem_error) {
        return Status(
            ErrorCode::not_found,
            "Authenticode target is not a regular file");
    }

    // Hold a non-delete-shared read handle for the whole verification so the
    // path cannot be replaced between hashing and signer extraction.
    UniqueHandle verified_file(::CreateFileW(
        canonical.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!verified_file) {
        return Status::FromWin32(
            "CreateFileW(Authenticode target)",
            ::GetLastError());
    }

    WINTRUST_FILE_INFO file{};
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = canonical.c_str();
    file.hFile = verified_file.get();

    WINTRUST_DATA data{};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags =
        WTD_CACHE_ONLY_URL_RETRIEVAL |
        WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT |
        WTD_DISABLE_MD2_MD4;
    data.dwUIContext = WTD_UICONTEXT_EXECUTE;

    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG trust_status = ::WinVerifyTrust(
        reinterpret_cast<HWND>(INVALID_HANDLE_VALUE),
        &action,
        &data);

    AuthenticodeResult result;
    result.native_status = static_cast<std::uint32_t>(trust_status);
    if (trust_status == ERROR_SUCCESS) {
        auto signer = ReadSigner(data.hWVTStateData, result);
        if (!signer.ok()) {
            result.state = SignatureState::invalid;
            result.detail = signer.status().message();
        } else {
            result.state = SignatureState::trusted;
            result.detail = "Trusted Authenticode signature";
        }
    } else if (trust_status == TRUST_E_NOSIGNATURE) {
        result.state = SignatureState::unsigned_file;
        result.detail = "File has no Authenticode signature";
    } else {
        result.state = SignatureState::invalid;
        result.detail =
            "Authenticode trust verification failed with status 0x";
        std::ostringstream status;
        status << std::hex << std::uppercase
               << static_cast<std::uint32_t>(trust_status);
        result.detail += status.str();
    }

    // Every VERIFY action must be paired with CLOSE, including failed trust
    // decisions whose provider allocated partial state.
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    static_cast<void>(::WinVerifyTrust(
        reinterpret_cast<HWND>(INVALID_HANDLE_VALUE),
        &action,
        &data));
    return result;
}

ComponentTrustDecision EvaluateComponentTrust(
    const std::span<const ComponentSignature> components,
    const bool allow_unsigned_development) {
    ComponentTrustDecision decision;
    if (components.empty()) {
        decision.detail = "Component trust set is empty";
        return decision;
    }

    bool all_trusted = true;
    bool all_unsigned = true;
    for (const auto& component : components) {
        if (component.component_name.empty()) {
            decision.detail = "Component trust set contains an unnamed file";
            return decision;
        }
        if (component.signature.state == SignatureState::invalid) {
            decision.detail =
                component.component_name + ": " + component.signature.detail;
            return decision;
        }
        all_trusted = all_trusted &&
                      component.signature.state == SignatureState::trusted;
        all_unsigned = all_unsigned &&
                       component.signature.state == SignatureState::unsigned_file;
    }

    if (all_trusted) {
        const auto expected = components.front().signature.publisher_thumbprint;
        if (IsZeroThumbprint(expected)) {
            decision.detail = "Trusted component has no publisher thumbprint";
            return decision;
        }
        for (const auto& component : components) {
            if (component.signature.publisher_thumbprint != expected) {
                decision.detail =
                    "Component publisher mismatch: " + component.component_name;
                return decision;
            }
        }
        decision.accepted = true;
        decision.publisher_thumbprint = expected;
        decision.detail = "All components have one trusted Authenticode publisher";
        return decision;
    }

    if (all_unsigned && allow_unsigned_development) {
        decision.accepted = true;
        decision.development_override = true;
        decision.detail =
            "Unsigned component set accepted by Debug-only development policy";
        return decision;
    }
    decision.detail = all_unsigned
                          ? "Unsigned components are forbidden by production policy"
                          : "Mixed signed and unsigned component set is forbidden";
    return decision;
}

Result<ComponentTrustDecision> VerifyComponentFiles(
    const std::vector<std::pair<std::string, std::filesystem::path>>& files,
    const bool allow_unsigned_development) {
    if (files.empty()) {
        return Status(ErrorCode::invalid_argument, "Component file set is empty");
    }
    std::vector<ComponentSignature> signatures;
    signatures.reserve(files.size());
    for (const auto& [name, path] : files) {
        if (name.empty()) {
            return Status(ErrorCode::invalid_argument, "Component name is empty");
        }
        auto signature = VerifyAuthenticode(path);
        if (!signature.ok()) {
            return signature.status();
        }
        signatures.push_back(ComponentSignature{
            name,
            std::move(signature).value()});
    }
    return EvaluateComponentTrust(signatures, allow_unsigned_development);
}

std::string HexThumbprint(const PublisherThumbprint& thumbprint) {
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto byte : thumbprint) {
        output << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return output.str();
}

}  // namespace metaplasia::trust
