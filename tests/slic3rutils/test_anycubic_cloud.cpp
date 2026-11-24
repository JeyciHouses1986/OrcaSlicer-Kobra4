#include <catch2/catch_test_macros.hpp>

#include <array>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "slic3r/Utils/AnycubicCloud.hpp"
#include "slic3r/Utils/AnycubicPrinterAgent.hpp"

using namespace Slic3r;
using namespace Slic3r::AnycubicCloud;

TEST_CASE("A request is signed with the md5 of the client constants, time and nonce", "[AnycubicCloud]")
{
    CHECK(md5_hex("") == "d41d8cd98f00b204e9800998ecf8427e");

    // md5(app_id + timestamp + version + secret + nonce + app_id), with the slicer client's version
    const auto slicer = request_headers(TokenKind::slicer, "", 1700000000000, "nonce-1");
    CHECK(slicer.at("Xx-Signature") == "bda2a9ad37852ae1835321cfe3592950");
    CHECK(slicer.at("Xx-Device-Type") == "pcf");
    CHECK(slicer.at("Xx-Timestamp") == "1700000000000");
    CHECK(slicer.at("Xx-Nonce") == "nonce-1");
    CHECK(slicer.count("XX-Token") == 0);
    CHECK(slicer.count("Origin") == 0);

    // A web token is signed with the web client's version and sent as a browser would
    const auto web = request_headers(TokenKind::web, "user-token", 1700000000000, "nonce-1");
    CHECK(web.at("Xx-Signature") == "d45848af1da85ecb720be959b0b973c1");
    CHECK(web.at("Xx-Device-Type") == "web");
    CHECK(web.at("XX-Token") == "user-token");
    CHECK(web.count("Origin") == 1);
    CHECK(web.count("User-Agent") == 1);
}

TEST_CASE("An answer without data is a refusal carrying the server's message", "[AnycubicCloud]")
{
    nlohmann::json data;
    std::string    error;
    CHECK(parse_response(R"({"code":1,"msg":"ok","data":[1,2]})", data, error));
    CHECK(data == nlohmann::json::array({1, 2}));

    CHECK_FALSE(parse_response(R"({"code":0,"msg":"token expired","data":null})", data, error));
    CHECK(error == "token expired");
    CHECK_FALSE(parse_response("<html>", data, error));
}

TEST_CASE("The printer list keeps each printer's id, model and state", "[AnycubicCloud]")
{
    const auto printers = parse_printers(nlohmann::json::parse(R"([
        {"id": 12, "name": "Workshop", "model": "Anycubic Kobra 4", "device_status": 1, "is_printing": 2},
        {"id": "34", "name": "Spare", "model": "Anycubic Kobra 3", "device_status": 2, "is_printing": 1},
        {"name": "no id"}
    ])"));
    REQUIRE(printers.size() == 2);
    CHECK(printers[0].id == "12");
    CHECK(printers[0].model == "Anycubic Kobra 4");
    CHECK(printers[0].online());
    CHECK(printers[0].busy());
    CHECK(printers[1].id == "34");
    CHECK_FALSE(printers[1].online());
    CHECK_FALSE(printers[1].busy());
}

TEST_CASE("The host names the printer, or the account's only printer is used", "[AnycubicCloud]")
{
    std::vector<Printer> printers(2);
    printers[0].id   = "12";
    printers[0].name = "Workshop";
    printers[1].id   = "34";
    printers[1].name = "Spare";

    CHECK(choose_printer(printers, "Spare") == &printers[1]);
    CHECK(choose_printer(printers, "12") == &printers[0]);
    CHECK(choose_printer(printers, "cloud") == nullptr); // ambiguous

    printers.resize(1);
    CHECK(choose_printer(printers, "cloud") == &printers[0]);
    CHECK(choose_printer({}, "cloud") == nullptr);
}

