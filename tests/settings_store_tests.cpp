#include "metaplasia/host/settings_store.hpp"

#include <Windows.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>
#include <system_error>

namespace {

void Require(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        std::error_code error;
        const auto root = std::filesystem::temp_directory_path(error);
        Require(!error, "resolve temporary directory");

        for (unsigned int attempt = 0; attempt < 32; ++attempt) {
            path_ = root /
                    (L"Metaplasia.SettingsTests." +
                     std::to_wstring(::GetCurrentProcessId()) + L"." +
                     std::to_wstring(::GetTickCount64()) + L"." +
                     std::to_wstring(attempt));
            if (std::filesystem::create_directory(path_, error)) {
                return;
            }
            Require(
                !error || error == std::errc::file_exists,
                "create temporary directory");
            error.clear();
        }
        Require(false, "allocate unique temporary directory");
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void WriteText(
    const std::filesystem::path& path,
    const std::string_view contents) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    Require(stream.good(), "open test settings file");
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    Require(stream.good(), "write test settings file");
}

}  // namespace

int main() {
    using metaplasia::ErrorCode;
    using metaplasia::host::HostSettings;
    using metaplasia::host::SettingsStore;

    TemporaryDirectory temporary;
    const auto settings_path = temporary.path() / L"nested" / L"settings.conf";
    SettingsStore store(settings_path);

    const auto missing = store.Load();
    Require(missing.ok(), "missing settings use safe defaults");
    Require(!missing.value().taskbar_enabled, "taskbar defaults disabled");
    Require(
        !missing.value().file_explorer_enabled,
        "file explorer defaults disabled");
    Require(!missing.value().start_menu_enabled, "start menu defaults disabled");

    HostSettings expected;
    expected.taskbar_enabled = true;
    expected.start_menu_enabled = true;
    expected.taskbar_clock_prefix = "Clock ";
    expected.taskbar_opacity_milli = 780;
    expected.taskbar_hide_notification_center = true;
    expected.taskbar_hide_show_desktop = true;
    expected.taskbar_capsule_enabled = true;
    expected.taskbar_background_color_enabled = true;
    expected.taskbar_background_color = 0xFF112233U;
    expected.file_explorer_title_prefix = "Files \xC2\xB7 ";
    expected.file_explorer_background_color_enabled = true;
    expected.file_explorer_background_color = 0xFF445566U;
    expected.file_explorer_transition_animation =
        metaplasia::protocol::kExplorerTransitionScaleFade;
    expected.file_explorer_custom_scrollbar_enabled = false;
    expected.start_menu_opacity_milli = 730;
    expected.start_menu_hide_recommended = true;
    expected.start_menu_background_color_enabled = true;
    expected.start_menu_background_color = 0xFF778899U;
    expected.start_menu_three_panel_layout_enabled = true;
    expected.start_menu_hide_all_apps = true;
    const auto saved = store.Save(expected);
    Require(saved.ok(), "atomically save settings");

    const auto loaded = store.Load();
    Require(loaded.ok(), "load saved settings");
    Require(loaded.value().taskbar_enabled, "round-trip taskbar");
    Require(!loaded.value().file_explorer_enabled, "round-trip explorer");
    Require(loaded.value().start_menu_enabled, "round-trip start menu");
    Require(
        loaded.value().taskbar_clock_prefix == "Clock ",
        "round-trip taskbar prefix");
    Require(
        loaded.value().taskbar_opacity_milli == 780,
        "round-trip Taskbar opacity");
    Require(
        loaded.value().taskbar_hide_notification_center &&
            !loaded.value().taskbar_hide_control_center &&
            loaded.value().taskbar_hide_show_desktop,
        "round-trip Taskbar visibility rules");
    Require(
        loaded.value().taskbar_capsule_enabled,
        "round-trip Taskbar capsule setting");
    Require(
        loaded.value().taskbar_background_color_enabled &&
            loaded.value().taskbar_background_color == 0xFF112233U,
        "round-trip Taskbar background color");
    Require(
        loaded.value().file_explorer_title_prefix == "Files \xC2\xB7 ",
        "round-trip explorer prefix");
    Require(
        loaded.value().file_explorer_background_color_enabled &&
            loaded.value().file_explorer_background_color == 0xFF445566U,
        "round-trip Explorer background color");
    Require(
        loaded.value().file_explorer_transition_animation ==
            metaplasia::protocol::kExplorerTransitionScaleFade,
        "round-trip Explorer transition animation");
    Require(
        !loaded.value().file_explorer_custom_scrollbar_enabled,
        "round-trip Explorer custom scrollbar");
    Require(
        loaded.value().start_menu_opacity_milli == 730,
        "round-trip Start menu opacity");
    Require(
        loaded.value().start_menu_hide_recommended,
        "round-trip Start menu visibility rule");
    Require(
        loaded.value().start_menu_background_color_enabled &&
            loaded.value().start_menu_background_color == 0xFF778899U,
        "round-trip Start menu background color");
    Require(
        loaded.value().start_menu_three_panel_layout_enabled,
        "round-trip three-panel Start layout");
    Require(
        loaded.value().start_menu_hide_all_apps,
        "round-trip three-panel All apps visibility");
    for (const auto& entry :
         std::filesystem::directory_iterator(settings_path.parent_path())) {
        Require(
            entry.path().filename().native().find(L"settings.conf.tmp.") != 0,
            "temporary settings file was replaced");
    }

    WriteText(
        settings_path,
        "version=1\n"
        "taskbar_enabled=1\n"
        "taskbar_enabled=0\n");
    const auto duplicate = store.Load();
    Require(!duplicate.ok(), "duplicate key rejected");
    Require(
        duplicate.status().code() == ErrorCode::invalid_data,
        "duplicate key reports invalid data");

    WriteText(settings_path, "version=10\ntaskbar_enabled=1\n");
    const auto future_version = store.Load();
    Require(!future_version.ok(), "unsupported version rejected");
    Require(
        future_version.status().code() == ErrorCode::incompatible,
        "unsupported version reports incompatible");

    WriteText(settings_path, "version=4\nfile_explorer_enabled=1\n");
    const auto migrated_animation = store.Load();
    Require(migrated_animation.ok(), "version 4 animation migration");
    Require(
        migrated_animation.value().file_explorer_transition_animation ==
            metaplasia::protocol::kDefaultExplorerTransition,
        "version 4 migrates to the Fade animation");

    WriteText(settings_path, "version=5\nfile_explorer_enabled=1\n");
    const auto migrated_scrollbar = store.Load();
    Require(migrated_scrollbar.ok(), "version 5 scrollbar migration");
    Require(
        migrated_scrollbar.value().file_explorer_custom_scrollbar_enabled,
        "version 5 preserves the custom Explorer scrollbar");

    WriteText(settings_path, "version=6\ntaskbar_enabled=1\n");
    const auto migrated_capsule = store.Load();
    Require(migrated_capsule.ok(), "version 6 capsule migration");
    Require(
        !migrated_capsule.value().taskbar_capsule_enabled,
        "version 6 migrates to the native Taskbar shape");

    WriteText(settings_path, "version=7\nstart_menu_enabled=1\n");
    const auto migrated_three_panel = store.Load();
    Require(migrated_three_panel.ok(), "version 7 Start layout migration");
    Require(
        !migrated_three_panel.value().start_menu_three_panel_layout_enabled,
        "version 7 migrates to the native Start layout");

    WriteText(settings_path, "version=8\nstart_menu_enabled=1\n");
    const auto migrated_all_apps = store.Load();
    Require(migrated_all_apps.ok(), "version 8 All apps migration");
    Require(
        !migrated_all_apps.value().start_menu_hide_all_apps,
        "version 8 keeps the three-panel All apps list visible");

    WriteText(settings_path, "version=1\nstart_menu_enabled=yes\n");
    Require(!store.Load().ok(), "non-canonical boolean rejected");

    WriteText(
        settings_path,
        "version=1\n"
        "unknown_future_key=42\n"
        "file_explorer_enabled=true\n");
    const auto forward_compatible = store.Load();
    Require(forward_compatible.ok(), "unknown key ignored");
    Require(
        forward_compatible.value().file_explorer_enabled,
        "known key remains readable after unknown key");

    Require(
        forward_compatible.value().taskbar_clock_prefix == "M ",
        "version 1 migrates taskbar prefix default");
    Require(
        forward_compatible.value().file_explorer_title_prefix ==
            "Meta \xC2\xB7 ",
        "version 1 migrates Explorer prefix default");
    Require(
        forward_compatible.value().start_menu_opacity_milli == 940,
        "version 1 migrates opacity default");
    Require(
        !forward_compatible.value().start_menu_hide_recommended,
        "version 1 migrates visibility rule default");

    WriteText(
        settings_path,
        "version=2\n"
        "taskbar_enabled=0\n"
        "file_explorer_enabled=0\n"
        "start_menu_enabled=0\n"
        "taskbar_clock_prefix_utf8_hex=4D20\n"
        "file_explorer_title_prefix_utf8_hex=4D65746120C2B720\n"
        "start_menu_opacity_milli=875\n"
        "start_menu_hide_recommended=1\n");
    const auto version_two = store.Load();
    Require(version_two.ok(), "load version 2 settings");
    Require(
        version_two.value().file_explorer_title_prefix == "Meta \xC2\xB7 ",
        "decode UTF-8 hex setting");
    Require(
        version_two.value().start_menu_opacity_milli == 875,
        "load version 2 opacity");
    Require(
        version_two.value().start_menu_hide_recommended,
        "load version 2 visibility rule");
    Require(
        version_two.value().taskbar_opacity_milli == 1000 &&
            !version_two.value().taskbar_hide_notification_center &&
            !version_two.value().taskbar_hide_control_center &&
            !version_two.value().taskbar_hide_show_desktop,
        "version 2 migrates neutral Taskbar XAML defaults");

    WriteText(
        settings_path,
        "version=3\n"
        "taskbar_enabled=0\n"
        "file_explorer_enabled=0\n"
        "start_menu_enabled=0\n"
        "taskbar_clock_prefix_utf8_hex=4D20\n"
        "taskbar_opacity_milli=665\n"
        "taskbar_hide_notification_center=1\n"
        "taskbar_hide_control_center=1\n"
        "taskbar_hide_show_desktop=0\n"
        "file_explorer_title_prefix_utf8_hex=4D65746120C2B720\n"
        "start_menu_opacity_milli=940\n"
        "start_menu_hide_recommended=0\n");
    const auto version_three = store.Load();
    Require(version_three.ok(), "load version 3 settings");
    Require(
        version_three.value().taskbar_opacity_milli == 665 &&
            version_three.value().taskbar_hide_notification_center &&
            version_three.value().taskbar_hide_control_center &&
            !version_three.value().taskbar_hide_show_desktop,
        "load version 3 Taskbar XAML settings");

    WriteText(
        settings_path,
        "version=2\n"
        "taskbar_clock_prefix_utf8_hex=0G\n");
    Require(!store.Load().ok(), "invalid hexadecimal setting rejected");

    WriteText(
        settings_path,
        "version=2\n"
        "taskbar_clock_prefix_utf8_hex=C328\n");
    Require(!store.Load().ok(), "invalid UTF-8 setting rejected");

    WriteText(
        settings_path,
        "version=2\n"
        "start_menu_opacity_milli=99\n");
    Require(!store.Load().ok(), "out-of-range persisted opacity rejected");

    std::cout << "Settings store tests passed\n";
    return EXIT_SUCCESS;
}
