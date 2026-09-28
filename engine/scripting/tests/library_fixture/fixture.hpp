#pragma once
#include "base/result.hpp"
#include "scripting/annotations.hpp" // IWYU pragma: keep

#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ets::test {
ETS_REFLECT()
struct LibraryRecord {
    std::string name;
    std::vector<int> values;
    std::map<std::string, int> counts;
};

ETS_REFLECT()
struct LibraryOwned {
    int number {17};

  private:
    std::unique_ptr<int> m_token = std::make_unique<int>(1);
};
ETS_REFLECT(LuauLibrary(name = "@fixture"))
class LibraryFixture {
  public:
    ETS_REFLECT(LuauExport(name = "add"))
    int add_value(int value) {
        m_value += value;
        return m_value;
    }
    static std::string echo(std::string_view value) {
        return std::string(value);
    }
    static std::optional<int> optional(std::optional<int> value) {
        return value;
    }
    static unsigned int unsigned_value(unsigned int value) { return value; }
    static char character(char value) { return value; }
    static char32_t codepoint(char32_t value) { return value; }
    static LibraryOwned owned() { return {}; }
    static int inspect(const LibraryOwned& value) { return value.number; }
    static LibraryRecord record(LibraryRecord value) { return value; }
    static int sum(const LibraryRecord& value) {
        int result = 0;
        for (int v : value.values) {
            result += v;
        }
        return result;
    }
    LibraryRecord& retained() { return m_record; }
    static std::string_view view(std::string_view value) { return value; }
    static Result<int, std::string> checked(bool success) {
        if (!success) {
            return failure(std::string("expected failure"));
        }
        return 42;
    }
    static const LibraryRecord readonly_record(LibraryRecord value) {
        return value;
    }
    static const std::vector<LibraryRecord> readonly_records() {
        return {{"nested", {1}, {{"a", 2}}}};
    }
    static void fail() { throw std::runtime_error("fixture failure"); }

  private:
    int m_value {0};
    LibraryRecord m_record {"retained", {1, 2}, {{"a", 3}}};
};
} // namespace ets::test
