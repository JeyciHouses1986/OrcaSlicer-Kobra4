#include "AnycubicHeader.hpp"

#include "libslic3r/PrintConfig.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <vector>

namespace Slic3r {
namespace {

// Printers whose firmware is known to read these lines. Matched on printer_model so that only
// system and user presets derived from these printers are affected.
constexpr std::array<const char *, 1> ace_printer_models{"Anycubic Kobra 4"};

std::array<int, 3> rgb_from_hex(const std::string &colour)
{
    std::array<int, 3> rgb{0, 0, 0};
    // "#RRGGBB" or "#RRGGBBAA"; anything else stays black, which the printer treats as "no colour".
    if (colour.size() < 7 || colour[0] != '#')
        return rgb;
    for (size_t i = 0; i < 3; ++i)
        rgb[i] = static_cast<int>(std::strtol(colour.substr(1 + 2 * i, 2).c_str(), nullptr, 16));
    return rgb;
}

} // namespace

bool anycubic_ace_header_wanted(const PrintConfig &config)
{
    if (!config.single_extruder_multi_material.value)
        return false;
    for (const char *model : ace_printer_models)
        if (config.printer_model.value == model)
            return true;
    return false;
}

std::string anycubic_ace_header(const PrintConfig &config, const std::vector<unsigned int> &used_filaments)
{
    if (used_filaments.empty())
        return {};

    nlohmann::json paint_info = nlohmann::json::array();
    for (unsigned int id : used_filaments) {
        paint_info.push_back({
            {"paint_index", id},
            {"paint_color", rgb_from_hex(config.filament_colour.get_at(id))},
            {"material_type", config.filament_type.get_at(id)},
        });
    }

    // flush_volumes_matrix holds one n x n block per nozzle; the Kobra 4 has a single nozzle.
    const size_t         n = config.filament_colour.values.size();
    std::vector<double>  matrix(config.flush_volumes_matrix.values.begin(),
                                config.flush_volumes_matrix.values.begin() +
                                    std::min(config.flush_volumes_matrix.values.size(), n * n));
    const std::vector<double> &vector = config.flush_volumes_vector.values;

    nlohmann::json project_info = {
        {"flush_multiplier", config.flush_multiplier.values.empty() ? 1.0 : config.flush_multiplier.get_at(0)},
        // Slicer Next sends one multiplier per entry of the vector; Orca has no such setting.
        {"flush_volumes_chan_multipliers", std::vector<double>(vector.size(), 1.0)},
        {"flush_volumes_matrix", matrix},
        {"flush_volumes_vector", vector},
    };

    std::string out;
    out += "; paint_info = " + paint_info.dump() + "\n";
    out += "; project_info = " + project_info.dump() + "\n";
    out += "; flush_multiplier_calculate_by_acnext: 1\n";
    return out;
}

} // namespace Slic3r
