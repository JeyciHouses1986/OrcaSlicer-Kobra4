#pragma once

#include <string>
#include <vector>

namespace Slic3r {

class PrintConfig;

// Anycubic printers fed by an ACE multi-colour box (Kobra 4) read two lines from the G-code header:
// which colour and material every tool (T<n>) is, so the printer can offer an ACE slot for each, and
// the purge volumes between tools, because the purge itself is done by the firmware on T<n>
// rather than by the G-code. Anycubic Slicer Next writes them; without them the printer has to be
// told everything by hand.
bool anycubic_ace_header_wanted(const PrintConfig &config);

// The header lines, each ending in '\n', for the tools the print uses (indices into the filament
// lists of `config`). Empty when nothing is used.
std::string anycubic_ace_header(const PrintConfig &config, const std::vector<unsigned int> &used_filaments);

} // namespace Slic3r
