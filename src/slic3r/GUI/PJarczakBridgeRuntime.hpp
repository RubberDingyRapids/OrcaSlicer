#pragma once

#include <string>
#include <boost/filesystem/path.hpp>

namespace Slic3r { namespace GUI {

// Linux plug-in bridge: WSL2 runtime helpers used by GUI_App at start-up.

// Runs verify_runtime.cmd from the plugins folder and, when it fails, install_runtime.cmd,
// then verifies again. Output goes to the log.
void pjarczak_verify_or_install_windows_bridge_runtime(const boost::filesystem::path& plugin_folder,
                                                       const boost::filesystem::path& plugin_cache_dir);

// True when every file the forwarder module needs is present in the plugins folder.
bool pjarczak_bridge_payload_ready(const boost::filesystem::path& plugin_folder, std::string* reason);

// Copies the runtime files installed next to the executable into the plugins folder.
void pjarczak_seed_plugins_folder_from_bundle(const boost::filesystem::path& plugin_folder);

}} // namespace Slic3r::GUI
