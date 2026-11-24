#include <catch2/catch_test_macros.hpp>

#include "libslic3r/GCode/AnycubicHeader.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "test_helpers.hpp"

#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;

TEST_CASE("Anycubic ACE header lists each used tool and the purge volumes", "[AnycubicHeader]")
{
    DynamicPrintConfig dyn = DynamicPrintConfig::full_print_config();
    dyn.set_deserialize_strict({
        {"printer_model", "Anycubic Kobra 4"},
        {"single_extruder_multi_material", "1"},
        {"filament_colour", "#000000;#C2E189"},
        {"filament_type", "PLA;PETG"},
        {"flush_volumes_matrix", "0,574,161,0"},
        {"flush_volumes_vector", "140,140,140,140"},
        {"flush_multiplier", "1"},
    });
    PrintConfig config;
    config.apply(dyn, true);

    REQUIRE(anycubic_ace_header_wanted(config));

    const std::string header = anycubic_ace_header(config, {0, 1});
    CHECK(header.find(R"(; paint_info = [{"material_type":"PLA","paint_color":[0,0,0],"paint_index":0},)"
                      R"({"material_type":"PETG","paint_color":[194,225,137],"paint_index":1}])") != std::string::npos);
    CHECK(header.find(R"("flush_volumes_matrix":[0.0,574.0,161.0,0.0])") != std::string::npos);
    CHECK(header.find(R"("flush_volumes_vector":[140.0,140.0,140.0,140.0])") != std::string::npos);
    CHECK(header.find("; flush_multiplier_calculate_by_acnext: 1\n") != std::string::npos);
}

TEST_CASE("Anycubic ACE header is only written for ACE printers", "[AnycubicHeader]")
{
    SECTION("Kobra 4 with the ACE gets it")
    {
        const std::string gcode = slice({cube(20)},
                                        {{"printer_model", "Anycubic Kobra 4"}, {"single_extruder_multi_material", "1"}});
        const size_t header_end = gcode.find("; HEADER_BLOCK_END");
        REQUIRE(header_end != std::string::npos);
        const size_t paint_info = gcode.find("; paint_info = ");
        REQUIRE(paint_info != std::string::npos);
        CHECK(paint_info < header_end);
    }
    SECTION("other printers do not")
    {
        const std::string gcode = slice({cube(20)}, {{"single_extruder_multi_material", "1"}});
        CHECK(gcode.find("; paint_info = ") == std::string::npos);
    }
}
