#pragma once

#include <nlohmann/json.hpp>
#include "../../src/slic3r/Utils/bambu_networking.hpp"

namespace Slic3r::PJarczakLinuxBridge::JsonBridge {

inline nlohmann::json to_json(const Slic3r::PrintParams& p)
{
    return {
        {"dev_id", p.dev_id},
        {"task_name", p.task_name},
        {"project_name", p.project_name},
        {"preset_name", p.preset_name},
        {"filename", p.filename},
        {"config_filename", p.config_filename},
        {"plate_index", p.plate_index},
        {"ftp_folder", p.ftp_folder},
        {"ftp_file", p.ftp_file},
        {"ftp_file_md5", p.ftp_file_md5},
        {"nozzle_mapping", p.nozzle_mapping},
        {"ams_mapping", p.ams_mapping},
        {"ams_mapping2", p.ams_mapping2},
        {"ams_mapping_info", p.ams_mapping_info},
        {"nozzles_info", p.nozzles_info},
        {"connection_type", p.connection_type},
        {"comments", p.comments},
        {"origin_profile_id", p.origin_profile_id},
        {"stl_design_id", p.stl_design_id},
        {"origin_model_id", p.origin_model_id},
        {"print_type", p.print_type},
        {"dst_file", p.dst_file},
        {"dev_name", p.dev_name},
        {"dev_ip", p.dev_ip},
        {"use_ssl_for_ftp", p.use_ssl_for_ftp},
        {"use_ssl_for_mqtt", p.use_ssl_for_mqtt},
        {"username", p.username},
        {"password", p.password},
        {"task_bed_leveling", p.task_bed_leveling},
        {"task_flow_cali", p.task_flow_cali},
        {"task_vibration_cali", p.task_vibration_cali},
        {"task_layer_inspect", p.task_layer_inspect},
        {"task_record_timelapse", p.task_record_timelapse},
        {"task_timelapse_use_internal", p.task_timelapse_use_internal},
        {"task_use_ams", p.task_use_ams},
        {"task_bed_type", p.task_bed_type},
        {"extra_options", p.extra_options},
        {"auto_bed_leveling", p.auto_bed_leveling},
        {"auto_flow_cali", p.auto_flow_cali},
        {"auto_offset_cali", p.auto_offset_cali},
        {"extruder_cali_manual_mode", p.extruder_cali_manual_mode},
        {"task_ext_change_assist", p.task_ext_change_assist},
        {"try_emmc_print", p.try_emmc_print},
        {"svc_context", p.svc_context},
        {"slicer_uid", p.slicer_uid}
    };
}

inline Slic3r::PrintParams print_params_from_json(const nlohmann::json& j)
{
    Slic3r::PrintParams p{};
    p.dev_id = j.value("dev_id", std::string());
    p.task_name = j.value("task_name", std::string());
    p.project_name = j.value("project_name", std::string());
    p.preset_name = j.value("preset_name", std::string());
    p.filename = j.value("filename", std::string());
    p.config_filename = j.value("config_filename", std::string());
    p.plate_index = j.value("plate_index", 0);
    p.ftp_folder = j.value("ftp_folder", std::string());
    p.ftp_file = j.value("ftp_file", std::string());
    p.ftp_file_md5 = j.value("ftp_file_md5", std::string());
    p.nozzle_mapping = j.value("nozzle_mapping", std::string());
    p.ams_mapping = j.value("ams_mapping", std::string());
    p.ams_mapping2 = j.value("ams_mapping2", std::string());
    p.ams_mapping_info = j.value("ams_mapping_info", std::string());
    p.nozzles_info = j.value("nozzles_info", std::string());
    p.connection_type = j.value("connection_type", std::string());
    p.comments = j.value("comments", std::string());
    p.origin_profile_id = j.value("origin_profile_id", 0);
    p.stl_design_id = j.value("stl_design_id", 0);
    p.origin_model_id = j.value("origin_model_id", std::string());
    p.print_type = j.value("print_type", std::string());
    p.dst_file = j.value("dst_file", std::string());
    p.dev_name = j.value("dev_name", std::string());
    p.dev_ip = j.value("dev_ip", std::string());
    p.use_ssl_for_ftp = j.value("use_ssl_for_ftp", false);
    p.use_ssl_for_mqtt = j.value("use_ssl_for_mqtt", false);
    p.username = j.value("username", std::string());
    p.password = j.value("password", std::string());
    p.task_bed_leveling = j.value("task_bed_leveling", false);
    p.task_flow_cali = j.value("task_flow_cali", false);
    p.task_vibration_cali = j.value("task_vibration_cali", false);
    p.task_layer_inspect = j.value("task_layer_inspect", false);
    p.task_record_timelapse = j.value("task_record_timelapse", false);
    p.task_use_ams = j.value("task_use_ams", false);
    p.task_bed_type = j.value("task_bed_type", std::string());
    p.extra_options = j.value("extra_options", std::string());
    p.auto_bed_leveling = j.value("auto_bed_leveling", 0);
    p.auto_flow_cali = j.value("auto_flow_cali", 0);
    p.auto_offset_cali = j.value("auto_offset_cali", 0);
    p.extruder_cali_manual_mode = j.value("extruder_cali_manual_mode", -1);
    p.task_ext_change_assist = j.value("task_ext_change_assist", false);
    p.try_emmc_print = j.value("try_emmc_print", false);
    p.svc_context = j.value("svc_context", std::string());
    p.slicer_uid = j.value("slicer_uid", std::string());
    return p;
}

// 02.08.01 filament-spool / AMS sync request structs.
inline nlohmann::json to_json(const Slic3r::FilamentQueryParams& p)
{
    return {
        {"category", p.category},
        {"status", p.status},
        {"spool_id", p.spool_id},
        {"rfid", p.rfid},
        {"offset", p.offset},
        {"limit", p.limit}
    };
}

inline Slic3r::FilamentQueryParams filament_query_params_from_json(const nlohmann::json& j)
{
    Slic3r::FilamentQueryParams p{};
    p.category = j.value("category", std::string());
    p.status = j.value("status", std::string());
    p.spool_id = j.value("spool_id", std::string());
    p.rfid = j.value("rfid", std::string());
    p.offset = j.value("offset", 0);
    p.limit = j.value("limit", 20);
    return p;
}

inline nlohmann::json to_json(const Slic3r::FilamentDeleteParams& p)
{
    return {
        {"ids", p.ids},
        {"rfids", p.rfids}
    };
}

inline Slic3r::FilamentDeleteParams filament_delete_params_from_json(const nlohmann::json& j)
{
    Slic3r::FilamentDeleteParams p{};
    p.ids = j.value("ids", std::vector<std::string>());
    p.rfids = j.value("rfids", std::vector<std::string>());
    return p;
}

inline nlohmann::json to_json(const Slic3r::AmsSyncItem& it)
{
    return {
        {"RFID", it.RFID},
        {"filamentVendor", it.filamentVendor},
        {"filamentType", it.filamentType},
        {"filamentName", it.filamentName},
        {"filamentId", it.filamentId},
        {"isSupport", it.isSupport},
        {"color", it.color},
        {"colorType", it.colorType},
        {"colors", it.colors},
        {"netWeight", it.netWeight},
        {"totalNetWeight", it.totalNetWeight},
        {"trayIdName", it.trayIdName},
        {"note", it.note},
        {"amsSn", it.amsSn},
        {"slotId", it.slotId},
        {"amsId", it.amsId},
        {"amsType", it.amsType},
        {"createNew", it.createNew}
    };
}

inline Slic3r::AmsSyncItem ams_sync_item_from_json(const nlohmann::json& j)
{
    Slic3r::AmsSyncItem it{};
    it.RFID = j.value("RFID", std::string());
    it.filamentVendor = j.value("filamentVendor", std::string());
    it.filamentType = j.value("filamentType", std::string());
    it.filamentName = j.value("filamentName", std::string());
    it.filamentId = j.value("filamentId", std::string());
    it.isSupport = j.value("isSupport", false);
    it.color = j.value("color", std::string());
    it.colorType = j.value("colorType", 0);
    it.colors = j.value("colors", std::vector<std::string>());
    it.netWeight = j.value("netWeight", 0);
    it.totalNetWeight = j.value("totalNetWeight", 0);
    it.trayIdName = j.value("trayIdName", std::string());
    it.note = j.value("note", std::string());
    it.amsSn = j.value("amsSn", std::string());
    it.slotId = j.value("slotId", std::string());
    it.amsId = j.value("amsId", 0);
    it.amsType = j.value("amsType", 0);
    it.createNew = j.value("createNew", false);
    return it;
}

inline nlohmann::json to_json(const Slic3r::AmsSyncParams& p)
{
    nlohmann::json items = nlohmann::json::array();
    for (const auto& it : p.items)
        items.push_back(to_json(it));
    return {
        {"devId", p.devId},
        {"items", items}
    };
}

inline Slic3r::AmsSyncParams ams_sync_params_from_json(const nlohmann::json& j)
{
    Slic3r::AmsSyncParams p{};
    p.devId = j.value("devId", std::string());
    if (j.contains("items") && j["items"].is_array()) {
        for (const auto& it : j["items"])
            p.items.push_back(ams_sync_item_from_json(it));
    }
    return p;
}

inline nlohmann::json to_json(const Slic3r::PublishParams& p)
{
    return {
        {"project_name", p.project_name},
        {"project_3mf_file", p.project_3mf_file},
        {"preset_name", p.preset_name},
        {"project_model_id", p.project_model_id},
        {"design_id", p.design_id},
        {"config_filename", p.config_filename}
    };
}

inline Slic3r::PublishParams publish_params_from_json(const nlohmann::json& j)
{
    Slic3r::PublishParams p{};
    p.project_name = j.value("project_name", std::string());
    p.project_3mf_file = j.value("project_3mf_file", std::string());
    p.preset_name = j.value("preset_name", std::string());
    p.project_model_id = j.value("project_model_id", std::string());
    p.design_id = j.value("design_id", std::string());
    p.config_filename = j.value("config_filename", std::string());
    return p;
}

}
