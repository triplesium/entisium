#include "clock.hpp"

#include <chrono>
namespace ets::luau::task {
double Library::now() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch()
    )
        .count();
}
} // namespace ets::luau::task