TEST_CASE("ACE slots are numbered across units and empty slots are not loaded", "[AnycubicCloud]")
{
    // Slot 1 of the first unit is empty (edit_status 2) but still reports the last spool it held
    const auto slots = parse_ace_slots(nlohmann::json::parse(R"({
        "multi_color_box": [
            {"id": 1, "slots": [
                {"index": 0, "type": "PLA", "color": [0, 0, 0], "edit_status": 0},
                {"index": 1, "type": "PETG", "color": [255, 255, 255], "edit_status": 2}
            ]},
            {"id": 2, "slots": [
                {"index": 2, "type": "PETG", "color": [194, 225, 137], "edit_status": 1}
            ]}
        ]
    })"));
    REQUIRE(slots.size() == 3);
    CHECK(slots[0].index == 0);
    CHECK(slots[0].loaded);
    CHECK(slots[0].material_type == "PLA");
    CHECK_FALSE(slots[1].loaded);
    CHECK(slots[2].index == 6);
    CHECK(slots[2].loaded);
    CHECK(slots[2].rgb == std::array<int, 3>{194, 225, 137});
}

TEST_CASE("A single ACE unit may come as an object", "[AnycubicCloud]")
{
    const auto slots = parse_ace_slots(nlohmann::json::parse(
        R"({"multi_color_box": {"id": 1, "slots": [{"index": 3, "type": "PLA", "color": [1, 2, 3], "edit_status": 0}]}})"));
    REQUIRE(slots.size() == 1);
    CHECK(slots[0].index == 3);
    CHECK(parse_ace_slots(nlohmann::json::parse(R"({"multi_color_box": null})")).empty());
}

TEST_CASE("Colours convert between hex and RGB", "[AnycubicCloud]")
{
    CHECK(rgb_from_hex("#C2E189") == std::array<int, 3>{194, 225, 137});
    CHECK(rgb_from_hex("C2E189FF") == std::array<int, 3>{194, 225, 137});
    CHECK(rgb_from_hex("#xyz") == std::array<int, 3>{0, 0, 0});
    CHECK(hex_from_rgb({194, 225, 137}) == "#C2E189");
}

TEST_CASE("Only filaments given a tray are mapped to an ACE slot", "[AnycubicCloud]")
{
    // Filament 0 goes to slot 2, filament 1 is unused, filament 2 goes to slot 0
    const auto mapping = slot_mapping_from_ams_mapping_info(R"([
        {"ams": 2, "filamentType": "PLA", "sourceColor": "#000000", "targetColor": "101010FF"},
        {"ams": -1, "filamentType": "", "sourceColor": "", "targetColor": ""},
        {"ams": 0, "filamentType": "PETG", "sourceColor": "#C2E189", "targetColor": "C0E080FF"}
    ])");
    REQUIRE(mapping.size() == 2);
    CHECK(mapping[0].paint_index == 0);
    CHECK(mapping[0].ams_index == 2);
    CHECK(mapping[0].ams_color == std::array<int, 3>{16, 16, 16});
    CHECK(mapping[1].paint_index == 2);
    CHECK(mapping[1].ams_index == 0);
    CHECK(mapping[1].material_type == "PETG");
    CHECK(mapping[1].paint_color == std::array<int, 3>{194, 225, 137});

    CHECK(slot_mapping_from_ams_mapping_info("").empty());
    CHECK(slot_mapping_from_ams_mapping_info("[1,").empty());
}

