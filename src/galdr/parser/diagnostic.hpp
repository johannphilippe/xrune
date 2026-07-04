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
