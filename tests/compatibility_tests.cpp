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

void TestLocal26300Profiles() {
    using namespace metaplasia::compatibility;
    const WindowsVersion windows{10, 0, 26300, 9711};
    // Independent capture of the loaded host images, 2026-10-29. These are
    // fixtures, not observations synthesized from the catalog being tested.
    const ModuleObservation explorer{
        L"Explorer.EXE", L"C:\\Windows\\Explorer.EXE",
        "8664-BEE7E13F-00344000-00359899-explorer.pdb-1D666F27BA2F85885888D3E2B94B0FD81"};
    const ModuleObservation user32{
        L"user32.dll", L"C:\\Windows\\System32\\user32.dll",
        "8664-1D1C21A9-001D7000-001E61E2-user32.pdb-F0BF56EE0A784974D49A58173A7CBC7E1"};
    struct Fixture final {
        AdapterId adapter;
        std::string id;
        std::vector<ModuleObservation> modules;
    };
    const std::vector<Fixture> fixtures{
        {AdapterId::taskbar_clock,
         "win11-26h2-26300.9711-x64-taskbar-local-v1",
         {explorer, user32,
          {L"Taskbar.dll", L"C:\\Windows\\System32\\Taskbar.dll",
           "8664-769B877B-00308000-003142B7-Taskbar.pdb-DAB69A79FEE033F0D81E023FBF172B511"},
          {L"Taskbar.View.dll",
           L"C:\\Windows\\SystemApps\\MicrosoftWindows.Client.Core_cw5n1h2txyewy\\Taskbar.View.dll",
           "8664-6AA3487A-00997000-00997AD8-Taskbar.View.pdb-24534974E00E46A58F69F60D0CEB9BD91"},
          {L"twinui.pcshell.dll", L"C:\\Windows\\System32\\twinui.pcshell.dll",
           "8664-BC86BB4D-009A5000-009A2204-twinui.pcshell.pdb-9507F3DAD47F5F026569FC50273C40D71"}}},
        {AdapterId::file_explorer_title,
         "win11-26h2-26300.9711-x64-explorer-local-v2",
         {explorer, user32,
          {L"dwmapi.dll", L"C:\\Windows\\System32\\dwmapi.dll",
           "8664-B15B6B4A-0002E000-00030704-dwmapi.pdb-03449B4864BEFF0E155BEB1845861AF71"},
          {L"Microsoft.Internal.FrameworkUdk.dll",
           L"C:\\Windows\\SystemApps\\Microsoft.WindowsAppRuntime.CBS.2_8wekyb3d8bbwe\\Microsoft.Internal.FrameworkUdk.dll",
           "8664-1E55A7F2-000E2000-000ED768-Microsoft.Internal.FrameworkUdk.pdb-C6961B993EC4FC97EB658F91BAC6278F1"}}},
        {AdapterId::start_menu_xaml,
         "win11-26h2-26300.9711-x64-start-xaml-local-v1",
         {{L"StartMenuExperienceHost.exe",
           L"C:\\Windows\\SystemApps\\Microsoft.Windows.StartMenuExperienceHost_cw5n1h2txyewy\\StartMenuExperienceHost.exe",
           "8664-14A98365-00038000-0003E450-startmenuexperiencehost.pdb-1A3D3C8E9F91E211D88A10EFAB3BBBD31"},
          {L"Windows.UI.Xaml.dll", L"C:\\Windows\\System32\\Windows.UI.Xaml.dll",
           "8664-27807062-01121000-01121BDF-windows.ui.xaml.pdb-E15A88ED9599B347A0D2576C13287FC41"},
          {L"StartDocked.dll",
           L"C:\\Windows\\SystemApps\\Microsoft.Windows.StartMenuExperienceHost_cw5n1h2txyewy\\StartDocked.dll",
           "8664-E540DF64-0041D000-0041D269-StartDocked.pdb-65DDC4DBDAB3EC86831571146496637C1"},
          {L"StartMenu.dll",
           L"C:\\Windows\\SystemApps\\MicrosoftWindows.Client.Core_cw5n1h2txyewy\\StartMenu.dll",
           "8664-6A8F587A-004DD000-004E34F8-StartMenu.pdb-932E8BA3FA3A41429498E5F6426A43E61"},
          {L"Windows.UI.Xaml.Controls.dll",
           L"C:\\Windows\\System32\\Windows.UI.Xaml.Controls.dll",
           "8664-B084F599-00329000-0032BD5D-Windows.UI.Xaml.Controls.pdb-559C62626C4A21F93930D93D3A98AF741"}}},
    };
    for (const auto& fixture : fixtures) {
        const auto selected = FindProfile(fixture.adapter, windows);
        Require(selected.ok(), "local profile selection is unambiguous");
#ifdef NDEBUG
        Require(!selected.value().has_value(), "uncertified local profile excluded from Release");
#else
        Require(selected.value().has_value(), "exact local Debug profile exists");
        const auto& profile = selected.value().value();
        Require(profile.id == fixture.id, "exact local profile identity retained");
        Require(profile.modules.size() == fixture.modules.size(), "all captured dependencies required");
        Require(EvaluateProfile(profile, windows, fixture.modules, L"C:\\Windows").supported,
                "captured mapped images match the local profile");
        for (std::size_t index = 0; index < fixture.modules.size(); ++index) {
            auto changed = fixture.modules;
            changed[index].compatibility_key += "-different";
            Require(!EvaluateProfile(profile, windows, changed, L"C:\\Windows").supported,
                    "every local dependency identity is enforced");
            changed = fixture.modules;
            changed[index].path = L"C:\\Temp" / changed[index].path.filename();
            Require(!EvaluateProfile(profile, windows, changed, L"C:\\Windows").supported,
                    "every local dependency path is enforced");
            changed = fixture.modules;
            changed.erase(changed.begin() + static_cast<std::ptrdiff_t>(index));
            Require(!EvaluateProfile(profile, windows, changed, L"C:\\Windows").supported,
                    "every local dependency must be loaded");
            changed = fixture.modules;
            changed.push_back(changed[index]);
            Require(!EvaluateProfile(profile, windows, changed, L"C:\\Windows").supported,
                    "every ambiguous local dependency is rejected");
        }
        if (fixture.adapter == AdapterId::file_explorer_title) {
            auto old_path = fixture.modules;
            old_path.back().path =
                L"C:\\Windows\\SystemApps\\Microsoft.WindowsAppRuntime.CBS_8wekyb3d8bbwe\\Microsoft.Internal.FrameworkUdk.dll";
            Require(!EvaluateProfile(profile, windows, old_path, L"C:\\Windows").supported,
                    "legacy CBS location cannot satisfy the CBS.2 profile");
        }
#endif
        for (const auto other : {WindowsVersion{10, 0, 26300, 9710},
                                 WindowsVersion{10, 0, 26300, 9712},
                                 WindowsVersion{10, 0, 26200, 9711}}) {
            const auto missing = FindProfile(fixture.adapter, other);
            Require(missing.ok() && !missing.value().has_value(),
                    "local profile never approves a neighboring build or revision");
        }
    }
}

}  // namespace

int main(const int argc, char** argv) {
    using namespace metaplasia::compatibility;

    Require(argc == 2, "resource-only compatibility fixture path supplied");
    TestLocal26300Profiles();

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