TEST_CASE("The start order prints the uploaded file with the slot mapping", "[AnycubicCloud]")
{
    SlotMapping entry;
    entry.paint_index   = 1;
    entry.ams_index     = 3;
    entry.material_type = "PLA";
    entry.paint_color   = {1, 2, 3};
    entry.ams_color     = {4, 5, 6};

    const auto order = start_print_order("12", 99, {entry});
    CHECK(order["order_id"] == 1);
    CHECK(order["printer_id"] == 12);
    CHECK(order["data"]["file_id"] == 99);
    CHECK(order["data"]["is_delete_file"] == 1);
    CHECK(order["ams_info"]["use_ams"] == true);
    const auto& box = order["ams_info"]["ams_box_mapping"].at(0);
    CHECK(box["paint_index"] == 1);
    CHECK(box["ams_index"] == 3);
    CHECK(box["paint_color"] == nlohmann::json::array({1, 2, 3}));
    CHECK(box["ams_color"] == nlohmann::json::array({4, 5, 6}));

    // Without a mapping the printer prints from its single feed
    CHECK(start_print_order("12", 99, {})["ams_info"].is_null());
}

TEST_CASE("The start order asks for a timelapse only when the dialog does", "[AnycubicCloud]")
{
    CHECK(start_print_order("12", 99, {})["data"]["task_settings"]["camera_timelapse"] == 0);
    CHECK(start_print_order("12", 99, {}, true)["data"]["task_settings"]["camera_timelapse"] == 1);
}

TEST_CASE("The camera opens with Agora credentials for one viewing", "[AnycubicCloud]")
{
    const auto order = camera_open_order("12");
    CHECK(order["order_id"] == 1001);
    CHECK(order["printer_id"] == 12);
    CHECK(order["shengwang_rtc_support"] == true);

    const auto answer = nlohmann::json::parse(R"({
        "msgid": "m",
        "shengwang": {"appid": "a", "channel": "c", "rtc_token": "t", "client_uid": 7,
                      "encryption_mode": "AES_128_GCM2", "encryption_key": "k", "encryption_kdf_salt": "c2FsdA=="},
        "shengwang_device": {"uid": 8}
    })");
    const auto session = camera_session(answer);
    REQUIRE(session.is_object());
    CHECK(session["channel"] == "c");
    CHECK(session["client_uid"] == 7);
    CHECK(session["encryption_mode"] == "AES_128_GCM2");
    CHECK(!session.contains("msgid"));

    // Another transport, or credentials missing a part, cannot be played
    CHECK(camera_session(nlohmann::json::parse(R"({"token": {"tmpSecretId": "x"}})")).is_null());
    auto partial = answer;
    partial["shengwang"].erase("rtc_token");
    CHECK(camera_session(partial).is_null());
    CHECK(camera_session(nullptr).is_null());
}

TEST_CASE("Temperatures are read from the printer's parameters", "[AnycubicCloud]")
{
    int nozzle = 0, bed = 0;
    parse_temperatures(nlohmann::json::parse(R"({"parameter": {"curr_nozzle_temp": 215.4, "curr_hotbed_temp": "60"}})"), nozzle, bed);
    CHECK(nozzle == 215);
    CHECK(bed == 60);
    parse_temperatures(nlohmann::json::parse(R"({"parameter": null})"), nozzle, bed);
    CHECK(nozzle == -1);
    CHECK(bed == -1);
}

TEST_CASE("The newest job of the printer is taken from the project list", "[AnycubicCloud]")
{
    const auto projects = nlohmann::json::parse(R"([
        {"printer_id": 34, "gcode_name": "other printer", "print_status": 1},
        {"printer_id": 12, "gcode_name": "frame.gcode", "print_status": 1, "pause": 0, "progress": 42,
         "remain_time": 75, "settings": "{\"curr_layer\": 30, \"total_layers\": 120}"},
        {"printer_id": 12, "gcode_name": "older.gcode", "print_status": 2}
    ])");
    const auto job = latest_job(projects, "12");
    REQUIRE(job.has_value());
    CHECK(job->name == "frame.gcode");
    CHECK(job->print_status == Job::printing);
    CHECK_FALSE(job->paused);
    CHECK(job->progress == 42);
    CHECK(job->remain_minutes == 75);
    CHECK(job->layer == 30);
    CHECK(job->total_layers == 120);

    CHECK_FALSE(latest_job(projects, "56").has_value());
    CHECK_FALSE(latest_job(nlohmann::json(), "12").has_value());
}

