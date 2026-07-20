#include "moltenvk_capability_collector.h"

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#if defined(__APPLE__)
#include <dlfcn.h>
#include <mach-o/dyld.h>
#endif

using json = nlohmann::json;

#if defined(__APPLE__) && UINTPTR_MAX == UINT64_MAX

namespace {

// MoltenVK 1.4.1 reports private API version 43. This minimal ABI snapshot is
// intentionally local to the collector because the packaged runtime, rather
// than the newer reference source checkout, defines the structure we query.
// MoltenVK guarantees that MVKConfiguration only grows by appending fields and
// provides a size-negotiation call for detecting an incompatible runtime.
// The member layout is derived from MoltenVK's mvk_private_api.h, copyright
// The Brenwill Workshop Ltd. and distributed under the Apache License 2.0.
constexpr std::uint32_t MOLTENVK_PRIVATE_API_VERSION = 43;
constexpr std::size_t MOLTENVK_CONFIGURATION_V43_SIZE = 184;
constexpr std::size_t MAX_MOLTENVK_CONFIGURATION_SIZE = 64 * 1024;
constexpr std::uint32_t MAX_LOADED_IMAGE_COUNT = 4096;
constexpr std::uint32_t MOLTENVK_ADVERTISE_EXTENSION_MASK = 0x7;

struct LoadedImageRegistry {
    std::mutex mutex;
    std::array<const mach_header*, MAX_LOADED_IMAGE_COUNT> headers{};
    std::size_t count = 0;
    std::uint64_t generation = 0;
    bool overflowed = false;
};

LoadedImageRegistry& loadedImageRegistry() {
    // dyld retains the callbacks for the process lifetime, so the registry must
    // outlive normal static destruction.
    static auto* registry = new LoadedImageRegistry;
    return *registry;
}

void loadedImageAdded(const mach_header* header, std::intptr_t slide) {
    static_cast<void>(slide);
    auto& registry = loadedImageRegistry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    auto end = registry.headers.begin() + registry.count;
    if(std::find(registry.headers.begin(), end, header) != end) {
        return;
    }
    ++registry.generation;
    if(registry.count == registry.headers.size()) {
        registry.overflowed = true;
        return;
    }
    registry.headers[registry.count++] = header;
}

void loadedImageRemoved(const mach_header* header, std::intptr_t slide) {
    static_cast<void>(slide);
    auto& registry = loadedImageRegistry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    ++registry.generation;
    auto end = registry.headers.begin() + registry.count;
    auto found = std::find(registry.headers.begin(), end, header);
    if(found != end) {
        std::move(found + 1, end, found);
        --registry.count;
        registry.headers[registry.count] = nullptr;
    }
}

struct LoadedImageSnapshot {
    std::vector<const mach_header*> headers;
    std::uint64_t generation = 0;
    bool overflowed = false;
};

LoadedImageSnapshot snapshotLoadedImages() {
    static std::once_flag callbacksRegistered;
    std::call_once(callbacksRegistered, [] {
        loadedImageRegistry();
        // Register removal first so an image cannot disappear between the
        // existing-image callbacks and installation of the removal observer.
        _dyld_register_func_for_remove_image(loadedImageRemoved);
        _dyld_register_func_for_add_image(loadedImageAdded);
    });
    auto& registry = loadedImageRegistry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    LoadedImageSnapshot snapshot;
    snapshot.headers.assign(registry.headers.begin(),
                            registry.headers.begin() + registry.count);
    snapshot.generation = registry.generation;
    snapshot.overflowed = registry.overflowed;
    return snapshot;
}

bool loadedImagesChanged(std::uint64_t generation) {
    auto& registry = loadedImageRegistry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    return registry.overflowed || registry.generation != generation;
}

struct MoltenVkConfigurationV43 {
    std::uint32_t debugMode;
    std::uint32_t shaderConversionFlipVertexY;
    std::uint32_t synchronousQueueSubmits;
    std::uint32_t prefillMetalCommandBuffers;
    std::uint32_t maxActiveMetalCommandBuffersPerQueue;
    std::uint32_t supportLargeQueryPools;
    std::uint32_t presentWithCommandBuffer;
    std::uint32_t swapchainMinMagFilterUseNearest;
    std::uint64_t metalCompileTimeout;
    std::uint32_t performanceTracking;
    std::uint32_t performanceLoggingFrameCount;
    std::uint32_t displayWatermark;
    std::uint32_t specializedQueueFamilies;
    std::uint32_t switchSystemGPU;
    std::uint32_t fullImageViewSwizzle;
    std::uint32_t defaultGPUCaptureScopeQueueFamilyIndex;
    std::uint32_t defaultGPUCaptureScopeQueueIndex;
    std::uint32_t fastMathEnabled;
    std::uint32_t logLevel;
    std::uint32_t traceVulkanCalls;
    std::uint32_t forceLowPowerGPU;
    std::uint32_t semaphoreUseMTLFence;
    std::uint32_t semaphoreSupportStyle;
    std::uint32_t autoGPUCaptureScope;
    const char* autoGPUCaptureOutputFilepath;
    std::uint32_t texture1DAs2D;
    std::uint32_t preallocateDescriptors;
    std::uint32_t useCommandPooling;
    std::uint32_t useMTLHeap;
    std::uint32_t activityPerformanceLoggingStyle;
    std::uint32_t apiVersionToAdvertise;
    std::uint32_t advertiseExtensions;
    std::uint32_t resumeLostDevice;
    std::uint32_t useMetalArgumentBuffers;
    std::uint32_t shaderSourceCompressionAlgorithm;
    std::uint32_t shouldMaximizeConcurrentCompilation;
    float timestampPeriodLowPassAlpha;
    std::uint32_t useMetalPrivateAPI;
    const char* shaderDumpDir;
    std::uint32_t shaderLogEstimatedGLSL;
    std::uint32_t liveCheckAllResources;
};

static_assert(sizeof(MoltenVkConfigurationV43) == MOLTENVK_CONFIGURATION_V43_SIZE,
              "MoltenVK private API v43 structure layout changed");
static_assert(offsetof(MoltenVkConfigurationV43, metalCompileTimeout) == 32,
              "MoltenVK private API v43 integer layout changed");
static_assert(offsetof(MoltenVkConfigurationV43, autoGPUCaptureOutputFilepath) == 104,
              "MoltenVK private API v43 pointer layout changed");
static_assert(offsetof(MoltenVkConfigurationV43, shaderDumpDir) == 168,
              "MoltenVK private API v43 tail layout changed");

using GetMoltenVkConfiguration = VkResult(VKAPI_PTR *)(
    VkInstance, MoltenVkConfigurationV43*, std::size_t*);

void addError(MoltenVkCapabilityObservation& observation, const char* code,
              const char* message) {
    auto duplicate = std::find_if(observation.errors.begin(), observation.errors.end(),
                                  [code](const auto& error) {
                                      return std::strcmp(error.code, code) == 0;
                                  });
    if(duplicate == observation.errors.end()) {
        observation.errors.push_back({code, message});
    }
}

json makeEmptyData() {
    return {
        {"query_path", "angle_vulkan_bridge_moltenvk_private_api"},
        {"active_driver", {
            {"driver_id", "moltenvk"},
            {"source", "vk_physical_device_driver_properties"}
        }},
        {"private_api_resolution", nullptr},
        {"version", nullptr},
        {"configuration_abi", nullptr},
        {"configuration_scope", nullptr},
        {"configuration", nullptr},
        {"omitted_fields", {
            {"path_bearing", {"auto_gpu_capture_output_file", "shader_dump_dir"}},
            {"ignored_legacy", {"full_image_view_swizzle", "preallocate_descriptors",
                                "present_with_command_buffer", "semaphore_use_mtl_fence",
                                "support_large_query_pools"}}
        }}
    };
}

json vulkanApiVersion(std::uint32_t version) {
    return {
        {"variant", VK_API_VERSION_VARIANT(version)},
        {"major", VK_API_VERSION_MAJOR(version)},
        {"minor", VK_API_VERSION_MINOR(version)},
        {"patch", VK_API_VERSION_PATCH(version)}
    };
}

std::optional<const char*> prefillStyle(std::uint32_t value) {
    switch(value) {
    case 0: return "no_prefill";
    case 1: return "deferred_encoding";
    case 2: return "immediate_encoding";
    case 3: return "immediate_encoding_no_autorelease";
    default: return std::nullopt;
    }
}

std::optional<const char*> fastMathStyle(std::uint32_t value) {
    switch(value) {
    case 0: return "never";
    case 1: return "always";
    case 2: return "on_demand";
    default: return std::nullopt;
    }
}

std::optional<const char*> logLevel(std::uint32_t value) {
    switch(value) {
    case 0: return "none";
    case 1: return "error";
    case 2: return "warning";
    case 3: return "info";
    case 4: return "debug";
    default: return std::nullopt;
    }
}

std::optional<const char*> traceStyle(std::uint32_t value) {
    switch(value) {
    case 0: return "none";
    case 1: return "enter";
    case 2: return "enter_thread_id";
    case 3: return "enter_exit";
    case 4: return "enter_exit_thread_id";
    case 5: return "duration";
    case 6: return "duration_thread_id";
    default: return std::nullopt;
    }
}

std::optional<const char*> semaphoreStyle(std::uint32_t value) {
    switch(value) {
    case 0: return "single_queue";
    case 1: return "metal_events_where_safe";
    case 2: return "metal_events";
    case 3: return "callback";
    default: return std::nullopt;
    }
}

std::optional<const char*> captureScope(std::uint32_t value) {
    switch(value) {
    case 0: return "none";
    case 1: return "device";
    case 2: return "frame";
    case 3: return "on_demand";
    default: return std::nullopt;
    }
}

std::optional<const char*> mtlHeapStyle(std::uint32_t value) {
    switch(value) {
    case 0: return "never";
    case 1: return "where_safe";
    case 2: return "always";
    default: return std::nullopt;
    }
}

std::optional<const char*> performanceLoggingStyle(std::uint32_t value) {
    switch(value) {
    case 0: return "frame_count";
    case 1: return "immediate";
    case 2: return "device_lifetime";
    case 3: return "device_lifetime_accumulate";
    default: return std::nullopt;
    }
}

std::optional<const char*> compressionAlgorithm(std::uint32_t value) {
    switch(value) {
    case 0: return "none";
    case 1: return "lzfse";
    case 2: return "zlib";
    case 3: return "lz4";
    case 4: return "lzma";
    default: return std::nullopt;
    }
}

bool validBoolean(std::uint32_t value) {
    return value == VK_FALSE || value == VK_TRUE;
}

std::optional<json> encodeConfiguration(const MoltenVkConfigurationV43& value) {
    const std::uint32_t booleanValues[] = {
        value.debugMode, value.shaderConversionFlipVertexY,
        value.synchronousQueueSubmits, value.swapchainMinMagFilterUseNearest,
        value.performanceTracking, value.displayWatermark,
        value.specializedQueueFamilies, value.switchSystemGPU,
        value.forceLowPowerGPU, value.texture1DAs2D, value.useCommandPooling,
        value.resumeLostDevice, value.useMetalArgumentBuffers,
        value.shouldMaximizeConcurrentCompilation, value.useMetalPrivateAPI,
        value.shaderLogEstimatedGLSL, value.liveCheckAllResources
    };
    if(!std::all_of(std::begin(booleanValues), std::end(booleanValues), validBoolean)) {
        return std::nullopt;
    }

    auto prefill = prefillStyle(value.prefillMetalCommandBuffers);
    auto fastMath = fastMathStyle(value.fastMathEnabled);
    auto logging = logLevel(value.logLevel);
    auto tracing = traceStyle(value.traceVulkanCalls);
    auto semaphore = semaphoreStyle(value.semaphoreSupportStyle);
    auto capture = captureScope(value.autoGPUCaptureScope);
    auto heap = mtlHeapStyle(value.useMTLHeap);
    auto performanceStyle = performanceLoggingStyle(value.activityPerformanceLoggingStyle);
    auto compression = compressionAlgorithm(value.shaderSourceCompressionAlgorithm);
    if(!prefill || !fastMath || !logging || !tracing || !semaphore || !capture ||
       !heap || !performanceStyle || !compression ||
       (value.advertiseExtensions & ~MOLTENVK_ADVERTISE_EXTENSION_MASK) != 0 ||
       !std::isfinite(value.timestampPeriodLowPassAlpha) ||
       value.timestampPeriodLowPassAlpha < 0.0f ||
       value.timestampPeriodLowPassAlpha > 1.0f ||
       VK_API_VERSION_MAJOR(value.apiVersionToAdvertise) == 0) {
        return std::nullopt;
    }

    return json{
        {"debug_mode", value.debugMode == VK_TRUE},
        {"shader_conversion_flip_vertex_y", value.shaderConversionFlipVertexY == VK_TRUE},
        {"synchronous_queue_submits", value.synchronousQueueSubmits == VK_TRUE},
        {"prefill_metal_command_buffers", *prefill},
        {"max_active_metal_command_buffers_per_queue", value.maxActiveMetalCommandBuffersPerQueue},
        {"swapchain_min_mag_filter_use_nearest", value.swapchainMinMagFilterUseNearest == VK_TRUE},
        {"metal_compile_timeout_nanoseconds", std::to_string(value.metalCompileTimeout)},
        {"performance_tracking", value.performanceTracking == VK_TRUE},
        {"performance_logging_frame_count", value.performanceLoggingFrameCount},
        {"activity_performance_logging_style", *performanceStyle},
        {"display_watermark", value.displayWatermark == VK_TRUE},
        {"specialized_queue_families", value.specializedQueueFamilies == VK_TRUE},
        {"switch_system_gpu", value.switchSystemGPU == VK_TRUE},
        {"default_gpu_capture_scope_queue_family_index", value.defaultGPUCaptureScopeQueueFamilyIndex},
        {"default_gpu_capture_scope_queue_index", value.defaultGPUCaptureScopeQueueIndex},
        {"fast_math", *fastMath},
        {"log_level", *logging},
        {"trace_vulkan_calls", *tracing},
        {"force_low_power_gpu", value.forceLowPowerGPU == VK_TRUE},
        {"semaphore_support_style", *semaphore},
        {"auto_gpu_capture_scope", *capture},
        {"texture_1d_as_2d", value.texture1DAs2D == VK_TRUE},
        {"use_command_pooling", value.useCommandPooling == VK_TRUE},
        {"use_mtl_heap", *heap},
        {"api_version_to_advertise", vulkanApiVersion(value.apiVersionToAdvertise)},
        {"advertise_extensions", {
            {"raw", value.advertiseExtensions},
            {"all", (value.advertiseExtensions & 0x1) != 0},
            {"wsi", (value.advertiseExtensions & 0x2) != 0},
            {"portability", (value.advertiseExtensions & 0x4) != 0}
        }},
        {"resume_lost_device", value.resumeLostDevice == VK_TRUE},
        {"use_metal_argument_buffers", value.useMetalArgumentBuffers == VK_TRUE},
        {"shader_source_compression_algorithm", *compression},
        {"maximize_concurrent_compilation", value.shouldMaximizeConcurrentCompilation == VK_TRUE},
        {"timestamp_period_low_pass_alpha", value.timestampPeriodLowPassAlpha},
        {"use_metal_private_api", value.useMetalPrivateAPI == VK_TRUE},
        {"shader_log_estimated_glsl", value.shaderLogEstimatedGLSL == VK_TRUE},
        {"live_check_all_resources", value.liveCheckAllResources == VK_TRUE}
    };
}

}  // namespace

