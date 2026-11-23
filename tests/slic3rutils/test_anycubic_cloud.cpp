#include <catch2/catch_test_macros.hpp>

#include <array>
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

TEST_CASE("The device page sees a busy printer as running", "[AnycubicPrinterAgent]")
{
    Printer printer;
    printer.is_printing = 2;
    CHECK(AnycubicPrinterAgent::status_payload(printer)["print"]["gcode_state"] == "RUNNING");
    printer.is_printing = 1;
    const auto idle = AnycubicPrinterAgent::status_payload(printer);
    CHECK(idle["print"]["gcode_state"] == "IDLE");
    CHECK(idle["print"]["command"] == "push_status");
    // Storage must be reported present, or the send dialog blocks
    CHECK(idle["print"]["sdcard"] == true);
}