TEST_CASE("The device page sees a busy printer as running", "[AnycubicPrinterAgent]")
{
    Printer printer;
    printer.is_printing = 2;
    CHECK(AnycubicPrinterAgent::status_payload(printer, {})["print"]["gcode_state"] == "RUNNING");
    printer.is_printing = 1;
    const auto idle = AnycubicPrinterAgent::status_payload(printer, {});
    CHECK(idle["print"]["gcode_state"] == "IDLE");
    CHECK(idle["print"]["command"] == "push_status");
    CHECK(idle["print"].count("nozzle_temper") == 0);
    // Storage must be reported present, or the send dialog blocks
    CHECK(idle["print"]["sdcard"] == true);
}

TEST_CASE("The device page shows the job, its progress and the temperatures", "[AnycubicPrinterAgent]")
{
    Printer printer;
    printer.is_printing = 2;
    AnycubicPrinterAgent::CloudState state;
    state.nozzle_temp = 250;
    state.bed_temp    = 70;
    state.job         = Job{};
    state.job->name           = "frame.gcode";
    state.job->print_status   = Job::printing;
    state.job->progress       = 42;
    state.job->remain_minutes = 75;
    state.job->layer          = 30;
    state.job->total_layers   = 120;

    auto print = AnycubicPrinterAgent::status_payload(printer, state)["print"];
    CHECK(print["gcode_state"] == "RUNNING");
    CHECK(print["nozzle_temper"] == 250);
    CHECK(print["bed_temper"] == 70);
    CHECK(print["subtask_name"] == "frame.gcode");
    CHECK(print["mc_percent"] == 42);
    CHECK(print["mc_remaining_time"] == 75);
    CHECK(print["layer_num"] == 30);
    CHECK(print["total_layer_num"] == 120);

    state.job->paused = true;
    CHECK(AnycubicPrinterAgent::status_payload(printer, state)["print"]["gcode_state"] == "PAUSE");
    state.job->print_status = Job::preheating;
    CHECK(AnycubicPrinterAgent::status_payload(printer, state)["print"]["gcode_state"] == "PREPARE");

    // Once the printer is idle, the page shows how the last job ended
    printer.is_printing     = 1;
    state.job->print_status = Job::complete;
    CHECK(AnycubicPrinterAgent::status_payload(printer, state)["print"]["gcode_state"] == "FINISH");
}

TEST_CASE("A print order waits while the cloud reads the uploaded file", "[AnycubicCloud]")
{
    CHECK(file_not_ready("No slicer found"));
    CHECK(file_not_ready("No file found"));
    CHECK(!file_not_ready("Login information has expired. Please login again."));
}

TEST_CASE("The statistics block is built from Orca's stats", "[AnycubicCloud]")
{
    std::istringstream gcode("; HEADER_BLOCK_START\n"
                             "; max_z_height: 12.40\n"
                             "; HEADER_BLOCK_END\n"
                             "M83\n"
                             "G1 X5 Y5 E2 ; purge line\n"
                             "G0 X100 Y100\n"
                             "G1 X110 Y120 E0.5\n"
                             "G1 X130 E0.5\n"
                             "G1 X200 Y200\n"
                             "G1 E-0.8\n"
                             "; total filament used [g] = 87.70\r\n"
                             "; total layers count = 62\n"
                             "; estimated printing time (normal mode) = 2h 47m 3s\n");
    const std::string block = statistics_block(gcode);
    CHECK(block.find("; statistics = begin\n") != std::string::npos);
    CHECK(block.find("; used_filament = 87.70\n") != std::string::npos);
    CHECK(block.find("; print_time = 2h 47m 3s\n") != std::string::npos);
    CHECK(block.find("; model_size = 125.00,115.00,12.40\n") != std::string::npos);
    CHECK(block.find("; total_layers = 62\n") != std::string::npos);
    CHECK(block.find("; statistics = end\n") != std::string::npos);

    // Slicer Next's own G-code, and one without stats, are left alone
    std::istringstream slicer_next("; estimated printing time (normal mode) = 1h\n; statistics = begin\n");
    CHECK(statistics_block(slicer_next).empty());
    std::istringstream no_stats("G1 X1 E1\n");
    CHECK(statistics_block(no_stats).empty());
}

