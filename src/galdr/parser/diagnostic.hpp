/*
 * Xrune — a real-time audio engine, graph and instancing system.
 * Copyright (C) 2026 Johann Philippe
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once
#include <string>
#include <vector>

namespace xrune::galdr {

// A compile-time error/warning with a source position (spec §12).
struct diagnostic {
    std::string message;
    int line = 0;
    int col = 0;

    std::string format() const {
        return std::to_string(line) + ":" + std::to_string(col) + ": " + message;
    }
};

using diagnostics = std::vector<diagnostic>;

} // namespace xrune::galdr
