#include "metaplasia/compatibility/catalog.hpp"

#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

void Require(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

metaplasia::compatibility::CompatibilityProfile TestProfile() {
    using namespace metaplasia::compatibility;
    return {
        "test-profile",
        AdapterId::file_explorer_title,
        {10, 0, 12345, 67},
        {
            {L"explorer.exe", L"explorer.exe", "explorer-key"},
            {L"user32.dll", L"System32\\user32.dll", "user32-key"},
        }};
}

std::vector<metaplasia::compatibility::ModuleObservation>
TestObservations() {
    using metaplasia::compatibility::ModuleObservation;
    return {
        ModuleObservation{
            L"EXPLORER.EXE",
            L"C:\\Windows\\explorer.exe",
            "explorer-key"},
        ModuleObservation{
            L"User32.dll",
            L"C:\\WINDOWS\\System32\\user32.dll",
            "user32-key"},
    };
}

template <typename T>
void AppendInteger(std::vector<std::byte>& output, const T value) {
    static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>);
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        output.push_back(static_cast<std::byte>(
            static_cast<unsigned long long>(value) >> (index * 8U)));
    }
}

void AppendText(std::vector<std::byte>& output, const std::string_view text) {
    for (const unsigned char character : text) {
        output.push_back(static_cast<std::byte>(character));
    }
}

void WriteUint32(
    std::vector<std::byte>& output,
    const std::size_t offset,
    const std::uint32_t value) {
    Require(offset + sizeof(value) <= output.size(), "patch encoded uint32");
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        output[offset + index] = static_cast<std::byte>(value >> (index * 8U));
    }
}

std::vector<std::byte> ProfileRecord(
    const std::string_view id,
    const std::string_view relative_path = "System32\\user32.dll") {
    const std::string_view module_name = "user32.dll";
    const std::string_view key = "8664-TEST-user32.pdb-ABC1";
    std::vector<std::byte> body;
    AppendInteger<std::uint8_t>(body, 2);
    AppendInteger<std::uint8_t>(body, 0);
    AppendInteger<std::uint16_t>(body, 1);
    AppendInteger<std::uint32_t>(body, 10);
    AppendInteger<std::uint32_t>(body, 0);
    AppendInteger<std::uint32_t>(body, 12345);
    AppendInteger<std::uint32_t>(body, 67);
    AppendInteger<std::uint16_t>(body, static_cast<std::uint16_t>(id.size()));
    AppendInteger<std::uint16_t>(body, 0);
    AppendText(body, id);
    AppendInteger<std::uint16_t>(
        body,
        static_cast<std::uint16_t>(module_name.size()));
    AppendInteger<std::uint16_t>(
        body,
        static_cast<std::uint16_t>(relative_path.size()));
    AppendInteger<std::uint16_t>(body, static_cast<std::uint16_t>(key.size()));
    AppendInteger<std::uint16_t>(body, 0);
    AppendText(body, module_name);
    AppendText(body, relative_path);
    AppendText(body, key);

    std::vector<std::byte> record;
    AppendInteger<std::uint32_t>(
        record,
        static_cast<std::uint32_t>(body.size() + sizeof(std::uint32_t)));
    record.insert(record.end(), body.begin(), body.end());
    return record;
}

std::vector<std::byte> ProfilePack(
    const std::vector<std::vector<std::byte>>& records) {
    std::vector<std::byte> pack;
    AppendInteger<std::uint32_t>(pack, 0x5043504D);
    AppendInteger<std::uint16_t>(pack, 1);
    AppendInteger<std::uint16_t>(pack, 16);
    AppendInteger<std::uint32_t>(pack, 0);
    AppendInteger<std::uint16_t>(
        pack,
        static_cast<std::uint16_t>(records.size()));
    AppendInteger<std::uint16_t>(pack, 0);
    for (const auto& record : records) {
        pack.insert(pack.end(), record.begin(), record.end());
    }
    WriteUint32(pack, 8, static_cast<std::uint32_t>(pack.size()));
    return pack;
}

}  // namespace

