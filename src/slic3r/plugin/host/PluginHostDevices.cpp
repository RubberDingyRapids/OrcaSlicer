#include "PluginHostBindings.hpp"
#include <pybind11/pybind11.h>

#include <slic3r/GUI/DeviceCore/DevManager.h>
#include <slic3r/GUI/DeviceManager.hpp>
#include <slic3r/GUI/GUI_App.hpp>

#include <cstdint>
#include <exception>
#include <future>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>
#include <wx/app.h>
#include <wx/thread.h>

namespace py = pybind11;

namespace Slic3r {
namespace {

// MachineObject and DeviceManager are owned by the UI thread. Every read below is
// marshalled there and blocks the caller with the GIL released, the same pattern
// PluginHostUi.cpp uses for dialogs. Plain structs cross back; Python objects are
// only built once the GIL is held again.
template<typename Fn>
auto on_ui_thread(Fn&& fn) -> std::invoke_result_t<Fn&>
{
    using R = std::invoke_result_t<Fn&>;
    if (wxTheApp == nullptr)
        throw std::runtime_error("OrcaSlicer application is not initialized");

    if (wxIsMainThread()) {
        py::gil_scoped_release nogil;
        return fn();
    }

    std::promise<R> prom;
    std::future<R>  fut = prom.get_future();

    py::gil_scoped_release nogil;
    GUI::wxGetApp().CallAfter([&prom, &fn]() {
        try {
            if constexpr (std::is_void_v<R>) {
                fn();
                prom.set_value();
            } else {
                prom.set_value(fn());
            }
        } catch (...) {
            prom.set_exception(std::current_exception());
        }
    });
    return fut.get();
}

struct DeviceInfo
{
    std::string dev_id;
    std::string dev_name;
    std::string printer_type;
    bool        is_connected{false};
    bool        is_online{false};
    bool        is_selected{false};
};

// nullptr before the network plugin is loaded or a device list exists.
DeviceManager* device_manager()
{
    return GUI::wxGetApp().getDeviceManager();
}

MachineObject* find_machine(const std::string& dev_id)
{
    DeviceManager* dm = device_manager();
    if (dm == nullptr)
        return nullptr;
    if (MachineObject* m = dm->get_my_machine(dev_id); m != nullptr)
        return m;
    if (MachineObject* m = dm->get_local_machine(dev_id); m != nullptr)
        return m;
    return nullptr;
}

std::vector<DeviceInfo> collect_devices()
{
    std::vector<DeviceInfo> out;
    DeviceManager* dm = device_manager();
    if (dm == nullptr)
        return out;

    const MachineObject* selected = dm->get_selected_machine();
    const std::string selected_id = selected != nullptr ? selected->get_dev_id() : std::string();

    for (const auto& [dev_id, machine] : dm->get_my_machine_list()) {
        if (machine == nullptr)
            continue;
        DeviceInfo info;
        info.dev_id       = dev_id;
        info.dev_name     = machine->get_dev_name();
        info.printer_type = machine->printer_type;
        info.is_connected = machine->is_connected();
        info.is_online    = machine->is_online();
        info.is_selected  = dev_id == selected_id;
        out.push_back(std::move(info));
    }
    return out;
}

} // namespace

// Read-only view of the connected printers for plugins, so a filament or
// monitoring plugin does not have to scrape the debug log for status pushes.
//
//   orca.host.devices()               -> list of dicts (dev_id, dev_name, printer_type,
//                                        is_connected, is_online, is_selected)
//   orca.host.selected_device_id()    -> str | None
//   orca.host.device_status(dev_id)   -> str (JSON of the last merged status push) | None
//   orca.host.device_status_seq(dev_id) -> int, +1 per status push, 0 before the first
void host_bindings::register_devices(py::module_& host)
{
    host.def("devices", []() {
        const std::vector<DeviceInfo> devices = on_ui_thread([]() { return collect_devices(); });
        py::list result;
        for (const DeviceInfo& d : devices) {
            py::dict item;
            item["dev_id"]       = d.dev_id;
            item["dev_name"]     = d.dev_name;
            item["printer_type"] = d.printer_type;
            item["is_connected"] = d.is_connected;
            item["is_online"]    = d.is_online;
            item["is_selected"]  = d.is_selected;
            result.append(std::move(item));
        }
        return result;
    }, "Printers known to the device manager, as a list of dicts. Empty before the printer agent is up.");

    host.def("selected_device_id", []() -> py::object {
        const std::optional<std::string> id = on_ui_thread([]() -> std::optional<std::string> {
            DeviceManager* dm = device_manager();
            if (dm == nullptr)
                return std::nullopt;
            const MachineObject* selected = dm->get_selected_machine();
            if (selected == nullptr)
                return std::nullopt;
            return selected->get_dev_id();
        });
        return id.has_value() ? py::object(py::str(*id)) : py::object(py::none());
    }, "dev_id of the printer selected in the Device tab, or None.");

    host.def("device_status", [](const std::string& dev_id) -> py::object {
        const std::optional<std::string> payload = on_ui_thread([&dev_id]() -> std::optional<std::string> {
            const MachineObject* machine = find_machine(dev_id);
            if (machine == nullptr || machine->get_status_seq() == 0)
                return std::nullopt;
            return machine->get_last_status_payload();
        });
        return payload.has_value() ? py::object(py::str(*payload)) : py::object(py::none());
    }, py::arg("dev_id"),
    "JSON text of the last merged status push for the printer (the same document the slicer parses), or None if nothing has arrived yet.");

    host.def("device_status_seq", [](const std::string& dev_id) -> std::uint64_t {
        return on_ui_thread([&dev_id]() -> std::uint64_t {
            const MachineObject* machine = find_machine(dev_id);
            return machine == nullptr ? 0 : machine->get_status_seq();
        });
    }, py::arg("dev_id"),
    "Monotonic counter, incremented once per status push for the printer; 0 if none yet. Poll this and fetch device_status() only when it changes.");
}

} // namespace Slic3r
