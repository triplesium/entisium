#include "runtime_host/application.hpp"

#include "asset/server.hpp"
#include "base/env.hpp"
#include "base/log.hpp"
#include "core/time.hpp"
#include "graphics/graphics_device.hpp"
#include "graphics/swapchain.hpp"
#include "graphics_opengl_glfw/plugin.hpp"
#include "input/input.hpp"
#include "physics2d/physics_world.hpp"
#include "physics2d/plugin.hpp"
#include "project/project.hpp"
#include "project_runtime/runtime.hpp"
#include "project_scripting_luau/playtest.hpp"
#include "project_scripting_luau/plugin.hpp"
#include "rendering/plugin.hpp"
#include "rendering/render_app.hpp"
#include "runtime_host/quick_save.hpp"
#include "runtime_host/snapshot_archive.hpp"
#include "runtime_host/test_snapshot.hpp"
#include "runtime_inspection/provider.hpp"
#include "runtime_inspection/registry.hpp"
#include "runtime_inspection_ecs/entity.hpp"
#include "runtime_inspection_ecs/query.hpp"
#include "runtime_inspection_ecs/world_summary.hpp"
#include "runtime_inspection_snapshot/checkpoint.hpp"
#include "runtime_protocol/playtest.hpp"
#include "runtime_protocol/playtest_plugin.hpp"
#include "runtime_protocol/probe.hpp"
#include "scripting/playtest_segment.hpp"
#include "scripting/plugin.hpp"
#include "scripting/runtime.hpp"
#include "snapshot_runtime/adapters.hpp"
#include "snapshot_runtime_asset/adapters.hpp"
#include "snapshot_runtime_luau/adapters.hpp"
#include "snapshot_runtime_physics2d/adapters.hpp"
#include "snapshot_runtime_rendering/adapters.hpp"
#include "snapshot_runtime_ui/adapters.hpp"
#include "sprite/plugin.hpp"
#include "ui/surface.hpp"
#include "ui_rendering/plugin.hpp"
#include "ui_widgets/plugin.hpp"
#include "window/window.hpp"
#include "window_glfw/input.hpp"
#include "window_glfw/window.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <GLFW/glfw3.h>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image_write.h>