int main(const int argc, char** argv) {
    using namespace metaplasia::compatibility;

    Require(argc == 2, "resource-only compatibility fixture path supplied");

    const auto profile = TestProfile();
    const auto windows = profile.windows;
    auto observations = TestObservations();
    auto supported = EvaluateProfile(
        profile,
        windows,
        observations,
        L"C:\\Windows");
    Require(supported.supported, "exact profile is supported");
    Require(
        supported.profile_id == "test-profile",
        "profile identity is retained");
    Require(
        supported.modules.size() == observations.size(),
        "observations are retained for diagnostics");

    auto wrong_version = EvaluateProfile(
        profile,
        WindowsVersion{10, 0, 12345, 68},
        observations,
        L"C:\\Windows");
    Require(!wrong_version.supported, "revision mismatch fails closed");

    auto wrong_path = observations;
    wrong_path[1].path = L"C:\\Temp\\user32.dll";
    Require(
        !EvaluateProfile(
             profile,
             windows,
             wrong_path,
             L"C:\\Windows")
             .supported,
        "unexpected module path rejected");

    auto wrong_identity = observations;
    wrong_identity[0].compatibility_key = "unknown-key";
    Require(
        !EvaluateProfile(
             profile,
             windows,
             wrong_identity,
             L"C:\\Windows")
             .supported,
        "unknown module identity rejected");

    auto missing = observations;
    missing.pop_back();
    Require(
        !EvaluateProfile(profile, windows, missing, L"C:\\Windows").supported,
        "missing required module rejected");

    auto duplicate = observations;
    duplicate.push_back(observations.front());
    Require(
        !EvaluateProfile(profile, windows, duplicate, L"C:\\Windows").supported,
        "ambiguous duplicate module rejected");

    auto invalid_profile = profile;
    invalid_profile.modules[0].path_relative_to_windows =
        L"C:\\Windows\\explorer.exe";
    Require(
        !EvaluateProfile(
             invalid_profile,
             windows,
             observations,
             L"C:\\Windows")
             .supported,
        "absolute profile module path rejected");

    auto current_windows = QueryWindowsVersion();
    Require(current_windows.ok(), "query true Windows version");
    Require(current_windows.value().major >= 10, "current major version plausible");
    Require(current_windows.value().build != 0, "current build is present");

    Require(
        AdapterName(AdapterId::start_menu_xaml) == "start-menu-xaml",
        "adapter diagnostic name");

    auto encoded_pack = ProfilePack({ProfileRecord("external-test-profile")});
    auto parsed_pack = ParseProfilePack(encoded_pack);
    Require(parsed_pack.ok(), "strict external profile pack parsed");
    Require(parsed_pack.value().size() == 1, "one external profile decoded");
    Require(
        parsed_pack.value().front().id == "external-test-profile",
        "external profile id decoded");
    Require(
        parsed_pack.value().front().modules.front().module_name == L"user32.dll",
        "external module decoded");

    auto wrong_magic = encoded_pack;
    wrong_magic.front() = std::byte{0};
    Require(!ParseProfilePack(wrong_magic).ok(), "pack magic mismatch rejected");

    auto truncated_pack = encoded_pack;
    truncated_pack.pop_back();
    Require(!ParseProfilePack(truncated_pack).ok(), "truncated pack rejected");

    auto traversal_pack = ProfilePack(
        {ProfileRecord("traversal-test", "..\\user32.dll")});
    Require(
        !ParseProfilePack(traversal_pack).ok(),
        "relative path traversal rejected");

    auto alternate_data_stream_pack = ProfilePack(
        {ProfileRecord("ads-test", "System32\\user32.dll:payload")});
    Require(
        !ParseProfilePack(alternate_data_stream_pack).ok(),
        "alternate data stream path rejected");

    auto wildcard_pack = ProfilePack(
        {ProfileRecord("wildcard-test", "System32\\user*.dll")});
    Require(
        !ParseProfilePack(wildcard_pack).ok(),
        "wildcard module path rejected");

    auto traversal_profile = profile;
    traversal_profile.modules[0].path_relative_to_windows =
        L"..\\explorer.exe";
    Require(
        !EvaluateProfile(
             traversal_profile,
             windows,
             observations,
             L"C:\\Windows")
             .supported,
        "in-memory profile traversal rejected");

    auto duplicate_pack = ProfilePack(
        {ProfileRecord("first-profile"), ProfileRecord("second-profile")});
    Require(
        !ParseProfilePack(duplicate_pack).ok(),
        "duplicate adapter/version profile rejected");

    metaplasia::trust::PublisherThumbprint no_publisher{};
    auto absent_pack = LoadExternalProfilePack(
        L"C:\\definitely-missing-metaplasia-compatibility-pack.dll",
        no_publisher,
        true);
    Require(
        absent_pack.ok() && !absent_pack.value().present,
        "absent optional pack reported without weakening compiled profiles");

    const auto fixture_path =
        std::filesystem::absolute(std::filesystem::path(argv[1]));
    auto production_fixture = LoadExternalProfilePack(
        fixture_path,
        no_publisher,
        false);
    Require(
        !production_fixture.ok(),
        "unsigned resource pack rejected by production policy");

    auto development_fixture = LoadExternalProfilePack(
        fixture_path,
        no_publisher,
        true);
    Require(
        development_fixture.ok() && development_fixture.value().present &&
            development_fixture.value().loaded &&
            development_fixture.value().profile_count == 1,
        "unsigned resource pack loaded only by explicit development policy");

    auto duplicate_initialization = LoadExternalProfilePack(
        fixture_path,
        no_publisher,
        true);
    Require(
        !duplicate_initialization.ok(),
        "external profile catalog initialization is one-shot");
    std::cout << "Compatibility catalog tests passed\n";
    return EXIT_SUCCESS;
}
