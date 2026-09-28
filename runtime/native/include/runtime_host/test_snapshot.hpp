#pragma once
#include <string>
#include <string_view>
namespace ets {
class World;
namespace runtime_host {
std::string inspect_test_snapshot(const World& world, std::string_view payload);
}
} // namespace ets
