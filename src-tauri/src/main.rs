#![windows_subsystem = "windows"]

fn main() {
    if let Some(exit_code) = metaplasia_ui::run_portable_update_helper_from_args() {
        std::process::exit(exit_code);
    }
    if let Some(exit_code) = metaplasia_ui::run_start_all_apps_policy_helper_from_args() {
        std::process::exit(exit_code);
    }
    metaplasia_ui::run();
}