MoltenVkCapabilityObservation collectMoltenVkCapabilities(
    void* instanceHandle, MoltenVkHostFunction getInstanceProcAddressFunction,
    std::uint32_t physicalDeviceDriverVersion, bool activeDriverIsMoltenVk,
    const std::vector<std::string>* enabledInstanceExtensions) {
    MoltenVkCapabilityObservation observation;
    if(instanceHandle == nullptr || getInstanceProcAddressFunction == nullptr ||
       !activeDriverIsMoltenVk) {
        return observation;
    }

    auto getInstanceProcAddress =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(getInstanceProcAddressFunction);
    auto instance = reinterpret_cast<VkInstance>(instanceHandle);
    auto getConfiguration = reinterpret_cast<GetMoltenVkConfiguration>(
        getInstanceProcAddress(instance, "vkGetMoltenVKConfigurationMVK"));
    const char* resolutionPath = getConfiguration != nullptr
        ? "angle_instance_proc_addr" : nullptr;
    if(getConfiguration == nullptr) {
        getConfiguration = reinterpret_cast<GetMoltenVkConfiguration>(
            getInstanceProcAddress(VK_NULL_HANDLE,
                                   "vkGetMoltenVKConfigurationMVK"));
        if(getConfiguration != nullptr) {
            resolutionPath = "angle_global_proc_addr";
        }
    }

    struct ExistingImageHandle {
        void* value = nullptr;
        ~ExistingImageHandle() {
            if(value != nullptr) {
                dlclose(value);
            }
        }
    } existingImage;
    if(getConfiguration == nullptr) {
        GetMoltenVkConfiguration candidate = nullptr;
        void* candidateHandle = nullptr;
        bool ambiguous = false;
        auto snapshot = snapshotLoadedImages();
        if(!snapshot.overflowed) {
            for(const mach_header* imageHeader : snapshot.headers) {
                Dl_info imageInfo{};
                if(dladdr(imageHeader, &imageInfo) == 0 ||
                   imageInfo.dli_fname == nullptr ||
                   imageInfo.dli_fbase != imageHeader) {
                    continue;
                }
                void* handle = dlopen(
                    imageInfo.dli_fname,
                    RTLD_LAZY | RTLD_NOLOAD | RTLD_FIRST);
                if(handle == nullptr) {
                    continue;
                }
                void* configurationSymbol =
                    dlsym(handle, "vkGetMoltenVKConfigurationMVK");
                void* instanceProcAddressSymbol =
                    dlsym(handle, "vkGetInstanceProcAddr");
                Dl_info configurationInfo{};
                Dl_info instanceProcAddressInfo{};
                bool directlyExported = configurationSymbol != nullptr &&
                    instanceProcAddressSymbol != nullptr &&
                    dladdr(configurationSymbol, &configurationInfo) != 0 &&
                    dladdr(instanceProcAddressSymbol, &instanceProcAddressInfo) != 0 &&
                    configurationInfo.dli_fbase == imageHeader &&
                    instanceProcAddressInfo.dli_fbase == imageHeader;
                if(directlyExported) {
                    auto imageConfiguration =
                        reinterpret_cast<GetMoltenVkConfiguration>(
                            configurationSymbol);
                    if(candidate == nullptr) {
                        candidate = imageConfiguration;
                        candidateHandle = handle;
                        handle = nullptr;
                    } else {
                        ambiguous = true;
                    }
                }
                if(handle != nullptr) {
                    dlclose(handle);
                }
            }
        }
        ambiguous = ambiguous || loadedImagesChanged(snapshot.generation);
        if(!ambiguous && candidate != nullptr) {
            existingImage.value = candidateHandle;
            getConfiguration = candidate;
            resolutionPath = "unique_loaded_moltenvk_image_rtld_no_load";
        } else if(candidateHandle != nullptr) {
            dlclose(candidateHandle);
        }
        if(ambiguous) {
            getConfiguration = nullptr;
            resolutionPath = nullptr;
            if(existingImage.value != nullptr) {
                dlclose(existingImage.value);
                existingImage.value = nullptr;
            }
        }
    }
    if(getConfiguration == nullptr) {
        return observation;
    }

    observation.implementationDetected = true;
    observation.data = makeEmptyData();
    observation.data["private_api_resolution"] = resolutionPath;

    std::uint32_t encodedVersion = physicalDeviceDriverVersion;
    std::uint32_t versionMajor = encodedVersion / 10000;
    std::uint32_t versionMinor = (encodedVersion / 100) % 100;
    std::uint32_t versionPatch = encodedVersion % 100;
    if(encodedVersion != 0 && encodedVersion <= 999999 && versionMajor <= 99) {
        observation.data["version"] = {
            {"major", versionMajor},
            {"minor", versionMinor},
            {"patch", versionPatch},
            {"numeric", encodedVersion},
            {"source", "vk_physical_device_properties_driver_version"}
        };
    } else {
        addError(observation, "moltenvk_version_invalid",
                 "MoltenVK exposed an invalid encoded driver version");
    }

    std::size_t runtimeSize = 0;
    VkResult sizeResult = getConfiguration(VK_NULL_HANDLE, nullptr, &runtimeSize);
    if(sizeResult != VK_SUCCESS || runtimeSize == 0 ||
       runtimeSize > MAX_MOLTENVK_CONFIGURATION_SIZE) {
        addError(observation, "moltenvk_configuration_size_query_failed",
                 "MoltenVK did not expose its configuration structure size");
        return observation;
    }

    bool sizeMatches = runtimeSize == sizeof(MoltenVkConfigurationV43);
    observation.data["configuration_abi"] = {
        {"collector_private_api_version", MOLTENVK_PRIVATE_API_VERSION},
        {"collector_structure_size_bytes", sizeof(MoltenVkConfigurationV43)},
        {"runtime_structure_size_bytes", runtimeSize},
        {"size_match", sizeMatches}
    };

    bool layerSettingsKnown = enabledInstanceExtensions != nullptr;
    bool layerSettingsEnabled = layerSettingsKnown &&
        std::binary_search(enabledInstanceExtensions->begin(), enabledInstanceExtensions->end(),
                           "VK_EXT_layer_settings");
    observation.data["configuration_scope"] = {
        {"queried_scope", "process_global"},
        {"layer_settings_extension_enabled",
         layerSettingsKnown ? json(layerSettingsEnabled) : json(nullptr)},
        {"active_for_angle_instance",
         layerSettingsKnown && !layerSettingsEnabled ? json(true) : json(nullptr)}
    };
    if(!layerSettingsKnown) {
        addError(observation, "moltenvk_instance_configuration_scope_unknown",
                 "ANGLE's enabled instance extensions were unavailable, so the global MoltenVK configuration could not be proven active");
    } else if(layerSettingsEnabled) {
        addError(observation, "moltenvk_layer_settings_override_possible",
                 "VK_EXT_layer_settings was enabled, so MoltenVK's global configuration could not be proven active for ANGLE's instance");
    }

    if(!sizeMatches) {
        addError(observation, "moltenvk_configuration_abi_mismatch",
                 "The loaded MoltenVK configuration ABI did not match private API version 43");
        return observation;
    }

    MoltenVkConfigurationV43 configuration{};
    std::size_t copiedSize = sizeof(configuration);
    VkResult configurationResult =
        getConfiguration(VK_NULL_HANDLE, &configuration, &copiedSize);
    if(configurationResult != VK_SUCCESS || copiedSize != sizeof(configuration)) {
        addError(observation, "moltenvk_configuration_query_failed",
                 "MoltenVK did not return a complete compatible configuration snapshot");
        return observation;
    }
    auto encodedConfiguration = encodeConfiguration(configuration);
    if(encodedConfiguration) {
        observation.data["configuration"] = std::move(*encodedConfiguration);
    } else {
        addError(observation, "moltenvk_configuration_value_invalid",
                 "MoltenVK exposed an invalid boolean, enum, flag, version, or numeric configuration value");
    }

    observation.complete = observation.errors.empty() &&
        !observation.data["version"].is_null() &&
        !observation.data["active_driver"].is_null() &&
        !observation.data["configuration_abi"].is_null() &&
        !observation.data["configuration_scope"].is_null() &&
        observation.data["configuration_scope"]["active_for_angle_instance"] == true &&
        !observation.data["configuration"].is_null();
    return observation;
}

#else

MoltenVkCapabilityObservation collectMoltenVkCapabilities(
    void* instanceHandle, MoltenVkHostFunction getInstanceProcAddressFunction,
    std::uint32_t physicalDeviceDriverVersion, bool activeDriverIsMoltenVk,
    const std::vector<std::string>* enabledInstanceExtensions) {
    static_cast<void>(instanceHandle);
    static_cast<void>(getInstanceProcAddressFunction);
    static_cast<void>(physicalDeviceDriverVersion);
    static_cast<void>(activeDriverIsMoltenVk);
    static_cast<void>(enabledInstanceExtensions);
    return {};
}

#endif