namespace ets::runtime_host {
namespace {

using Json = nlohmann::json;

constexpr std::string_view c_playtest_interfaces_id {"play.interfaces"};
constexpr std::string_view c_playtest_interfaces_schema {"play.interfaces.v1"};
constexpr std::string_view c_playtest_capture_id {"play.capture"};
constexpr std::string_view c_playtest_capture_schema {"play.capture.v1"};
constexpr std::string_view c_playtest_observe_id {"play.observe"};
constexpr std::string_view c_playtest_observe_schema {"play.observe.v1"};
constexpr std::string_view c_playtest_step_id {"play.step"};
constexpr std::string_view c_playtest_step_schema {"play.step.v1"};
constexpr std::string_view c_playtest_segment_id {"play.segment"};
constexpr std::string_view c_playtest_segment_schema {"play.segment.v1"};
constexpr std::string_view c_checkpoint_restore_id {"play.checkpoint.restore"};
constexpr std::string_view c_checkpoint_restore_schema {
    "play.checkpoint.restore.v1"
};
constexpr float c_playtest_fixed_delta = 1.0f / 60.0f;

struct EncodedFrameCapture {
    std::string payload_json;
    std::vector<byte> png;
};

void append_png_bytes(void* context, void* data, int size) {
    auto& bytes = *static_cast<std::vector<byte>*>(context);
    const auto* first = static_cast<const byte*>(data);
    bytes.insert(bytes.end(), first, first + size);
}

Result<std::vector<byte>, std::string>
encode_png(const TextureReadbackFrame& frame) {
    if (frame.width == 0 || frame.height == 0 || frame.data.empty()) {
        return failure(std::string("Captured frame is empty"));
    }
    if (frame.format != PixelFormat::Rgba8Unorm) {
        return failure(std::string("Captured frame is not in RGBA8 format"));
    }
    constexpr std::size_t c_channels = 4;
    if (frame.width > static_cast<uint32>(std::numeric_limits<int>::max()) ||
        frame.height > static_cast<uint32>(std::numeric_limits<int>::max()) ||
        static_cast<std::size_t>(frame.width) >
            std::numeric_limits<std::size_t>::max() /
                static_cast<std::size_t>(frame.height) / c_channels ||
        static_cast<std::size_t>(frame.width) * c_channels >
            static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return failure(std::string("Captured frame dimensions overflow"));
    }
    const auto row_size = static_cast<std::size_t>(frame.width) * c_channels;
    const auto expected_size =
        row_size * static_cast<std::size_t>(frame.height);
    if (frame.data.size() != expected_size) {
        return failure(
            std::string("Captured frame byte size does not match dimensions")
        );
    }

    std::vector<byte> top_left_pixels(expected_size);
    for (uint32 destination_y = 0; destination_y < frame.height;
         ++destination_y) {
        const auto source_y = frame.data_origin == TextureDataOrigin::TopLeft ?
                                  destination_y :
                                  frame.height - destination_y - 1;
        std::copy_n(
            frame.data.data() + static_cast<std::size_t>(source_y) * row_size,
            row_size,
            top_left_pixels.data() +
                static_cast<std::size_t>(destination_y) * row_size
        );
    }

    std::vector<byte> png;
    const auto encoded = stbi_write_png_to_func(
        append_png_bytes,
        &png,
        static_cast<int>(frame.width),
        static_cast<int>(frame.height),
        static_cast<int>(c_channels),
        top_left_pixels.data(),
        static_cast<int>(row_size)
    );
    if (encoded == 0 || png.empty()) {
        return failure(std::string("Failed to encode captured frame as PNG"));
    }
    return png;
}

Result<std::string, runtime_protocol::RuntimeInspectionError> inspect_runtime(
    World& world,
    const runtime_protocol::InspectionRequest& request
) {
    if (!world.has_resource<runtime_inspection::InspectionRegistry>()) {
        return failure(
            runtime_protocol::RuntimeInspectionError {
                .kind = "internal",
                .message = "Runtime inspection registry is not installed",
            }
        );
    }
    auto response =
        world.resource<runtime_inspection::InspectionRegistry>().dispatch(
            world,
            runtime_inspection::InspectionInvocation {
                .provider = request.provider,
                .schema = request.schema,
                .payload_json = request.payload_json,
            }
        );
    if (!response) {
        return failure(
            runtime_protocol::RuntimeInspectionError {
                .kind = std::string(
                    runtime_inspection::inspection_error_kind_name(
                        response.error().kind
                    )
                ),
                .message = std::move(response.error().message),
            }
        );
    }
    return std::move(*response);
}

runtime_protocol::RuntimeInspectionError
playtest_error(const runtime_protocol::PlaytestError& error) {
    return runtime_protocol::RuntimeInspectionError {
        .kind =
            std::string(runtime_protocol::playtest_error_kind_name(error.kind)),
        .message = error.message,
    };
}

std::string keyboard_action_schema() {
    Json keys = Json::array();
    for (auto key : c_key_codes) {
        if (key != KeyCode::Unknown) {
            keys.push_back(key_code_to_string(key));
        }
    }
    return Json {
        {"type", "object"},
        {"additionalProperties", false},
        {"properties",
         Json {
             {"keys",
              Json {
                  {"type", "array"},
                  {"items", Json {{"type", "string"}, {"enum", keys}}},
                  {"uniqueItems", true},
                  {"maxItems", 16},
              }},
         }},
        {"required", Json::array({"keys"})},
    }
        .dump();
}

std::string pointer_action_schema() {
    return Json {
        {"type", "object"},
        {"additionalProperties", false},
        {"properties",
         Json {
             {"x", Json {{"type", "number"}, {"minimum", 0}}},
             {"y", Json {{"type", "number"}, {"minimum", 0}}},
             {"buttons",
              Json {
                  {"type", "array"},
                  {"items",
                   Json {
                       {"type", "string"},
                       {"enum", Json::array({"Left", "Right", "Middle"})},
                   }},
                  {"uniqueItems", true},
                  {"maxItems", 3},
              }},
         }},
        {"required", Json::array({"x", "y", "buttons"})},
    }
        .dump();
}

Status<runtime_protocol::PlaytestError>
begin_keyboard_step(World& world, std::string_view action_json) {
    if (!world.has_resource<VirtualInput>()) {
        return failure(
            runtime_protocol::PlaytestError {
                .kind = runtime_protocol::PlaytestErrorKind::Unsupported,
                .message = "The project does not install InputPlugin",
            }
        );
    }

    try {
        const auto action = Json::parse(action_json);
        if (!action.is_object() || action.size() != 1 ||
            !action.contains("keys") || !action.at("keys").is_array()) {
            return failure(
                runtime_protocol::PlaytestError {
                    .kind = runtime_protocol::PlaytestErrorKind::InvalidAction,
                    .message = "Keyboard action must contain only a keys array",
                }
            );
        }
        std::vector<KeyCode> keys;
        keys.reserve(action.at("keys").size());
        for (const auto& value : action.at("keys")) {
            if (!value.is_string()) {
                return failure(
                    runtime_protocol::PlaytestError {
                        .kind =
                            runtime_protocol::PlaytestErrorKind::InvalidAction,
                        .message = "Keyboard action keys must be strings",
                    }
                );
            }
            const auto key = key_code_from_string(value.get<std::string>());
            if (key == KeyCode::Unknown) {
                return failure(
                    runtime_protocol::PlaytestError {
                        .kind =
                            runtime_protocol::PlaytestErrorKind::InvalidAction,
                        .message = "Unknown keyboard key '" +
                                   value.get<std::string>() + "'",
                    }
                );
            }
            keys.push_back(key);
        }
        world.resource<VirtualInput>().set_pressed_keys(keys);
        return {};
    } catch (const std::exception& error) {
        return failure(
            runtime_protocol::PlaytestError {
                .kind = runtime_protocol::PlaytestErrorKind::InvalidAction,
                .message =
                    std::string("Invalid keyboard action: ") + error.what(),
            }
        );
    }
}

Status<runtime_protocol::PlaytestError> end_keyboard_step(World& world) {
    if (world.has_resource<VirtualInput>()) {
        world.resource<VirtualInput>().clear_keys();
    }
    return {};
}

Status<runtime_protocol::PlaytestError>
begin_pointer_step(World& world, std::string_view action_json) {
    if (!world.has_resource<VirtualInput>()) {
        return failure(
            runtime_protocol::PlaytestError {
                .kind = runtime_protocol::PlaytestErrorKind::Unsupported,
                .message = "The project does not install InputPlugin",
            }
        );
    }

    try {
        const auto action = Json::parse(action_json);
        if (!action.is_object() || action.size() != 3 ||
            !action.contains("x") || !action.at("x").is_number() ||
            !action.contains("y") || !action.at("y").is_number() ||
            !action.contains("buttons") || !action.at("buttons").is_array()) {
            return failure(
                runtime_protocol::PlaytestError {
                    .kind = runtime_protocol::PlaytestErrorKind::InvalidAction,
                    .message = "Pointer action must contain only numeric x, "
                               "numeric y, and a buttons array",
                }
            );
        }
        const auto x = action.at("x").get<float>();
        const auto y = action.at("y").get<float>();
        if (x < 0.0F || y < 0.0F) {
            return failure(
                runtime_protocol::PlaytestError {
                    .kind = runtime_protocol::PlaytestErrorKind::InvalidAction,
                    .message = "Pointer coordinates must be non-negative",
                }
            );
        }
        std::vector<MouseButton> buttons;
        buttons.reserve(action.at("buttons").size());
        for (const auto& value : action.at("buttons")) {
            if (!value.is_string()) {
                return failure(
                    runtime_protocol::PlaytestError {
                        .kind =
                            runtime_protocol::PlaytestErrorKind::InvalidAction,
                        .message = "Pointer action buttons must be strings",
                    }
                );
            }
            const auto name = value.get<std::string>();
            if (name == "Left") {
                buttons.push_back(MouseButton::Left);
            } else if (name == "Right") {
                buttons.push_back(MouseButton::Right);
            } else if (name == "Middle") {
                buttons.push_back(MouseButton::Middle);
            } else {
                return failure(
                    runtime_protocol::PlaytestError {
                        .kind =
                            runtime_protocol::PlaytestErrorKind::InvalidAction,
                        .message = "Unknown pointer button '" + name + "'",
                    }
                );
            }
        }
        auto& input = world.resource<VirtualInput>();
        input.set_mouse_position({x, y});
        input.set_pressed_mouse_buttons(buttons);
        return {};
    } catch (const std::exception& error) {
        return failure(
            runtime_protocol::PlaytestError {
                .kind = runtime_protocol::PlaytestErrorKind::InvalidAction,
                .message =
                    std::string("Invalid pointer action: ") + error.what(),
            }
        );
    }
}

Status<runtime_protocol::PlaytestError> end_pointer_step(World& world) {
    if (world.has_resource<VirtualInput>()) {
        world.resource<VirtualInput>().clear_mouse_buttons();
    }
    return {};
}

Status<runtime_protocol::PlaytestError> invoke_playtest_begin(
    const runtime_protocol::PlaytestInterfaceRegistration& interface,
    World& world,
    std::string_view action_json
) {
    try {
        return interface.begin_step(world, action_json);
    } catch (const std::exception& error) {
        return failure(
            runtime_protocol::PlaytestError {
                .kind = runtime_protocol::PlaytestErrorKind::Internal,
                .message =
                    std::string("Playtest begin_step failed: ") + error.what(),
            }
        );
    } catch (...) {
        return failure(
            runtime_protocol::PlaytestError {
                .kind = runtime_protocol::PlaytestErrorKind::Internal,
                .message = "Playtest begin_step failed with an unknown "
                           "exception",
            }
        );
    }
}

Status<runtime_protocol::PlaytestError> invoke_playtest_end(
    const runtime_protocol::PlaytestInterfaceRegistration& interface,
    World& world
) {
    try {
        return interface.end_step(world);
    } catch (const std::exception& error) {
        return failure(
            runtime_protocol::PlaytestError {
                .kind = runtime_protocol::PlaytestErrorKind::Internal,
                .message =
                    std::string("Playtest end_step failed: ") + error.what(),
            }
        );
    } catch (...) {
        return failure(
            runtime_protocol::PlaytestError {
                .kind = runtime_protocol::PlaytestErrorKind::Internal,
                .message = "Playtest end_step failed with an unknown "
                           "exception",
            }
        );
    }
}

Result<std::string, runtime_protocol::PlaytestError> invoke_playtest_observe(
    const runtime_protocol::PlaytestInterfaceRegistration& interface,
    World& world
) {
    try {
        return interface.observe(world);
    } catch (const std::exception& error) {
        return failure(
            runtime_protocol::PlaytestError {
                .kind = runtime_protocol::PlaytestErrorKind::Internal,
                .message =
                    std::string("Playtest observe failed: ") + error.what(),
            }
        );
    } catch (...) {
        return failure(
            runtime_protocol::PlaytestError {
                .kind = runtime_protocol::PlaytestErrorKind::Internal,
                .message = "Playtest observe failed with an unknown exception",
            }
        );
    }
}

void register_builtin_playtest_interfaces(
    runtime_protocol::PlaytestRegistry& registry
) {
    auto registered = registry.add(
        runtime_protocol::PlaytestInterfaceRegistration {
            .descriptor =
                runtime_protocol::PlaytestInterfaceDescriptor {
                    .id = "runtime.keyboard",
                    .label = "Runtime Keyboard",
                    .description = "Holds a discoverable set of virtual "
                                   "keyboard keys for one playtest step.",
                    .decision_ticks = 1,
                    .minimum_ticks = 1,
                    .maximum_ticks = 120,
                    .allow_tick_override = true,
                    .action_schema_json = keyboard_action_schema(),
                    .observation_schema_json =
                        R"({"type":"object","additionalProperties":false})",
                },
            .begin_step = begin_keyboard_step,
            .end_step = end_keyboard_step,
        }
    );
    if (!registered) {
        fatal(
            "Failed to register built-in playtest interface: {}",
            registered.error().message
        );
    }

    registered = registry.add(
        runtime_protocol::PlaytestInterfaceRegistration {
            .descriptor =
                runtime_protocol::PlaytestInterfaceDescriptor {
                    .id = "runtime.pointer",
                    .label = "Runtime Pointer",
                    .description = "Positions a virtual pointer and holds a "
                                   "discoverable set of mouse buttons for one "
                                   "playtest step.",
                    .decision_ticks = 1,
                    .minimum_ticks = 1,
                    .maximum_ticks = 120,
                    .allow_tick_override = true,
                    .action_schema_json = pointer_action_schema(),
                    .observation_schema_json =
                        R"({"type":"object","additionalProperties":false})",
                },
            .begin_step = begin_pointer_step,
            .end_step = end_pointer_step,
        }
    );
    if (!registered) {
        fatal(
            "Failed to register built-in playtest interface: {}",
            registered.error().message
        );
    }
}

Result<std::string, runtime_inspection::InspectionError>
inspect_playtest_interfaces(World& world, std::string_view payload_json) {
    try {
        const auto payload = Json::parse(payload_json);
        if (!payload.is_object() || !payload.empty()) {
            return failure(
                runtime_inspection::InspectionError {
                    .kind =
                        runtime_inspection::InspectionErrorKind::InvalidRequest,
                    .message =
                        "Playtest interface request must be an empty object",
                }
            );
        }
    } catch (const std::exception& error) {
        return failure(
            runtime_inspection::InspectionError {
                .kind = runtime_inspection::InspectionErrorKind::InvalidRequest,
                .message = std::string("Invalid playtest interface request: ") +
                           error.what(),
            }
        );
    }

    if (!world.has_resource<runtime_protocol::PlaytestRegistry>()) {
        return failure(
            runtime_inspection::InspectionError {
                .kind = runtime_inspection::InspectionErrorKind::Internal,
                .message = "Playtest registry is not installed",
            }
        );
    }

    Json interfaces = Json::array();
    for (const auto& registration :
         world.resource<runtime_protocol::PlaytestRegistry>().interfaces()) {
        const auto& descriptor = registration.descriptor;
        interfaces.push_back(
            Json {
                {"id", descriptor.id},
                {"label", descriptor.label},
                {"description", descriptor.description},
                {"decision_ticks", descriptor.decision_ticks},
                {"minimum_ticks", descriptor.minimum_ticks},
                {"maximum_ticks", descriptor.maximum_ticks},
                {"allow_tick_override", descriptor.allow_tick_override},
                {"action_schema", Json::parse(descriptor.action_schema_json)},
                {"observation_schema",
                 Json::parse(descriptor.observation_schema_json)},
            }
        );
    }
    return Json {{"interfaces", std::move(interfaces)}}.dump();
}

Status<runtime_inspection::InspectionError>
register_playtest_inspection_providers(
    runtime_inspection::InspectionRegistry& registry
) {
    for (const auto* name : {"test.input", "test.advance", "test.snapshot"}) {
        auto added = registry.add(
            runtime_inspection::InspectionDescriptor {
                .id = name,
                .label = name,
                .description = "External Luau test host operation",
                .schema = std::string(name) + ".v1",
                .read_only = std::string_view(name) == "test.snapshot",
                .cost = runtime_inspection::InspectionCost::Low,
                .request_schema_json = R"({"type":"object"})",
                .response_schema_json = R"({"type":"object"})",
            },
            [](World&, std::string_view)
                -> Result<std::string, runtime_inspection::InspectionError> {
                return failure(
                    runtime_inspection::InspectionError {
                        .kind = runtime_inspection::InspectionErrorKind::
                            Unsupported,
                        .message = "External testing requires supervised "
                                   "native execution",
                    }
                );
            }
        );
        if (!added) {
            return added;
        }
    }
    auto status = registry.add(
        runtime_inspection::InspectionDescriptor {
            .id = std::string(c_playtest_interfaces_id),
            .label = "Discover Playtest Interfaces",
            .description =
                "Lists project and runtime playtest action contracts.",
            .schema = std::string(c_playtest_interfaces_schema),
            .read_only = true,
            .cost = runtime_inspection::InspectionCost::Low,
            .request_schema_json =
                R"({"type":"object","additionalProperties":false})",
            .response_schema_json =
                R"({"type":"object","properties":{"interfaces":{"type":"array"}},"required":["interfaces"],"additionalProperties":false})",
        },
        inspect_playtest_interfaces
    );
    if (!status) {
        return status;
    }
    status = registry.add(
        runtime_inspection::InspectionDescriptor {
            .id = std::string(c_playtest_capture_id),
            .label = "Capture Playtest Frame",
            .description = "Captures the currently presented frame without "
                           "advancing or rendering the game.",
            .schema = std::string(c_playtest_capture_schema),
            .read_only = true,
            .cost = runtime_inspection::InspectionCost::Moderate,
            .request_schema_json =
                R"({"type":"object","additionalProperties":false})",
            .response_schema_json =
                R"({"type":"object","properties":{"frame":{"type":"integer"},"width":{"type":"integer"},"height":{"type":"integer"},"format":{"const":"png"}},"required":["frame","width","height","format"],"additionalProperties":false})",
        },
        [](World&, std::string_view)
            -> Result<std::string, runtime_inspection::InspectionError> {
            return failure(
                runtime_inspection::InspectionError {
                    .kind = runtime_inspection::InspectionErrorKind::Internal,
                    .message =
                        "play.capture requires Runtime Host manual dispatch",
                }
            );
        }
    );
    if (!status) {
        return status;
    }
    status = registry.add(
        runtime_inspection::InspectionDescriptor {
            .id = std::string(c_playtest_observe_id),
            .label = "Observe Playtest",
            .description = "Returns the current structured observation for a "
                           "discovered playtest interface without advancing "
                           "the game.",
            .schema = std::string(c_playtest_observe_schema),
            .read_only = true,
            .cost = runtime_inspection::InspectionCost::Low,
            .request_schema_json =
                R"({"type":"object","properties":{"interface":{"type":"string"}},"required":["interface"],"additionalProperties":false})",
            .response_schema_json =
                R"({"type":"object","properties":{"interface":{"type":"string"},"frame":{"type":"integer"},"observation":{}},"required":["interface","frame","observation"],"additionalProperties":false})",
        },
        [](World&, std::string_view)
            -> Result<std::string, runtime_inspection::InspectionError> {
            return failure(
                runtime_inspection::InspectionError {
                    .kind = runtime_inspection::InspectionErrorKind::Internal,
                    .message =
                        "play.observe requires Runtime Host manual dispatch",
                }
            );
        }
    );
    if (!status) {
        return status;
    }
    status = registry.add(
        runtime_inspection::InspectionDescriptor {
            .id = std::string(c_playtest_step_id),
            .label = "Step Playtest",
            .description = "Applies one discovered action, advances a bounded "
                           "number of deterministic ticks, and pauses again.",
            .schema = std::string(c_playtest_step_schema),
            .read_only = false,
            .cost = runtime_inspection::InspectionCost::Moderate,
            .request_schema_json =
                R"({"type":"object","properties":{"interface":{"type":"string"},"action":{},"ticks":{"type":"integer","minimum":1}},"required":["interface","action"],"additionalProperties":false})",
            .response_schema_json =
                R"({"type":"object","properties":{"interface":{"type":"string"},"ticks":{"type":"integer"},"frame":{"type":"integer"},"stopped":{"type":"boolean"},"observation":{}},"required":["interface","ticks","frame","stopped","observation"],"additionalProperties":false})",
        },
        [](World&, std::string_view)
            -> Result<std::string, runtime_inspection::InspectionError> {
            return failure(
                runtime_inspection::InspectionError {
                    .kind = runtime_inspection::InspectionErrorKind::Internal,
                    .message =
                        "play.step requires Runtime Host manual dispatch",
                }
            );
        }
    );
    if (!status) {
        return status;
    }
    return registry.add(
        runtime_inspection::InspectionDescriptor {
            .id = std::string(c_playtest_segment_id),
            .label = "Run Playtest Segment",
            .description = "Runs an isolated reactive Luau controller once per "
                           "fixed tick and returns the final observation.",
            .schema = std::string(c_playtest_segment_schema),
            .read_only = false,
            .cost = runtime_inspection::InspectionCost::High,
            .request_schema_json =
                R"({"type":"object","properties":{"interface":{"type":"string"},"source":{"type":"string","maxLength":65536},"max_ticks":{"type":"integer","minimum":1,"maximum":600}},"required":["interface","source","max_ticks"],"additionalProperties":false})",
            .response_schema_json =
                R"({"type":"object","properties":{"interface":{"type":"string"},"ticks":{"type":"integer"},"frame":{"type":"integer"},"reason":{"type":"string"},"observation":{}},"required":["interface","ticks","frame","reason","observation"],"additionalProperties":false})",
        },
        [](World&, std::string_view)
            -> Result<std::string, runtime_inspection::InspectionError> {
            return failure(
                runtime_inspection::InspectionError {
                    .kind = runtime_inspection::InspectionErrorKind::Internal,
                    .message =
                        "play.segment requires Runtime Host manual dispatch",
                }
            );
        }
    );
}

} // namespace

RuntimeHostApplication::RuntimeHostApplication(
    Project project,
    RuntimeHostOptions options
) {
    auto engine_build = read_environment_variable("ETS_RUNTIME_BUILD_ID");
    if (!engine_build) {
        auto detected_build = current_runtime_build_id();
        if (!detected_build) {
            throw std::runtime_error(
                "Failed to identify the runtime build: " +
                detected_build.error()
            );
        }
        engine_build = std::move(*detected_build);
    }
    auto archive_metadata =
        make_snapshot_archive_metadata(project, std::move(*engine_build));
    if (!archive_metadata) {
        throw std::runtime_error(
            "Failed to prepare snapshot archive metadata: " +
            archive_metadata.error()
        );
    }
    runtime_protocol::RuntimeProbeConfig runtime_probe_config {
        .project = project.config().name,
        .project_file = project.project_file().generic_string(),
        .build_id = archive_metadata->engine_build,
        .inspection_handler = inspect_runtime,
    };
    runtime_inspection::InspectionRegistry inspection_registry;
    auto registration =
        runtime_inspection::ecs::register_entity_inspection_provider(
            inspection_registry
        );
    if (!registration) {
        fatal(
            "Failed to register runtime inspection provider: {}",
            registration.error().message
        );
    }
    registration = runtime_inspection::ecs::register_query_inspection_provider(
        inspection_registry
    );
    if (!registration) {
        fatal(
            "Failed to register runtime inspection provider: {}",
            registration.error().message
        );
    }
    registration =
        runtime_inspection::ecs::register_world_summary_inspection_provider(
            inspection_registry
        );
    if (!registration) {
        fatal(
            "Failed to register runtime inspection provider: {}",
            registration.error().message
        );
    }
    registration = register_playtest_inspection_providers(inspection_registry);
    if (!registration) {
        fatal(
            "Failed to register runtime inspection provider: {}",
            registration.error().message
        );
    }
    registration = runtime_inspection::checkpoint::
        register_checkpoint_inspection_providers(inspection_registry);
    if (!registration) {
        fatal(
            "Failed to register runtime inspection provider: {}",
            registration.error().message
        );
    }
    inspection_registry.freeze();
    runtime_probe_config.inspections.reserve(
        inspection_registry.descriptors().size()
    );
    for (const auto& descriptor : inspection_registry.descriptors()) {
        runtime_probe_config.inspections.push_back(
            runtime_protocol::InspectionCapability {
                .id = descriptor.id,
                .label = descriptor.label,
                .description = descriptor.description,
                .schema = descriptor.schema,
                .read_only = descriptor.read_only,
                .cost = std::string(
                    runtime_inspection::inspection_cost_name(descriptor.cost)
                ),
                .request_schema_json = descriptor.request_schema_json,
                .response_schema_json = descriptor.response_schema_json,
            }
        );
    }

    runtime_protocol::PlaytestRegistry playtest_registry;
    register_builtin_playtest_interfaces(playtest_registry);

    m_app
        .add_resource(std::move(inspection_registry))
        // This host owns deterministic stepping in its manual inspection loop.
        // The shared runner must not pause the clock underneath that loop.
        .add_resource(
            runtime_protocol::PlaytestConfig {
                .mode = runtime_protocol::PlaytestMode::Interactive,
            }
        )
        .add_resource(std::move(playtest_registry))
        .add_resource(std::move(*archive_metadata))
        .add_resource(snapshot::CheckpointStore {})
        .add_resource(QuickSaveRequests {})
        .add_resource(QuickSaveHotkeyLatch {})
        .add_resource(
            GlfwWindowConfig {
                .width = 1600,
                .height = 900,
                .title = "Entisium Runtime Host",
                .hints =
                    {{GLFW_VISIBLE, options.hidden ? GLFW_FALSE : GLFW_TRUE}},
            }
        )
        .add_systems(Last, request_quick_save_hotkeys);
    m_app.add_plugin<snapshot_runtime::SnapshotRuntimePlugin>();
    m_app.add_plugin(LuauScriptingPlugin {options.luau_config});
    configure_project_runtime(m_app, std::move(project));
    m_app.add_plugin<OpenGLGlfwPlugin>()
        .add_plugin<RenderingPlugin>()
        .add_plugin<SpritePlugin>()
        .add_plugin<GlfwInputPlugin>()
        .add_plugin<TimePlugin>()
        .add_plugin<PhysicsPlugin2d>()
        .add_plugin<ui::rendering::UiRenderingPlugin>()
        .add_plugin<ui_widgets::ButtonPlugin>()
        .add_plugin<project_runtime::LuauPlaytestsPlugin>();
    runtime_probe_config.manual_inspection_dispatch = true;
    m_app.add_plugin(
        runtime_protocol::RuntimeProbePlugin {
            std::move(runtime_probe_config),
        }
    );
}

RuntimeHostApplication::~RuntimeHostApplication() {
    shutdown();
}

void RuntimeHostApplication::shutdown() noexcept {
    m_app.shutdown();
}

void RuntimeHostApplication::update_frame() {
    m_app.update();
    const auto quick_save = process_quick_save_requests(m_app.world());
    switch (quick_save.kind) {
        case QuickSaveOutcomeKind::Saved:
        case QuickSaveOutcomeKind::Restored:
            info("{}", quick_save.message);
            break;
        case QuickSaveOutcomeKind::Failed:
            warn("{}", quick_save.message);
            break;
        case QuickSaveOutcomeKind::None:
            break;
    }
    if (quick_save.kind == QuickSaveOutcomeKind::Restored &&
        m_app.has_sub_app<RenderApp>()) {
        m_app.sub_app<RenderApp>().invalidate_source();
    }
    m_app.render();
}

void RuntimeHostApplication::run() {
    if (m_app.lifecycle() == AppLifecycle::Stopped) {
        return;
    }

    const auto exit_after_seconds =
        read_environment_variable<double>("ETS_EXIT_AFTER_SECONDS");
    const auto exit_after_frames =
        read_environment_variable<std::uint64_t>("ETS_EXIT_AFTER_FRAMES");
    const auto start_time = std::chrono::steady_clock::now();
    std::uint64_t frame_count = 0;

    try {
        m_app.startup();
        auto& checkpoint_store = m_app.resource<snapshot::CheckpointStore>();
        auto& snapshot_registry = checkpoint_store.registry();
        snapshot_registry.resource<AppStates>(snapshot::ResourcePolicy::Ignore);
        snapshot_registry.resource<CommandsQueue>(
            snapshot::ResourcePolicy::Ignore
        );
        snapshot_registry.resource<runtime_inspection::InspectionRegistry>(
            snapshot::ResourcePolicy::Ignore
        );
        snapshot_registry.resource<runtime_protocol::PlaytestRegistry>(
            snapshot::ResourcePolicy::Ignore
        );
        snapshot_registry.resource<runtime_protocol::RuntimeProbe>(
            snapshot::ResourcePolicy::Ignore
        );
        snapshot_registry.resource<snapshot::CheckpointStore>(
            snapshot::ResourcePolicy::Ignore
        );
        snapshot_registry.resource<snapshot::SnapshotArchiveMetadata>(
            snapshot::ResourcePolicy::Ignore
        );
        snapshot_registry.resource<QuickSaveRequests>(
            snapshot::ResourcePolicy::Ignore
        );
        snapshot_registry.resource<QuickSaveHotkeyLatch>(
            snapshot::ResourcePolicy::Ignore
        );
        snapshot_registry.resource<Project>(snapshot::ResourcePolicy::Ignore);
        snapshot_registry.resource<Window>(snapshot::ResourcePolicy::Ignore);
        snapshot_registry.resource<GlfwWindow>(
            snapshot::ResourcePolicy::Ignore
        );
        snapshot_registry.resource<GlfwWindowConfig>(
            snapshot::ResourcePolicy::Ignore
        );
        if (auto configured = snapshot_runtime::configure_builtin_adapters(
                m_app.world(),
                snapshot_registry
            );
            !configured) {
            throw std::runtime_error(
                "Failed to configure snapshot runtime adapters: " +
                configured.error().message
            );
        }
        if (m_app.has_resource<AssetServer>()) {
            if (auto configured =
                    snapshot_runtime_asset::configure_asset_adapters(
                        m_app.world(),
                        snapshot_registry
                    );
                !configured) {
                throw std::runtime_error(
                    "Failed to configure asset snapshot adapters: " +
                    configured.error().message
                );
            }
        }
        if (m_app.has_sub_app<RenderApp>()) {
            if (auto configured =
                    snapshot_runtime_rendering::configure_rendering_adapters(
                        m_app.world(),
                        snapshot_registry
                    );
                !configured) {
                throw std::runtime_error(
                    "Failed to configure rendering snapshot adapters: " +
                    configured.error().message
                );
            }
        }
        if (m_app.has_resource<PhysicsWorld2d>()) {
            if (auto configured =
                    snapshot_runtime_physics2d::configure_physics2d_adapters(
                        m_app.world(),
                        snapshot_registry
                    );
                !configured) {
                throw std::runtime_error(
                    "Failed to configure Physics2d snapshot adapters: " +
                    configured.error().message
                );
            }
        }
        if (m_app.has_resource<ui::Surface>()) {
            if (auto configured = snapshot_runtime_ui::configure_ui_adapters(
                    m_app.world(),
                    snapshot_registry
                );
                !configured) {
                throw std::runtime_error(
                    "Failed to configure UI snapshot adapters: " +
                    configured.error().message
                );
            }
        }
        if (m_app.has_resource<LuauRuntime>()) {
            if (auto configured =
                    snapshot_runtime_luau::configure_luau_adapters(
                        m_app.world(),
                        snapshot_registry
                    );
                !configured) {
                throw std::runtime_error(
                    "Failed to configure Luau snapshot adapters: " +
                    configured.error().message
                );
            }
            if (m_app.has_resource<project_runtime::LuauScriptsState>()) {
                snapshot_registry.resource<project_runtime::LuauScriptsState>(
                    snapshot::ResourcePolicy::Ignore
                );
            }
        }
        auto& probe = m_app.resource<runtime_protocol::RuntimeProbe>();
        const auto supervised = probe.status().enabled;
        if (supervised && m_app.has_resource<Time>()) {
            auto& time = m_app.resource<Time>();
            time.set_fixed_delta(c_playtest_fixed_delta);
            time.reset_elapsed_time();
        }
        if (supervised && m_app.has_resource<VirtualInput>()) {
            auto& input = m_app.resource<VirtualInput>();
            input.set_exclusive(true);
            input.clear();
        }
        m_app.resource<runtime_protocol::PlaytestRegistry>().freeze();

        auto execute_playtest_capture =
            [this,
             &frame_count](const runtime_protocol::InspectionRequest& request)
            -> Result<
                EncodedFrameCapture,
                runtime_protocol::RuntimeInspectionError> {
            try {
                const auto payload = Json::parse(request.payload_json);
                if (!payload.is_object() || !payload.empty()) {
                    return failure(
                        runtime_protocol::RuntimeInspectionError {
                            .kind = "invalid_request",
                            .message =
                                "play.capture request must be an empty object",
                        }
                    );
                }
            } catch (const std::exception& error) {
                return failure(
                    runtime_protocol::RuntimeInspectionError {
                        .kind = "invalid_request",
                        .message =
                            std::string("Invalid play.capture request: ") +
                            error.what(),
                    }
                );
            }

            if (!m_app.has_sub_app<RenderApp>()) {
                return failure(
                    runtime_protocol::RuntimeInspectionError {
                        .kind = "unsupported",
                        .message = "The project does not install a Render App",
                    }
                );
            }

            // Captures are observational and must represent the current Main
            // World, not the previously presented frame in the threaded
            // render pipeline. This refresh runs render schedules only.
            m_app.render();

            Result<TextureReadbackFrame, std::string> captured = failure(
                std::string("The Render App does not expose a main swapchain")
            );
            m_app.sub_app_runner<RenderApp>().run_on_execution_thread(
                [&captured](SubApp& render_app) {
                    if (!render_app.has_resource<GraphicsDevice>() ||
                        !render_app.has_resource<MainSwapchain>()) {
                        return;
                    }
                    const auto& read_only_render_app =
                        static_cast<const SubApp&>(render_app);
                    const auto& main_swapchain =
                        read_only_render_app.resource<MainSwapchain>();
                    if (!main_swapchain.swapchain) {
                        return;
                    }
                    captured =
                        read_only_render_app.resource<GraphicsDevice>()
                            .capture_presented_frame(*main_swapchain.swapchain);
                }
            );
            if (!captured) {
                return failure(
                    runtime_protocol::RuntimeInspectionError {
                        .kind = "unsupported",
                        .message = std::move(captured.error()),
                    }
                );
            }

            auto png = encode_png(*captured);
            if (!png) {
                return failure(
                    runtime_protocol::RuntimeInspectionError {
                        .kind = "internal",
                        .message = std::move(png.error()),
                    }
                );
            }
            if (png->size() >
                runtime_protocol::c_max_inspection_response_attachment_bytes) {
                return failure(
                    runtime_protocol::RuntimeInspectionError {
                        .kind = "internal",
                        .message = "Captured PNG exceeds the runtime protocol "
                                   "attachment limit",
                    }
                );
            }
            return EncodedFrameCapture {
                .payload_json =
                    Json {
                        {"frame", frame_count},
                        {"width", captured->width},
                        {"height", captured->height},
                        {"format", "png"},
                    }
                        .dump(),
                .png = std::move(*png),
            };
        };

        auto execute_playtest_observe =
            [this, &frame_count](
                const runtime_protocol::InspectionRequest& request
            ) -> Result<std::string, runtime_protocol::RuntimeInspectionError> {
            Json payload;
            try {
                payload = Json::parse(request.payload_json);
                if (!payload.is_object() || payload.size() != 1 ||
                    !payload.contains("interface") ||
                    !payload.at("interface").is_string()) {
                    return failure(
                        runtime_protocol::RuntimeInspectionError {
                            .kind = "invalid_request",
                            .message =
                                "play.observe requires only an interface",
                        }
                    );
                }
            } catch (const std::exception& error) {
                return failure(
                    runtime_protocol::RuntimeInspectionError {
                        .kind = "invalid_request",
                        .message =
                            std::string("Invalid play.observe request: ") +
                            error.what(),
                    }
                );
            }

            const auto interface_id =
                payload.at("interface").get<std::string>();
            const auto* interface =
                m_app.resource<runtime_protocol::PlaytestRegistry>().find(
                    interface_id
                );
            if (interface == nullptr) {
                return failure(
                    runtime_protocol::RuntimeInspectionError {
                        .kind = "unsupported",
                        .message =
                            "Unknown playtest interface '" + interface_id + "'",
                    }
                );
            }

            auto observation =
                invoke_playtest_observe(*interface, m_app.world());
            if (!observation) {
                return failure(playtest_error(observation.error()));
            }
            try {
                return Json {
                    {"interface", interface_id},
                    {"frame", frame_count},
                    {"observation", Json::parse(*observation)},
                }
                    .dump();
            } catch (const std::exception& error) {
                return failure(
                    runtime_protocol::RuntimeInspectionError {
                        .kind = "internal",
                        .message =
                            std::string("Playtest observation is invalid: ") +
                            error.what(),
                    }
                );
            }
        };

        auto execute_playtest_step =
            [this, &frame_count](
                const runtime_protocol::InspectionRequest& request,
                bool segment_tick = false
            ) -> Result<std::string, runtime_protocol::RuntimeInspectionError> {
            Json payload;
            try {
                payload = Json::parse(request.payload_json);
                if (!payload.is_object() ||
                    (payload.size() != 2 && payload.size() != 3) ||
                    !payload.contains("interface") ||
                    !payload.at("interface").is_string() ||
                    !payload.contains("action")) {
                    return failure(
                        runtime_protocol::RuntimeInspectionError {
                            .kind = "invalid_request",
                            .message =
                                "play.step requires interface and action, "
                                "with optional ticks",
                        }
                    );
                }
                for (const auto& [name, value] : payload.items()) {
                    (void)value;
                    if (name != "interface" && name != "action" &&
                        name != "ticks") {
                        return failure(
                            runtime_protocol::RuntimeInspectionError {
                                .kind = "invalid_request",
                                .message =
                                    "Unknown play.step field '" + name + "'",
                            }
                        );
                    }
                }
            } catch (const std::exception& error) {
                return failure(
                    runtime_protocol::RuntimeInspectionError {
                        .kind = "invalid_request",
                        .message = std::string("Invalid play.step request: ") +
                                   error.what(),
                    }
                );
            }

            auto& registry =
                m_app.resource<runtime_protocol::PlaytestRegistry>();
            const auto interface_id =
                payload.at("interface").get<std::string>();
            const auto* interface = registry.find(interface_id);
            if (interface == nullptr) {
                return failure(
                    runtime_protocol::RuntimeInspectionError {
                        .kind = "unsupported",
                        .message =
                            "Unknown playtest interface '" + interface_id + "'",
                    }
                );
            }

            const auto& descriptor = interface->descriptor;
            auto ticks = descriptor.decision_ticks;
            try {
                if (payload.contains("ticks")) {
                    if (!descriptor.allow_tick_override && !segment_tick) {
                        return failure(
                            runtime_protocol::RuntimeInspectionError {
                                .kind = "invalid_request",
                                .message = "Playtest interface '" +
                                           interface_id +
                                           "' uses a fixed decision interval",
                            }
                        );
                    }
                    ticks = payload.at("ticks").get<uint32>();
                }
            } catch (const std::exception& error) {
                return failure(
                    runtime_protocol::RuntimeInspectionError {
                        .kind = "invalid_request",
                        .message = std::string("Invalid play.step ticks: ") +
                                   error.what(),
                    }
                );
            }
            if (!segment_tick && (ticks < descriptor.minimum_ticks ||
                                  ticks > descriptor.maximum_ticks)) {
                return failure(
                    runtime_protocol::RuntimeInspectionError {
                        .kind = "invalid_request",
                        .message = "play.step ticks must be between " +
                                   std::to_string(descriptor.minimum_ticks) +
                                   " and " +
                                   std::to_string(descriptor.maximum_ticks),
                    }
                );
            }

            auto started = invoke_playtest_begin(
                *interface,
                m_app.world(),
                payload.at("action").dump()
            );
            if (!started) {
                return failure(playtest_error(started.error()));
            }

            uint32 completed_ticks = 0;
            try {
                for (; completed_ticks < ticks; ++completed_ticks) {
                    update_frame();
                    ++frame_count;
                    if (m_app.resource<AppStates>().should_stop) {
                        ++completed_ticks;
                        break;
                    }
                }
            } catch (const std::exception& error) {
                auto ended = invoke_playtest_end(*interface, m_app.world());
                auto message =
                    std::string("Playtest step failed: ") + error.what();
                if (!ended) {
                    message +=
                        "; input cleanup failed: " + ended.error().message;
                }
                return failure(
                    runtime_protocol::RuntimeInspectionError {
                        .kind = "internal",
                        .message = std::move(message),
                    }
                );
            } catch (...) {
                auto ended = invoke_playtest_end(*interface, m_app.world());
                auto message = std::string(
                    "Playtest step failed with an unknown exception"
                );
                if (!ended) {
                    message +=
                        "; input cleanup failed: " + ended.error().message;
                }
                return failure(
                    runtime_protocol::RuntimeInspectionError {
                        .kind = "internal",
                        .message = std::move(message),
                    }
                );
            }

            auto ended = invoke_playtest_end(*interface, m_app.world());
            if (!ended) {
                return failure(playtest_error(ended.error()));
            }
            auto observation =
                invoke_playtest_observe(*interface, m_app.world());
            if (!observation) {
                return failure(playtest_error(observation.error()));
            }
            try {
                return Json {
                    {"interface", interface_id},
                    {"ticks", completed_ticks},
                    {"frame", frame_count},
                    {"stopped", m_app.resource<AppStates>().should_stop},
                    {"observation", Json::parse(*observation)},
                }
                    .dump();
            } catch (const std::exception& error) {
                return failure(
                    runtime_protocol::RuntimeInspectionError {
                        .kind = "internal",
                        .message =
                            std::string("Playtest observation is invalid: ") +
                            error.what(),
                    }
                );
            }
        };

        auto execute_playtest_segment =
            [&execute_playtest_observe, &execute_playtest_step](
                const runtime_protocol::InspectionRequest& request
            ) -> Result<std::string, runtime_protocol::RuntimeInspectionError> {
            const auto invalid = [](std::string message) {
                return failure(
                    runtime_protocol::RuntimeInspectionError {
                        .kind = "invalid_request",
                        .message = std::move(message),
                    }
                );
            };
            const auto payload = Json::parse(request.payload_json);
            if (!payload.is_object() || payload.size() != 3 ||
                !payload.contains("interface") ||
                !payload.at("interface").is_string() ||
                !payload.contains("source") ||
                !payload.at("source").is_string() ||
                !payload.contains("max_ticks") ||
                !payload.at("max_ticks").is_number_unsigned()) {
                return invalid(
                    "play.segment requires interface, source and max_ticks"
                );
            }
            const auto maximum = payload.at("max_ticks").get<uint64>();
            if (maximum < 1 || maximum > 600) {
                return invalid(
                    "play.segment max_ticks must be between 1 and 600"
                );
            }
            auto program = LuauPlaytestSegment::compile(
                payload.at("source").get<std::string>()
            );
            if (!program) {
                return invalid(program.error().message);
            }
            auto internal_request = request;
            internal_request.payload_json =
                Json {{"interface", payload.at("interface")}}.dump();
            auto observed = execute_playtest_observe(internal_request);
            if (!observed) {
                return failure(std::move(observed.error()));
            }
            auto state = Json::parse(*observed);
            uint32 ticks = 0;
            std::string reason = "max_ticks";
            while (ticks < maximum) {
                auto decision =
                    (*program)->next(state.at("observation").dump(), ticks);
                if (!decision) {
                    return invalid(decision.error().message);
                }
                if (decision->kind == LuauPlaytestSegmentDecisionKind::Stop) {
                    reason = decision->value;
                    break;
                }
                internal_request.payload_json =
                    Json {
                        {"interface", payload.at("interface")},
                        {"action", Json::parse(decision->value)},
                        {"ticks", 1},
                    }
                        .dump();
                auto step = execute_playtest_step(internal_request, true);
                if (!step) {
                    return failure(std::move(step.error()));
                }
                state = Json::parse(*step);
                ++ticks;
                if (state.value("stopped", false)) {
                    reason = "runtime_stopped";
                    break;
                }
            }
            return Json {
                {"interface", payload.at("interface")},
                {"ticks", ticks},
                {"frame", state.at("frame")},
                {"reason", reason},
                {"observation", state.at("observation")},
            }
                .dump();
        };

        auto dispatch_manual_inspection = [this,
                                           &frame_count,
                                           &execute_playtest_capture,
                                           &execute_playtest_observe,
                                           &execute_playtest_step,
                                           &execute_playtest_segment](
                                              const runtime_protocol::
                                                  InspectionRequest& request
                                          ) {
            runtime_protocol::InspectionResponse response {
                .session = request.session,
                .request_id = request.request_id,
            };
            try {
                if (request.provider == c_playtest_capture_id &&
                    request.schema == c_playtest_capture_schema) {
                    auto capture = execute_playtest_capture(request);
                    if (capture) {
                        response.ok = true;
                        response.payload_json =
                            std::move(capture->payload_json);
                        response.attachment_content_type = "image/png";
                        response.attachment = std::move(capture->png);
                    } else {
                        response.error_kind = std::move(capture.error().kind);
                        response.error_message =
                            std::move(capture.error().message);
                    }
                    return response;
                }
                Result<std::string, runtime_protocol::RuntimeInspectionError>
                    result;
                if (request.provider == "test.input" &&
                    request.schema == "test.input.v1") {
                    auto applied = begin_keyboard_step(
                        m_app.world(),
                        request.payload_json
                    );
                    if (!applied) {
                        result = failure(playtest_error(applied.error()));
                    } else {
                        result = std::string {"{}"};
                    }
                } else if (
                    request.provider == "test.advance" &&
                    request.schema == "test.advance.v1"
                ) {
                    const auto payload = Json::parse(request.payload_json);
                    if (!payload.is_object() || payload.size() != 1 ||
                        !payload.contains("ticks") ||
                        !payload.at("ticks").is_number_unsigned()) {
                        throw std::runtime_error(
                            "test.advance requires an integer ticks field"
                        );
                    }
                    const auto ticks = payload.at("ticks").get<uint64>();
                    if (ticks < 1 || ticks > 600) {
                        throw std::runtime_error(
                            "test.advance ticks must be between 1 and 600"
                        );
                    }
                    uint32 completed = 0;
                    for (; completed < ticks &&
                           !m_app.resource<AppStates>().should_stop;
                         ++completed) {
                        update_frame();
                        ++frame_count;
                    }
                    result = Json {
                        {"ticks", completed},
                        {"delta", c_playtest_fixed_delta},
                        {"frame", frame_count},
                        {"stopped", m_app.resource<AppStates>().should_stop}
                    }.dump();
                } else if (
                    request.provider == "test.snapshot" &&
                    request.schema == "test.snapshot.v1"
                ) {
                    auto snapshot = Json::parse(inspect_test_snapshot(
                        m_app.world(),
                        request.payload_json
                    ));
                    snapshot["frame"] = frame_count;
                    snapshot["simulation_time"] =
                        static_cast<double>(frame_count) *
                        c_playtest_fixed_delta;
                    result = snapshot.dump();
                } else if (
                    request.provider == c_playtest_observe_id &&
                    request.schema == c_playtest_observe_schema
                ) {
                    result = execute_playtest_observe(request);
                } else if (
                    request.provider == c_playtest_step_id &&
                    request.schema == c_playtest_step_schema
                ) {
                    result = execute_playtest_step(request);
                } else if (
                    request.provider == c_playtest_segment_id &&
                    request.schema == c_playtest_segment_schema
                ) {
                    result = execute_playtest_segment(request);
                } else {
                    result = inspect_runtime(m_app.world(), request);
                }
                if (result) {
                    if (request.provider == c_checkpoint_restore_id &&
                        request.schema == c_checkpoint_restore_schema &&
                        m_app.has_sub_app<RenderApp>()) {
                        // A supervised runtime is paused between play steps.
                        // Re-extract and present the restored World without
                        // running game schedules or consuming a simulation
                        // tick, so the next capture reflects the checkpoint.
                        m_app.sub_app<RenderApp>().invalidate_source();
                        m_app.render();
                    }
                    response.ok = true;
                    response.payload_json = std::move(*result);
                } else {
                    response.error_kind = std::move(result.error().kind);
                    response.error_message = std::move(result.error().message);
                }
            } catch (const std::exception& error) {
                response.error_kind = "internal";
                response.error_message =
                    std::string("Inspection handler failed: ") + error.what();
            } catch (...) {
                response.error_kind = "internal";
                response.error_message =
                    "Inspection handler failed with an unknown exception";
            }
            return response;
        };

        if (supervised) {
            update_frame();
            ++frame_count;
        }
        bool should_stop = false;
        while (!should_stop) {
            if (supervised) {
                if (auto request = probe.wait_for_inspection(
                        std::chrono::milliseconds(10)
                    )) {
                    probe.complete_inspection(
                        dispatch_manual_inspection(*request)
                    );
                }
                if (m_app.has_resource<GlfwWindow>()) {
                    glfwPollEvents();
                    if (glfwWindowShouldClose(
                            m_app.resource<GlfwWindow>().handle
                        )) {
                        m_app.resource<AppStates>().should_stop = true;
                    }
                }
            } else {
                update_frame();
                ++frame_count;
            }

            auto& app_states = m_app.resource<AppStates>();
            if (exit_after_frames && frame_count >= *exit_after_frames) {
                app_states.should_stop = true;
            }
            if (exit_after_seconds) {
                const auto elapsed = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - start_time
                );
                if (elapsed.count() >= *exit_after_seconds) {
                    app_states.should_stop = true;
                }
            }
            should_stop = app_states.should_stop;
        }
    } catch (...) {
        shutdown();
        throw;
    }
    shutdown();
}

} // namespace ets::runtime_host
