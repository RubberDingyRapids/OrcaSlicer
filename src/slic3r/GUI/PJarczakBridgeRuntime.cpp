#include "PJarczakBridgeRuntime.hpp"

#include "slic3r/Utils/PJarczakLinuxBridge/PJarczakLinuxBridgeConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/GUI.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <wx/string.h>
#include <wx/arrstr.h>
#include <wx/utils.h>
#include <wx/stdpaths.h>

namespace Slic3r { namespace GUI {

namespace {

void set_reason(std::string* reason, std::string value)
{
    if (reason)
        *reason = std::move(value);
}

wxString quote_windows_arg(const wxString& value)
{
    wxString escaped = value;
    escaped.Replace("\"", "\\\"");
    return wxString::Format("\"%s\"", escaped);
}

long run_hidden_windows_command(const wxString& command, wxArrayString* stdout_lines, wxArrayString* stderr_lines)
{
#ifdef WIN32
    wxArrayString local_stdout;
    wxArrayString local_stderr;
    long exit_code = wxExecute(command, local_stdout, local_stderr, wxEXEC_SYNC | wxEXEC_HIDE_CONSOLE);
    if (stdout_lines)
        *stdout_lines = local_stdout;
    if (stderr_lines)
        *stderr_lines = local_stderr;
    return exit_code;
#else
    (void)command;
    (void)stdout_lines;
    (void)stderr_lines;
    return -1;
#endif
}

void log_command_output(const char* tag, long exit_code, const wxArrayString& stdout_lines, const wxArrayString& stderr_lines)
{
    BOOST_LOG_TRIVIAL(info) << tag << ": exit_code=" << exit_code;
    for (const auto& line : stdout_lines)
        BOOST_LOG_TRIVIAL(info) << tag << " [stdout] " << into_u8(line);
    for (const auto& line : stderr_lines)
        BOOST_LOG_TRIVIAL(error) << tag << " [stderr] " << into_u8(line);
}

} // namespace

void pjarczak_verify_or_install_windows_bridge_runtime(const boost::filesystem::path& plugin_folder,
                                                       const boost::filesystem::path& plugin_cache_dir)
{
#ifndef WIN32
    (void)plugin_folder;
    (void)plugin_cache_dir;
#else
    if (!Slic3r::PJarczakLinuxBridge::enabled())
        return;

    const auto verify_cmd_file  = plugin_folder / "verify_runtime.cmd";
    const auto install_cmd_file = plugin_folder / "install_runtime.cmd";
    if (!boost::filesystem::exists(verify_cmd_file) || !boost::filesystem::exists(install_cmd_file)) {
        BOOST_LOG_TRIVIAL(warning) << "[pjarczak_runtime_setup] missing verify/install wrapper in " << plugin_folder.string();
        return;
    }

    const wxString plugin_dir_wx = from_u8(plugin_folder.string());
    const wxString cache_dir_wx  = from_u8(plugin_cache_dir.string());
    const wxString verify_cmd    = wxString::Format(
        "cmd.exe /c %s -PackageDir %s -PluginCacheDir %s -AllowMissingLinuxPlugin",
        quote_windows_arg(from_u8(verify_cmd_file.string())),
        quote_windows_arg(plugin_dir_wx),
        quote_windows_arg(cache_dir_wx));

    wxArrayString verify_out;
    wxArrayString verify_err;
    long verify_code = run_hidden_windows_command(verify_cmd, &verify_out, &verify_err);
    log_command_output("[pjarczak_runtime_verify]", verify_code, verify_out, verify_err);
    if (verify_code == 0)
        return;

    const wxString install_cmd = wxString::Format(
        "cmd.exe /c %s -PackageDir %s -PluginDir %s -PluginCacheDir %s",
        quote_windows_arg(from_u8(install_cmd_file.string())),
        quote_windows_arg(plugin_dir_wx),
        quote_windows_arg(plugin_dir_wx),
        quote_windows_arg(cache_dir_wx));

    wxArrayString install_out;
    wxArrayString install_err;
    long install_code = run_hidden_windows_command(install_cmd, &install_out, &install_err);
    log_command_output("[pjarczak_runtime_install]", install_code, install_out, install_err);

    verify_out.clear();
    verify_err.clear();
    verify_code = run_hidden_windows_command(verify_cmd, &verify_out, &verify_err);
    log_command_output("[pjarczak_runtime_verify_after_install]", verify_code, verify_out, verify_err);
#endif
}

bool pjarczak_bridge_payload_ready(const boost::filesystem::path& plugin_folder, std::string* reason)
{
    if (!Slic3r::PJarczakLinuxBridge::enabled()) {
        set_reason(reason, "bridge disabled");
        return true;
    }

    const auto has_file = [&plugin_folder](const std::string& file_name) {
        const auto candidate = plugin_folder / file_name;
        return boost::filesystem::exists(candidate) && !boost::filesystem::is_directory(candidate);
    };

    const std::string required_files[] = {
        Slic3r::PJarczakLinuxBridge::bridge_network_current_dir_name(),
        Slic3r::PJarczakLinuxBridge::host_executable_file_name(),
        std::string("pjarczak_bambu_linux_host_abi1"),
        std::string("pjarczak_bambu_linux_host_abi0"),
        Slic3r::PJarczakLinuxBridge::windows_wsl_distro_file_name(),
        Slic3r::PJarczakLinuxBridge::windows_wsl_import_script_file_name(),
        Slic3r::PJarczakLinuxBridge::windows_wsl_validate_script_file_name(),
        Slic3r::PJarczakLinuxBridge::windows_wsl_rootfs_file_name(),
        Slic3r::PJarczakLinuxBridge::windows_plugin_cache_subdir_file_name(),
        Slic3r::PJarczakLinuxBridge::linux_network_library_name(),
        Slic3r::PJarczakLinuxBridge::linux_source_library_name(),
        std::string("ca-certificates.crt"),
        std::string("slicer_base64.cer")
    };

    for (const std::string& file_name : required_files) {
        if (!has_file(file_name)) {
            set_reason(reason, "missing required runtime file: " + file_name);
            return false;
        }
    }
    if (!has_file(Slic3r::PJarczakLinuxBridge::windows_wsl_bootstrap_script_file_name()) &&
        !has_file("pjarczak-wsl-run-host.sh")) {
        set_reason(reason, "missing required runtime file: " + Slic3r::PJarczakLinuxBridge::windows_wsl_bootstrap_script_file_name());
        return false;
    }

    for (const std::string& file_name : {
            Slic3r::PJarczakLinuxBridge::linux_network_library_name(),
            Slic3r::PJarczakLinuxBridge::linux_source_library_name()}) {
        std::string validate_reason;
        if (!Slic3r::PJarczakLinuxBridge::validate_linux_so_binary((plugin_folder / file_name).string(), &validate_reason)) {
            set_reason(reason, file_name + ": " + validate_reason);
            return false;
        }
    }

    const auto manifest_path = plugin_folder / Slic3r::PJarczakLinuxBridge::linux_payload_manifest_file_name();
    if (boost::filesystem::exists(manifest_path) && !boost::filesystem::is_directory(manifest_path)) {
        std::string manifest_reason;
        if (!Slic3r::PJarczakLinuxBridge::validate_linux_payload_set_against_manifest(plugin_folder, &manifest_reason)) {
            set_reason(reason, "linux payload manifest validation failed: " + manifest_reason);
            return false;
        }
    }

    set_reason(reason, "ok");
    return true;
}

namespace {

void copy_runtime_file_if_exists(const boost::filesystem::path& src_dir,
                                 const boost::filesystem::path& dst_dir,
                                 const std::string& file_name)
{
    if (file_name.empty())
        return;

    const auto src = src_dir / file_name;
    const auto dst = dst_dir / file_name;

    if (!boost::filesystem::exists(src) || boost::filesystem::is_directory(src))
        return;

    boost::system::error_code ec;
    boost::filesystem::create_directories(dst.parent_path(), ec);

    std::string error_message;
    CopyFileResult cfr = copy_file(src.string(), dst.string(), error_message, false);
    if (cfr != CopyFileResult::SUCCESS) {
        BOOST_LOG_TRIVIAL(error) << "[pjarczak_runtime_setup] copy runtime file failed: "
                                 << src.string() << " -> " << dst.string()
                                 << ", code=" << cfr << ", err=" << error_message;
    }
}

} // namespace

void pjarczak_seed_plugins_folder_from_bundle(const boost::filesystem::path& plugin_folder)
{
    if (!Slic3r::PJarczakLinuxBridge::enabled())
        return;

    if (!boost::filesystem::exists(plugin_folder)) {
        boost::system::error_code ec;
        boost::filesystem::create_directories(plugin_folder, ec);
    }

    const boost::filesystem::path exe_path(into_u8(wxStandardPaths::Get().GetExecutablePath()));
    const boost::filesystem::path exe_dir = exe_path.parent_path();

    const std::string helper_files[] = {
        Slic3r::PJarczakLinuxBridge::windows_wsl_distro_file_name(),
        Slic3r::PJarczakLinuxBridge::windows_wsl_import_script_file_name(),
        Slic3r::PJarczakLinuxBridge::windows_wsl_validate_script_file_name(),
        Slic3r::PJarczakLinuxBridge::windows_wsl_bootstrap_script_file_name(),
        "pjarczak-wsl-run-host.sh",
        Slic3r::PJarczakLinuxBridge::windows_wsl_rootfs_file_name(),
        Slic3r::PJarczakLinuxBridge::windows_plugin_cache_subdir_file_name(),
        "install_runtime.cmd",
        "verify_runtime.cmd",
        "assemble_windows_runtime_bundle.ps1"
    };

    const boost::filesystem::path candidate_dirs[] = {
        exe_dir,
        exe_dir / "plugins"
    };

    for (const auto& candidate_dir : candidate_dirs) {
        if (!boost::filesystem::exists(candidate_dir) || !boost::filesystem::is_directory(candidate_dir))
            continue;

        for (const std::string& file_name : helper_files)
            copy_runtime_file_if_exists(candidate_dir, plugin_folder, file_name);

        try {
            for (auto& dir_entry : boost::filesystem::directory_iterator(candidate_dir)) {
                if (!boost::filesystem::is_regular_file(dir_entry.path()))
                    continue;
                const std::string file_name = dir_entry.path().filename().string();
                if (!Slic3r::PJarczakLinuxBridge::is_overlay_runtime_filename(file_name))
                    continue;
                copy_runtime_file_if_exists(candidate_dir, plugin_folder, file_name);
            }
        } catch (...) {}
    }

    const auto runtime_dst_dir = plugin_folder / "pjarczak_bambu_linux_host.runtime";
    if (boost::filesystem::exists(runtime_dst_dir) && boost::filesystem::is_directory(runtime_dst_dir)) {
        try {
            boost::filesystem::remove_all(runtime_dst_dir);
        } catch (...) {}
    }
}

}} // namespace Slic3r::GUI
