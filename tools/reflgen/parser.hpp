#pragma once

#include "model.hpp"

#include <string>
#include <vector>

namespace ets::reflgen {

struct HeaderParseOutput {
    ParseResult result;
    std::vector<std::string> dependencies;
    bool has_errors {false};
};

class HeaderParser {
  public:
    HeaderParser(
        std::vector<std::string> headers,
        std::vector<std::string> include_paths,
        bool verbose
    );

    [[nodiscard]] HeaderParseOutput parse();

  private:
    std::vector<std::string> m_headers;
    std::vector<std::string> m_include_paths;
    bool m_verbose = false;
};

} // namespace ets::reflgen