TEST_CASE("The dryer and humidity of each ACE unit are read from the printer info", "[AnycubicCloud]")
{
    const auto info  = nlohmann::json::parse(R"({"multi_color_box": [
        {"id": 0, "temp": 41, "humidity": "32", "drying_status": {"status": 1, "target_temp": 55, "duration": 240, "remain_time": 180}},
        {"id": 1, "drying_status": {"status": 0, "remain_time": null}}
    ]})");
    const auto units = parse_ace_units(info);
    REQUIRE(units.size() == 2);
    CHECK(units[0].temp == 41);
    CHECK(units[0].humidity == 32);
    CHECK(units[0].drying == 1);
    CHECK(units[0].dry_target_temp == 55);
    CHECK(units[0].dry_duration == 240);
    CHECK(units[0].dry_remain == 180);
    CHECK(units[1].id == 1);
    CHECK(units[1].temp == -1);
    CHECK(units[1].humidity == -1);
    CHECK(units[1].drying == 0);
    CHECK(parse_ace_units(nlohmann::json::parse(R"({"multi_color_box": null})")).empty());
}

TEST_CASE("Pause, resume and stop name the running job", "[AnycubicCloud]")
{
    const auto order = job_order("12", "345", JobOrder::stop);
    CHECK(order["order_id"] == 4);
    CHECK(order["printer_id"] == 12);
    CHECK(order["project_id"] == 345);
    CHECK(job_order("12", "345", JobOrder::pause)["order_id"] == 2);
    CHECK(job_order("12", "345", JobOrder::resume)["order_id"] == 3);
}

TEST_CASE("Print settings changes go to the running job", "[AnycubicCloud]")
{
    const auto order = print_settings_order("12", "345", {{"target_hotbed_temp", 70}});
    CHECK(order["order_id"] == 6);
    CHECK(order["project_id"] == 345);
    CHECK(order["data"]["settings"]["target_hotbed_temp"] == 70);
}

TEST_CASE("The ACE dryer starts with a temperature and time, and stops with no time", "[AnycubicCloud]")
{
    const auto start = ace_drying_order("12", 1, 55, 240);
    CHECK(start["order_id"] == 1207);
    const auto& box = start["data"]["multi_color_box"][0];
    CHECK(box["id"] == 1);
    CHECK(box["drying_status"]["status"] == 1);
    CHECK(box["drying_status"]["target_temp"] == 55);
    CHECK(box["drying_status"]["duration"] == 240);

    const auto stop = ace_drying_order("12", 1, 55, 0);
    CHECK(stop["data"]["multi_color_box"][0]["drying_status"]["status"] == 0);
}

TEST_CASE("Device page G-code lines become print settings", "[AnycubicCloud]")
{
    CHECK(print_settings_from_gcode("M140 S70\n") == nlohmann::json{{"target_hotbed_temp", 70}});
    CHECK(print_settings_from_gcode("M104 S250") == nlohmann::json{{"target_nozzle_temp", 250}});
    CHECK(print_settings_from_gcode("M106 P1 S255") == nlohmann::json{{"fan_speed_pct", 100}});
    CHECK(print_settings_from_gcode("M106 P2 S0") == nlohmann::json{{"aux_fan_speed_pct", 0}});
    CHECK(print_settings_from_gcode("M107") == nlohmann::json{{"fan_speed_pct", 0}});
    CHECK(print_settings_from_gcode("M106 P3 S128").is_null());
    CHECK(print_settings_from_gcode("G28").is_null());
}
