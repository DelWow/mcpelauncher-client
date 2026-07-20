#include "vulkan_capability_collector.h"

#define __ANDROID__
#include <EGL/egl.h>
#undef __ANDROID__

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using json = nlohmann::json;

namespace {

constexpr EGLint EGL_VULKAN_VERSION_ANGLE_VALUE = 0x34A8;
constexpr EGLint EGL_VULKAN_INSTANCE_ANGLE_VALUE = 0x34A9;
constexpr EGLint EGL_VULKAN_INSTANCE_EXTENSIONS_ANGLE_VALUE = 0x34AA;
constexpr EGLint EGL_VULKAN_PHYSICAL_DEVICE_ANGLE_VALUE = 0x34AB;
constexpr EGLint EGL_VULKAN_DEVICE_ANGLE_VALUE = 0x34AC;
constexpr EGLint EGL_VULKAN_DEVICE_EXTENSIONS_ANGLE_VALUE = 0x34AD;
constexpr EGLint EGL_VULKAN_FEATURES_ANGLE_VALUE = 0x34AE;
constexpr EGLint EGL_VULKAN_QUEUE_ANGLE_VALUE = 0x34AF;
constexpr EGLint EGL_VULKAN_QUEUE_FAMILY_INDEX_ANGLE_VALUE = 0x34D0;
constexpr EGLint EGL_VULKAN_GET_INSTANCE_PROC_ADDR_VALUE = 0x34D1;

constexpr std::size_t MAX_ENABLED_EXTENSION_COUNT = 1024;
constexpr std::size_t MAX_AVAILABLE_EXTENSION_COUNT = 4096;
constexpr std::size_t MAX_EXTENSION_NAME_LENGTH = 255;
constexpr std::size_t MAX_ENABLED_EXTENSION_BYTES = 64 * 1024;
constexpr std::size_t MAX_AVAILABLE_EXTENSION_BYTES = 1024 * 1024;
constexpr std::size_t MAX_QUEUE_FAMILY_COUNT = 256;

using EglQueryDeviceAttribute = EGLBoolean(EGLAPIENTRY*)(void*, EGLint, EGLAttrib*);
using EglGetError = EGLint(EGLAPIENTRY*)();

void addError(VulkanCapabilityObservation& observation, const char* code,
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
        {"query_path", "angle_egl_vulkan_device_bridge"},
        {"versions", nullptr},
        {"physical_device", nullptr},
        {"instance_extensions", nullptr},
        {"device_extensions", nullptr},
        {"queues", nullptr},
        {"memory", nullptr},
        {"features", nullptr},
        {"formats", nullptr},
        {"limits", nullptr}};
}

std::optional<EGLAttrib> queryBridgeAttribute(
    VulkanCapabilityObservation& observation, EglQueryDeviceAttribute query,
    EglGetError getError, void* device, EGLint attribute, const char* errorCode,
    const char* errorMessage) {
    EGLAttrib value = 0;
    if(query(device, attribute, &value) == EGL_FALSE) {
        if(getError != nullptr) {
            getError();
        }
        addError(observation, errorCode, errorMessage);
        return std::nullopt;
    }
    observation.bridgeDataAvailable = true;
    return value;
}

bool validExtensionName(const char* value, std::size_t maximumLength,
                        std::string& result) {
    if(value == nullptr) {
        return false;
    }
    std::size_t length = 0;
    while(length <= maximumLength && value[length] != '\0') {
        auto character = static_cast<unsigned char>(value[length]);
        if(!((character >= 'A' && character <= 'Z') ||
             (character >= 'a' && character <= 'z') ||
             (character >= '0' && character <= '9') || character == '_')) {
            return false;
        }
        ++length;
    }
    if(length < 4 || length > maximumLength || std::strncmp(value, "VK_", 3) != 0) {
        return false;
    }
    result.assign(value, length);
    return true;
}

std::optional<std::vector<std::string>> readEnabledExtensions(EGLAttrib attribute) {
    if(attribute == 0) {
        return std::nullopt;
    }
    auto values = reinterpret_cast<const char* const*>(attribute);
    std::vector<std::string> extensions;
    std::size_t aggregateBytes = 0;
    for(std::size_t index = 0; index <= MAX_ENABLED_EXTENSION_COUNT; ++index) {
        const char* value = values[index];
        if(value == nullptr) {
            std::sort(extensions.begin(), extensions.end());
            if(std::adjacent_find(extensions.begin(), extensions.end()) != extensions.end()) {
                return std::nullopt;
            }
            return extensions;
        }
        if(index == MAX_ENABLED_EXTENSION_COUNT) {
            return std::nullopt;
        }
        std::string extension;
        if(!validExtensionName(value, MAX_EXTENSION_NAME_LENGTH, extension) ||
           aggregateBytes > MAX_ENABLED_EXTENSION_BYTES - extension.size()) {
            return std::nullopt;
        }
        aggregateBytes += extension.size();
        extensions.push_back(std::move(extension));
    }
    return std::nullopt;
}

json apiVersion(std::uint32_t version) {
    return {
        {"variant", VK_API_VERSION_VARIANT(version)},
        {"major", VK_API_VERSION_MAJOR(version)},
        {"minor", VK_API_VERSION_MINOR(version)},
        {"patch", VK_API_VERSION_PATCH(version)}};
}

const char* physicalDeviceType(VkPhysicalDeviceType type) {
    switch(type) {
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        return "integrated_gpu";
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        return "discrete_gpu";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        return "virtual_gpu";
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
        return "cpu";
    case VK_PHYSICAL_DEVICE_TYPE_OTHER:
    default:
        return "other";
    }
}

std::optional<std::string> boundedDeviceName(
    const char (&value)[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE]) {
    std::size_t length = 0;
    while(length < VK_MAX_PHYSICAL_DEVICE_NAME_SIZE && value[length] != '\0') {
        auto character = static_cast<unsigned char>(value[length]);
        if(character < 0x20 || character > 0x7E) {
            return std::nullopt;
        }
        ++length;
    }
    if(length == 0 || length == VK_MAX_PHYSICAL_DEVICE_NAME_SIZE) {
        return std::nullopt;
    }
    return std::string(value, length);
}

struct ExtensionRecord {
    std::string name;
    std::uint32_t specVersion;
};

std::optional<std::vector<ExtensionRecord>> encodeExtensionProperties(
    const std::vector<VkExtensionProperties>& properties) {
    std::vector<ExtensionRecord> extensions;
    extensions.reserve(properties.size());
    std::size_t aggregateBytes = 0;
    for(const auto& property : properties) {
        std::string name;
        if(!validExtensionName(property.extensionName, MAX_EXTENSION_NAME_LENGTH, name) ||
           aggregateBytes > MAX_AVAILABLE_EXTENSION_BYTES - name.size()) {
            return std::nullopt;
        }
        aggregateBytes += name.size();
        extensions.push_back({std::move(name), property.specVersion});
    }
    std::sort(extensions.begin(), extensions.end(), [](const auto& left, const auto& right) {
        if(left.name != right.name) {
            return left.name < right.name;
        }
        return left.specVersion > right.specVersion;
    });
    if(std::adjacent_find(extensions.begin(), extensions.end(),
                          [](const auto& left, const auto& right) {
                              return left.name == right.name;
                          }) != extensions.end()) {
        return std::nullopt;
    }
    return extensions;
}

json extensionRecordsToJson(const std::vector<ExtensionRecord>& extensions) {
    json result = json::array();
    for(const auto& extension : extensions) {
        result.push_back({{"name", extension.name},
                          {"spec_version", extension.specVersion}});
    }
    return result;
}

bool extensionSetContains(const std::vector<ExtensionRecord>& extensions,
                          const char* name) {
    return std::any_of(extensions.begin(), extensions.end(),
                       [name](const auto& extension) {
                           return extension.name == name;
                       });
}

std::optional<std::vector<ExtensionRecord>> enumerateInstanceExtensions(
    PFN_vkEnumerateInstanceExtensionProperties enumerate) {
    std::uint32_t count = 0;
    if(enumerate(nullptr, &count, nullptr) != VK_SUCCESS ||
       count > MAX_AVAILABLE_EXTENSION_COUNT) {
        return std::nullopt;
    }
    std::vector<VkExtensionProperties> properties(count);
    std::uint32_t returnedCount = count;
    if(enumerate(nullptr, &returnedCount, count == 0 ? nullptr : properties.data()) != VK_SUCCESS ||
       returnedCount != count) {
        return std::nullopt;
    }
    return encodeExtensionProperties(properties);
}

std::optional<std::vector<ExtensionRecord>> enumerateDeviceExtensions(
    PFN_vkEnumerateDeviceExtensionProperties enumerate,
    VkPhysicalDevice physicalDevice) {
    std::uint32_t count = 0;
    if(enumerate(physicalDevice, nullptr, &count, nullptr) != VK_SUCCESS ||
       count > MAX_AVAILABLE_EXTENSION_COUNT) {
        return std::nullopt;
    }
    std::vector<VkExtensionProperties> properties(count);
    std::uint32_t returnedCount = count;
    if(enumerate(physicalDevice, nullptr, &returnedCount,
                 count == 0 ? nullptr : properties.data()) != VK_SUCCESS ||
       returnedCount != count) {
        return std::nullopt;
    }
    return encodeExtensionProperties(properties);
}

json encodeFeatures(const VkPhysicalDeviceFeatures& features) {
    return {
        {"robust_buffer_access", features.robustBufferAccess != VK_FALSE},
        {"full_draw_index_uint32", features.fullDrawIndexUint32 != VK_FALSE},
        {"image_cube_array", features.imageCubeArray != VK_FALSE},
        {"independent_blend", features.independentBlend != VK_FALSE},
        {"geometry_shader", features.geometryShader != VK_FALSE},
        {"tessellation_shader", features.tessellationShader != VK_FALSE},
        {"sample_rate_shading", features.sampleRateShading != VK_FALSE},
        {"dual_src_blend", features.dualSrcBlend != VK_FALSE},
        {"logic_op", features.logicOp != VK_FALSE},
        {"multi_draw_indirect", features.multiDrawIndirect != VK_FALSE},
        {"draw_indirect_first_instance", features.drawIndirectFirstInstance != VK_FALSE},
        {"depth_clamp", features.depthClamp != VK_FALSE},
        {"depth_bias_clamp", features.depthBiasClamp != VK_FALSE},
        {"fill_mode_non_solid", features.fillModeNonSolid != VK_FALSE},
        {"depth_bounds", features.depthBounds != VK_FALSE},
        {"wide_lines", features.wideLines != VK_FALSE},
        {"large_points", features.largePoints != VK_FALSE},
        {"alpha_to_one", features.alphaToOne != VK_FALSE},
        {"multi_viewport", features.multiViewport != VK_FALSE},
        {"sampler_anisotropy", features.samplerAnisotropy != VK_FALSE},
        {"texture_compression_etc2", features.textureCompressionETC2 != VK_FALSE},
        {"texture_compression_astc_ldr", features.textureCompressionASTC_LDR != VK_FALSE},
        {"texture_compression_bc", features.textureCompressionBC != VK_FALSE},
        {"occlusion_query_precise", features.occlusionQueryPrecise != VK_FALSE},
        {"pipeline_statistics_query", features.pipelineStatisticsQuery != VK_FALSE},
        {"vertex_pipeline_stores_and_atomics", features.vertexPipelineStoresAndAtomics != VK_FALSE},
        {"fragment_stores_and_atomics", features.fragmentStoresAndAtomics != VK_FALSE},
        {"shader_tessellation_and_geometry_point_size", features.shaderTessellationAndGeometryPointSize != VK_FALSE},
        {"shader_image_gather_extended", features.shaderImageGatherExtended != VK_FALSE},
        {"shader_storage_image_extended_formats", features.shaderStorageImageExtendedFormats != VK_FALSE},
        {"shader_storage_image_multisample", features.shaderStorageImageMultisample != VK_FALSE},
        {"shader_storage_image_read_without_format", features.shaderStorageImageReadWithoutFormat != VK_FALSE},
        {"shader_storage_image_write_without_format", features.shaderStorageImageWriteWithoutFormat != VK_FALSE},
        {"shader_uniform_buffer_array_dynamic_indexing", features.shaderUniformBufferArrayDynamicIndexing != VK_FALSE},
        {"shader_sampled_image_array_dynamic_indexing", features.shaderSampledImageArrayDynamicIndexing != VK_FALSE},
        {"shader_storage_buffer_array_dynamic_indexing", features.shaderStorageBufferArrayDynamicIndexing != VK_FALSE},
        {"shader_storage_image_array_dynamic_indexing", features.shaderStorageImageArrayDynamicIndexing != VK_FALSE},
        {"shader_clip_distance", features.shaderClipDistance != VK_FALSE},
        {"shader_cull_distance", features.shaderCullDistance != VK_FALSE},
        {"shader_float64", features.shaderFloat64 != VK_FALSE},
        {"shader_int64", features.shaderInt64 != VK_FALSE},
        {"shader_int16", features.shaderInt16 != VK_FALSE},
        {"shader_resource_residency", features.shaderResourceResidency != VK_FALSE},
        {"shader_resource_min_lod", features.shaderResourceMinLod != VK_FALSE},
        {"sparse_binding", features.sparseBinding != VK_FALSE},
        {"sparse_residency_buffer", features.sparseResidencyBuffer != VK_FALSE},
        {"sparse_residency_image_2d", features.sparseResidencyImage2D != VK_FALSE},
        {"sparse_residency_image_3d", features.sparseResidencyImage3D != VK_FALSE},
        {"sparse_residency_2_samples", features.sparseResidency2Samples != VK_FALSE},
        {"sparse_residency_4_samples", features.sparseResidency4Samples != VK_FALSE},
        {"sparse_residency_8_samples", features.sparseResidency8Samples != VK_FALSE},
        {"sparse_residency_16_samples", features.sparseResidency16Samples != VK_FALSE},
        {"sparse_residency_aliased", features.sparseResidencyAliased != VK_FALSE},
        {"variable_multisample_rate", features.variableMultisampleRate != VK_FALSE},
        {"inherited_queries", features.inheritedQueries != VK_FALSE}};
}

bool validFeatures(const VkPhysicalDeviceFeatures& features) {
    const VkBool32 values[] = {
        features.robustBufferAccess,
        features.fullDrawIndexUint32,
        features.imageCubeArray,
        features.independentBlend,
        features.geometryShader,
        features.tessellationShader,
        features.sampleRateShading,
        features.dualSrcBlend,
        features.logicOp,
        features.multiDrawIndirect,
        features.drawIndirectFirstInstance,
        features.depthClamp,
        features.depthBiasClamp,
        features.fillModeNonSolid,
        features.depthBounds,
        features.wideLines,
        features.largePoints,
        features.alphaToOne,
        features.multiViewport,
        features.samplerAnisotropy,
        features.textureCompressionETC2,
        features.textureCompressionASTC_LDR,
        features.textureCompressionBC,
        features.occlusionQueryPrecise,
        features.pipelineStatisticsQuery,
        features.vertexPipelineStoresAndAtomics,
        features.fragmentStoresAndAtomics,
        features.shaderTessellationAndGeometryPointSize,
        features.shaderImageGatherExtended,
        features.shaderStorageImageExtendedFormats,
        features.shaderStorageImageMultisample,
        features.shaderStorageImageReadWithoutFormat,
        features.shaderStorageImageWriteWithoutFormat,
        features.shaderUniformBufferArrayDynamicIndexing,
        features.shaderSampledImageArrayDynamicIndexing,
        features.shaderStorageBufferArrayDynamicIndexing,
        features.shaderStorageImageArrayDynamicIndexing,
        features.shaderClipDistance,
        features.shaderCullDistance,
        features.shaderFloat64,
        features.shaderInt64,
        features.shaderInt16,
        features.shaderResourceResidency,
        features.shaderResourceMinLod,
        features.sparseBinding,
        features.sparseResidencyBuffer,
        features.sparseResidencyImage2D,
        features.sparseResidencyImage3D,
        features.sparseResidency2Samples,
        features.sparseResidency4Samples,
        features.sparseResidency8Samples,
        features.sparseResidency16Samples,
        features.sparseResidencyAliased,
        features.variableMultisampleRate,
        features.inheritedQueries};
    return std::all_of(std::begin(values), std::end(values), [](VkBool32 value) {
        return value == VK_FALSE || value == VK_TRUE;
    });
}

bool enabledFeaturesAreAvailable(const json& enabled, const json& available) {
    for(auto feature = enabled.begin(); feature != enabled.end(); ++feature) {
        if(feature.value().template get<bool>() &&
           !available.at(feature.key()).template get<bool>()) {
            return false;
        }
    }
    return true;
}

bool enabledExtensionsAreAvailable(const std::vector<std::string>& enabled,
                                   const std::vector<ExtensionRecord>& available) {
    std::vector<std::string> availableNames;
    availableNames.reserve(available.size());
    for(const auto& extension : available) {
        availableNames.push_back(extension.name);
    }
    return std::includes(availableNames.begin(), availableNames.end(),
                         enabled.begin(), enabled.end());
}

bool negotiatedVersionIsSupported(std::uint32_t negotiated, std::uint32_t supported) {
    if(VK_API_VERSION_VARIANT(negotiated) != VK_API_VERSION_VARIANT(supported)) {
        return false;
    }
    constexpr std::uint32_t VERSION_WITHOUT_VARIANT_MASK = (1u << 29u) - 1u;
    return (negotiated & VERSION_WITHOUT_VARIANT_MASK) <=
           (supported & VERSION_WITHOUT_VARIANT_MASK);
}

bool standardApiVersionAtLeast(std::uint32_t version, std::uint32_t major,
                               std::uint32_t minor) {
    return VK_API_VERSION_VARIANT(version) == 0 &&
        (VK_API_VERSION_MAJOR(version) > major ||
         (VK_API_VERSION_MAJOR(version) == major &&
          VK_API_VERSION_MINOR(version) >= minor));
}

json encodeQueueFlags(VkQueueFlags flags) {
    return {
        {"raw", flags},
        {"graphics", (flags & VK_QUEUE_GRAPHICS_BIT) != 0},
        {"compute", (flags & VK_QUEUE_COMPUTE_BIT) != 0},
        {"transfer", (flags & VK_QUEUE_TRANSFER_BIT) != 0},
        {"sparse_binding", (flags & VK_QUEUE_SPARSE_BINDING_BIT) != 0},
        {"protected", (flags & VK_QUEUE_PROTECTED_BIT) != 0}};
}

json encodeMemoryHeapFlags(VkMemoryHeapFlags flags) {
    return {
        {"raw", flags},
        {"device_local", (flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0},
        {"multi_instance", (flags & VK_MEMORY_HEAP_MULTI_INSTANCE_BIT) != 0}};
}

json encodeMemoryPropertyFlags(VkMemoryPropertyFlags flags) {
    return {
        {"raw", flags},
        {"device_local", (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0},
        {"host_visible", (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0},
        {"host_coherent", (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0},
        {"host_cached", (flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0},
        {"lazily_allocated", (flags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) != 0},
        {"protected", (flags & VK_MEMORY_PROPERTY_PROTECTED_BIT) != 0}};
}

json encodeFormatFeatures(VkFormatFeatureFlags flags) {
    return {
        {"raw", flags},
        {"sampled_image", (flags & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0},
        {"storage_image", (flags & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0},
        {"storage_image_atomic", (flags & VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT) != 0},
        {"uniform_texel_buffer", (flags & VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT) != 0},
        {"storage_texel_buffer", (flags & VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT) != 0},
        {"storage_texel_buffer_atomic", (flags & VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_ATOMIC_BIT) != 0},
        {"vertex_buffer", (flags & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) != 0},
        {"color_attachment", (flags & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0},
        {"color_attachment_blend", (flags & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) != 0},
        {"depth_stencil_attachment", (flags & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0},
        {"blit_src", (flags & VK_FORMAT_FEATURE_BLIT_SRC_BIT) != 0},
        {"blit_dst", (flags & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0},
        {"sampled_image_filter_linear", (flags & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0},
        {"transfer_src", (flags & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) != 0},
        {"transfer_dst", (flags & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) != 0}};
}

struct ReportFormat {
    const char* name;
    VkFormat format;
};

constexpr ReportFormat REPORT_FORMATS[] = {
    {"VK_FORMAT_R8_UNORM", VK_FORMAT_R8_UNORM},
    {"VK_FORMAT_R8G8_UNORM", VK_FORMAT_R8G8_UNORM},
    {"VK_FORMAT_R8G8B8A8_UNORM", VK_FORMAT_R8G8B8A8_UNORM},
    {"VK_FORMAT_R8G8B8A8_SRGB", VK_FORMAT_R8G8B8A8_SRGB},
    {"VK_FORMAT_B8G8R8A8_UNORM", VK_FORMAT_B8G8R8A8_UNORM},
    {"VK_FORMAT_B8G8R8A8_SRGB", VK_FORMAT_B8G8R8A8_SRGB},
    {"VK_FORMAT_A2B10G10R10_UNORM_PACK32", VK_FORMAT_A2B10G10R10_UNORM_PACK32},
    {"VK_FORMAT_R16_SFLOAT", VK_FORMAT_R16_SFLOAT},
    {"VK_FORMAT_R16G16_SFLOAT", VK_FORMAT_R16G16_SFLOAT},
    {"VK_FORMAT_R16G16B16A16_SFLOAT", VK_FORMAT_R16G16B16A16_SFLOAT},
    {"VK_FORMAT_R32_SFLOAT", VK_FORMAT_R32_SFLOAT},
    {"VK_FORMAT_R32G32_SFLOAT", VK_FORMAT_R32G32_SFLOAT},
    {"VK_FORMAT_R32G32B32A32_SFLOAT", VK_FORMAT_R32G32B32A32_SFLOAT},
    {"VK_FORMAT_D16_UNORM", VK_FORMAT_D16_UNORM},
    {"VK_FORMAT_D24_UNORM_S8_UINT", VK_FORMAT_D24_UNORM_S8_UINT},
    {"VK_FORMAT_D32_SFLOAT", VK_FORMAT_D32_SFLOAT},
    {"VK_FORMAT_D32_SFLOAT_S8_UINT", VK_FORMAT_D32_SFLOAT_S8_UINT},
    {"VK_FORMAT_BC1_RGB_UNORM_BLOCK", VK_FORMAT_BC1_RGB_UNORM_BLOCK},
    {"VK_FORMAT_BC1_RGBA_UNORM_BLOCK", VK_FORMAT_BC1_RGBA_UNORM_BLOCK},
    {"VK_FORMAT_BC3_UNORM_BLOCK", VK_FORMAT_BC3_UNORM_BLOCK},
    {"VK_FORMAT_BC7_UNORM_BLOCK", VK_FORMAT_BC7_UNORM_BLOCK},
    {"VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK", VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK},
    {"VK_FORMAT_ASTC_4x4_UNORM_BLOCK", VK_FORMAT_ASTC_4x4_UNORM_BLOCK}};

json encodeSampleCounts(VkSampleCountFlags flags) {
    return {
        {"raw", flags},
        {"sample_1", (flags & VK_SAMPLE_COUNT_1_BIT) != 0},
        {"sample_2", (flags & VK_SAMPLE_COUNT_2_BIT) != 0},
        {"sample_4", (flags & VK_SAMPLE_COUNT_4_BIT) != 0},
        {"sample_8", (flags & VK_SAMPLE_COUNT_8_BIT) != 0},
        {"sample_16", (flags & VK_SAMPLE_COUNT_16_BIT) != 0},
        {"sample_32", (flags & VK_SAMPLE_COUNT_32_BIT) != 0},
        {"sample_64", (flags & VK_SAMPLE_COUNT_64_BIT) != 0}};
}

std::optional<json> encodeLimits(const VkPhysicalDeviceLimits& limits) {
    if(!std::isfinite(limits.maxSamplerLodBias) ||
       !std::isfinite(limits.maxSamplerAnisotropy) ||
       !std::isfinite(limits.viewportBoundsRange[0]) ||
       !std::isfinite(limits.viewportBoundsRange[1])) {
        return std::nullopt;
    }
    return json{
        {"max_image_dimension_1d", limits.maxImageDimension1D},
        {"max_image_dimension_2d", limits.maxImageDimension2D},
        {"max_image_dimension_3d", limits.maxImageDimension3D},
        {"max_image_dimension_cube", limits.maxImageDimensionCube},
        {"max_image_array_layers", limits.maxImageArrayLayers},
        {"max_texel_buffer_elements", limits.maxTexelBufferElements},
        {"max_uniform_buffer_range", limits.maxUniformBufferRange},
        {"max_storage_buffer_range", limits.maxStorageBufferRange},
        {"max_push_constants_size", limits.maxPushConstantsSize},
        {"max_memory_allocation_count", limits.maxMemoryAllocationCount},
        {"max_sampler_allocation_count", limits.maxSamplerAllocationCount},
        {"buffer_image_granularity", std::to_string(limits.bufferImageGranularity)},
        {"sparse_address_space_size", std::to_string(limits.sparseAddressSpaceSize)},
        {"max_bound_descriptor_sets", limits.maxBoundDescriptorSets},
        {"max_per_stage_descriptor_samplers", limits.maxPerStageDescriptorSamplers},
        {"max_per_stage_descriptor_uniform_buffers", limits.maxPerStageDescriptorUniformBuffers},
        {"max_per_stage_descriptor_storage_buffers", limits.maxPerStageDescriptorStorageBuffers},
        {"max_per_stage_descriptor_sampled_images", limits.maxPerStageDescriptorSampledImages},
        {"max_per_stage_descriptor_storage_images", limits.maxPerStageDescriptorStorageImages},
        {"max_per_stage_resources", limits.maxPerStageResources},
        {"max_descriptor_set_samplers", limits.maxDescriptorSetSamplers},
        {"max_descriptor_set_uniform_buffers", limits.maxDescriptorSetUniformBuffers},
        {"max_descriptor_set_uniform_buffers_dynamic", limits.maxDescriptorSetUniformBuffersDynamic},
        {"max_descriptor_set_storage_buffers", limits.maxDescriptorSetStorageBuffers},
        {"max_descriptor_set_storage_buffers_dynamic", limits.maxDescriptorSetStorageBuffersDynamic},
        {"max_descriptor_set_sampled_images", limits.maxDescriptorSetSampledImages},
        {"max_descriptor_set_storage_images", limits.maxDescriptorSetStorageImages},
        {"max_vertex_input_attributes", limits.maxVertexInputAttributes},
        {"max_vertex_input_bindings", limits.maxVertexInputBindings},
        {"max_vertex_input_attribute_offset", limits.maxVertexInputAttributeOffset},
        {"max_vertex_input_binding_stride", limits.maxVertexInputBindingStride},
        {"max_vertex_output_components", limits.maxVertexOutputComponents},
        {"max_fragment_input_components", limits.maxFragmentInputComponents},
        {"max_fragment_output_attachments", limits.maxFragmentOutputAttachments},
        {"max_fragment_dual_src_attachments", limits.maxFragmentDualSrcAttachments},
        {"max_fragment_combined_output_resources", limits.maxFragmentCombinedOutputResources},
        {"max_compute_shared_memory_size", limits.maxComputeSharedMemorySize},
        {"max_compute_work_group_count", {limits.maxComputeWorkGroupCount[0], limits.maxComputeWorkGroupCount[1], limits.maxComputeWorkGroupCount[2]}},
        {"max_compute_work_group_invocations", limits.maxComputeWorkGroupInvocations},
        {"max_compute_work_group_size", {limits.maxComputeWorkGroupSize[0], limits.maxComputeWorkGroupSize[1], limits.maxComputeWorkGroupSize[2]}},
        {"sub_pixel_precision_bits", limits.subPixelPrecisionBits},
        {"sub_texel_precision_bits", limits.subTexelPrecisionBits},
        {"mipmap_precision_bits", limits.mipmapPrecisionBits},
        {"max_draw_indexed_index_value", limits.maxDrawIndexedIndexValue},
        {"max_draw_indirect_count", limits.maxDrawIndirectCount},
        {"max_sampler_lod_bias", limits.maxSamplerLodBias},
        {"max_sampler_anisotropy", limits.maxSamplerAnisotropy},
        {"max_viewports", limits.maxViewports},
        {"max_viewport_dimensions", {limits.maxViewportDimensions[0], limits.maxViewportDimensions[1]}},
        {"viewport_bounds_range", {limits.viewportBoundsRange[0], limits.viewportBoundsRange[1]}},
        {"viewport_sub_pixel_bits", limits.viewportSubPixelBits},
        {"min_memory_map_alignment", std::to_string(static_cast<std::uint64_t>(limits.minMemoryMapAlignment))},
        {"min_texel_buffer_offset_alignment", std::to_string(limits.minTexelBufferOffsetAlignment)},
        {"min_uniform_buffer_offset_alignment", std::to_string(limits.minUniformBufferOffsetAlignment)},
        {"min_storage_buffer_offset_alignment", std::to_string(limits.minStorageBufferOffsetAlignment)},
        {"framebuffer_color_sample_counts", encodeSampleCounts(limits.framebufferColorSampleCounts)},
        {"framebuffer_depth_sample_counts", encodeSampleCounts(limits.framebufferDepthSampleCounts)},
        {"framebuffer_stencil_sample_counts", encodeSampleCounts(limits.framebufferStencilSampleCounts)},
        {"framebuffer_no_attachments_sample_counts", encodeSampleCounts(limits.framebufferNoAttachmentsSampleCounts)},
        {"sampled_image_color_sample_counts", encodeSampleCounts(limits.sampledImageColorSampleCounts)},
        {"sampled_image_integer_sample_counts", encodeSampleCounts(limits.sampledImageIntegerSampleCounts)},
        {"sampled_image_depth_sample_counts", encodeSampleCounts(limits.sampledImageDepthSampleCounts)},
        {"sampled_image_stencil_sample_counts", encodeSampleCounts(limits.sampledImageStencilSampleCounts)},
        {"storage_image_sample_counts", encodeSampleCounts(limits.storageImageSampleCounts)},
        {"max_color_attachments", limits.maxColorAttachments}};
}

template <typename Function>
Function resolveVulkanFunction(PFN_vkGetInstanceProcAddr getInstanceProcAddress,
                               VkInstance instance, const char* name) {
    return reinterpret_cast<Function>(getInstanceProcAddress(instance, name));
}

bool allDataGroupsComplete(const json& data) {
    constexpr const char* groups[] = {
        "versions", "physical_device", "instance_extensions", "device_extensions",
        "queues", "memory", "features", "formats", "limits"};
    return std::all_of(std::begin(groups), std::end(groups), [&](const char* group) {
        return !data[group].is_null();
    });
}

}  // namespace

VulkanCapabilityObservation collectAngleVulkanCapabilities(
    void* eglDevice, VulkanHostFunction queryDeviceAttributeFunction,
    VulkanHostFunction getEglErrorFunction) {
    VulkanCapabilityObservation observation;
    if(eglDevice == nullptr || queryDeviceAttributeFunction == nullptr) {
        addError(observation, "vulkan_bridge_query_entry_point_unavailable",
                 "The active ANGLE Vulkan device bridge could not be queried");
        return observation;
    }

    observation.queryEntryPointAvailable = true;
    auto queryDeviceAttribute =
        reinterpret_cast<EglQueryDeviceAttribute>(queryDeviceAttributeFunction);
    auto getEglError = reinterpret_cast<EglGetError>(getEglErrorFunction);

    auto negotiatedVersionAttribute = queryBridgeAttribute(
        observation, queryDeviceAttribute, getEglError, eglDevice,
        EGL_VULKAN_VERSION_ANGLE_VALUE, "vulkan_negotiated_version_query_failed",
        "ANGLE did not expose its negotiated Vulkan device API version");
    auto instanceAttribute = queryBridgeAttribute(
        observation, queryDeviceAttribute, getEglError, eglDevice,
        EGL_VULKAN_INSTANCE_ANGLE_VALUE, "vulkan_instance_bridge_query_failed",
        "ANGLE did not expose its active Vulkan instance");
    auto instanceExtensionsAttribute = queryBridgeAttribute(
        observation, queryDeviceAttribute, getEglError, eglDevice,
        EGL_VULKAN_INSTANCE_EXTENSIONS_ANGLE_VALUE,
        "vulkan_enabled_instance_extensions_query_failed",
        "ANGLE did not expose its enabled Vulkan instance extensions");
    auto physicalDeviceAttribute = queryBridgeAttribute(
        observation, queryDeviceAttribute, getEglError, eglDevice,
        EGL_VULKAN_PHYSICAL_DEVICE_ANGLE_VALUE,
        "vulkan_physical_device_bridge_query_failed",
        "ANGLE did not expose its active Vulkan physical device");
    auto deviceAttribute = queryBridgeAttribute(
        observation, queryDeviceAttribute, getEglError, eglDevice,
        EGL_VULKAN_DEVICE_ANGLE_VALUE, "vulkan_device_bridge_query_failed",
        "ANGLE did not expose its active Vulkan logical device");
    auto deviceExtensionsAttribute = queryBridgeAttribute(
        observation, queryDeviceAttribute, getEglError, eglDevice,
        EGL_VULKAN_DEVICE_EXTENSIONS_ANGLE_VALUE,
        "vulkan_enabled_device_extensions_query_failed",
        "ANGLE did not expose its enabled Vulkan device extensions");
    auto enabledFeaturesAttribute = queryBridgeAttribute(
        observation, queryDeviceAttribute, getEglError, eglDevice,
        EGL_VULKAN_FEATURES_ANGLE_VALUE, "vulkan_enabled_features_query_failed",
        "ANGLE did not expose its enabled core Vulkan features");
    auto activeQueueAttribute = queryBridgeAttribute(
        observation, queryDeviceAttribute, getEglError, eglDevice,
        EGL_VULKAN_QUEUE_ANGLE_VALUE, "vulkan_active_queue_bridge_query_failed",
        "ANGLE did not expose its active Vulkan queue");
    auto activeQueueFamilyAttribute = queryBridgeAttribute(
        observation, queryDeviceAttribute, getEglError, eglDevice,
        EGL_VULKAN_QUEUE_FAMILY_INDEX_ANGLE_VALUE,
        "vulkan_active_queue_family_query_failed",
        "ANGLE did not expose its active Vulkan queue family index");
    auto getInstanceProcAddressAttribute = queryBridgeAttribute(
        observation, queryDeviceAttribute, getEglError, eglDevice,
        EGL_VULKAN_GET_INSTANCE_PROC_ADDR_VALUE,
        "vulkan_proc_address_bridge_query_failed",
        "ANGLE did not expose its matching Vulkan instance procedure resolver");

    if(!observation.bridgeDataAvailable) {
        return observation;
    }
    observation.data = makeEmptyData();

    std::optional<std::vector<std::string>> enabledInstanceExtensions;
    if(instanceExtensionsAttribute) {
        enabledInstanceExtensions = readEnabledExtensions(*instanceExtensionsAttribute);
        if(!enabledInstanceExtensions) {
            addError(observation, "vulkan_enabled_instance_extensions_invalid",
                     "ANGLE's enabled Vulkan instance extensions contained invalid, duplicate, or out-of-bounds data");
        }
    }
    std::optional<std::vector<std::string>> enabledDeviceExtensions;
    if(deviceExtensionsAttribute) {
        enabledDeviceExtensions = readEnabledExtensions(*deviceExtensionsAttribute);
        if(!enabledDeviceExtensions) {
            addError(observation, "vulkan_enabled_device_extensions_invalid",
                     "ANGLE's enabled Vulkan device extensions contained invalid, duplicate, or out-of-bounds data");
        }
    }

    std::optional<std::uint32_t> negotiatedVersion;
    if(negotiatedVersionAttribute && *negotiatedVersionAttribute >= 0 &&
       static_cast<std::uint64_t>(*negotiatedVersionAttribute) <=
           std::numeric_limits<std::uint32_t>::max()) {
        negotiatedVersion = static_cast<std::uint32_t>(*negotiatedVersionAttribute);
    } else if(negotiatedVersionAttribute) {
        addError(observation, "vulkan_negotiated_version_invalid",
                 "ANGLE exposed an invalid negotiated Vulkan device API version");
    }

    std::optional<std::uint32_t> activeQueueFamily;
    if(activeQueueFamilyAttribute && *activeQueueFamilyAttribute >= 0 &&
       static_cast<std::uint64_t>(*activeQueueFamilyAttribute) <=
           std::numeric_limits<std::uint32_t>::max()) {
        activeQueueFamily = static_cast<std::uint32_t>(*activeQueueFamilyAttribute);
    } else if(activeQueueFamilyAttribute) {
        addError(observation, "vulkan_active_queue_family_invalid",
                 "ANGLE exposed an invalid active Vulkan queue family index");
    }

    if(deviceAttribute && *deviceAttribute == 0) {
        addError(observation, "vulkan_device_bridge_value_invalid",
                 "ANGLE exposed an invalid active Vulkan logical device");
    }
    if(activeQueueAttribute && *activeQueueAttribute == 0) {
        addError(observation, "vulkan_active_queue_bridge_value_invalid",
                 "ANGLE exposed an invalid active Vulkan queue");
    }

    VkInstance instance = VK_NULL_HANDLE;
    if(instanceAttribute && *instanceAttribute != 0) {
        instance = reinterpret_cast<VkInstance>(*instanceAttribute);
    } else if(instanceAttribute) {
        addError(observation, "vulkan_instance_bridge_value_invalid",
                 "ANGLE exposed an invalid active Vulkan instance");
    }
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    if(physicalDeviceAttribute && *physicalDeviceAttribute != 0) {
        physicalDevice = reinterpret_cast<VkPhysicalDevice>(*physicalDeviceAttribute);
    } else if(physicalDeviceAttribute) {
        addError(observation, "vulkan_physical_device_bridge_value_invalid",
                 "ANGLE exposed an invalid active Vulkan physical device");
    }
    PFN_vkGetInstanceProcAddr getInstanceProcAddress = nullptr;
    if(getInstanceProcAddressAttribute && *getInstanceProcAddressAttribute != 0) {
        getInstanceProcAddress =
            reinterpret_cast<PFN_vkGetInstanceProcAddr>(*getInstanceProcAddressAttribute);
    } else if(getInstanceProcAddressAttribute) {
        addError(observation, "vulkan_proc_address_bridge_value_invalid",
                 "ANGLE exposed an invalid Vulkan instance procedure resolver");
    }

    PFN_vkEnumerateInstanceVersion enumerateInstanceVersion = nullptr;
    PFN_vkEnumerateInstanceExtensionProperties enumerateInstanceExtensionProperties = nullptr;
    PFN_vkGetPhysicalDeviceProperties getPhysicalDeviceProperties = nullptr;
    PFN_vkGetPhysicalDeviceProperties2 getPhysicalDeviceProperties2Core = nullptr;
    PFN_vkGetPhysicalDeviceProperties2 getPhysicalDeviceProperties2Khr = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties enumerateDeviceExtensionProperties = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties getQueueFamilyProperties = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties getMemoryProperties = nullptr;
    PFN_vkGetPhysicalDeviceFeatures getPhysicalDeviceFeatures = nullptr;
    PFN_vkGetPhysicalDeviceFormatProperties getFormatProperties = nullptr;
    if(getInstanceProcAddress != nullptr) {
        enumerateInstanceVersion = resolveVulkanFunction<PFN_vkEnumerateInstanceVersion>(
            getInstanceProcAddress, VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
        enumerateInstanceExtensionProperties =
            resolveVulkanFunction<PFN_vkEnumerateInstanceExtensionProperties>(
                getInstanceProcAddress, VK_NULL_HANDLE,
                "vkEnumerateInstanceExtensionProperties");
        if(instance != VK_NULL_HANDLE) {
            getPhysicalDeviceProperties = resolveVulkanFunction<PFN_vkGetPhysicalDeviceProperties>(
                getInstanceProcAddress, instance, "vkGetPhysicalDeviceProperties");
            getPhysicalDeviceProperties2Core =
                resolveVulkanFunction<PFN_vkGetPhysicalDeviceProperties2>(
                    getInstanceProcAddress, instance,
                    "vkGetPhysicalDeviceProperties2");
            getPhysicalDeviceProperties2Khr =
                resolveVulkanFunction<PFN_vkGetPhysicalDeviceProperties2>(
                    getInstanceProcAddress, instance,
                    "vkGetPhysicalDeviceProperties2KHR");
            enumerateDeviceExtensionProperties =
                resolveVulkanFunction<PFN_vkEnumerateDeviceExtensionProperties>(
                    getInstanceProcAddress, instance,
                    "vkEnumerateDeviceExtensionProperties");
            getQueueFamilyProperties =
                resolveVulkanFunction<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
                    getInstanceProcAddress, instance,
                    "vkGetPhysicalDeviceQueueFamilyProperties");
            getMemoryProperties =
                resolveVulkanFunction<PFN_vkGetPhysicalDeviceMemoryProperties>(
                    getInstanceProcAddress, instance,
                    "vkGetPhysicalDeviceMemoryProperties");
            getPhysicalDeviceFeatures = resolveVulkanFunction<PFN_vkGetPhysicalDeviceFeatures>(
                getInstanceProcAddress, instance, "vkGetPhysicalDeviceFeatures");
            getFormatProperties =
                resolveVulkanFunction<PFN_vkGetPhysicalDeviceFormatProperties>(
                    getInstanceProcAddress, instance,
                    "vkGetPhysicalDeviceFormatProperties");
        }
    }

    std::optional<std::uint32_t> loaderVersion;
    if(getInstanceProcAddress != nullptr) {
        if(enumerateInstanceVersion == nullptr) {
            loaderVersion = VK_API_VERSION_1_0;
        } else {
            std::uint32_t version = 0;
            if(enumerateInstanceVersion(&version) == VK_SUCCESS) {
                loaderVersion = version;
            } else {
                addError(observation, "vulkan_loader_version_query_failed",
                         "The Vulkan loader API version could not be queried");
            }
        }
    }

    std::optional<VkPhysicalDeviceProperties> properties;
    if(physicalDevice != VK_NULL_HANDLE && getPhysicalDeviceProperties != nullptr) {
        VkPhysicalDeviceProperties value{};
        getPhysicalDeviceProperties(physicalDevice, &value);
        properties = value;
        auto name = boundedDeviceName(value.deviceName);
        if(name) {
            observation.data["physical_device"] = {
                {"name", *name},
                {"type", physicalDeviceType(value.deviceType)},
                {"vendor_id", value.vendorID},
                {"device_id", value.deviceID},
                {"driver_version_raw", value.driverVersion}};
        } else {
            addError(observation, "vulkan_physical_device_name_invalid",
                     "The Vulkan physical device name was outside the report bounds");
        }
        auto limits = encodeLimits(value.limits);
        if(limits) {
            observation.data["limits"] = std::move(*limits);
        } else {
            addError(observation, "vulkan_limit_data_invalid",
                     "The Vulkan physical device exposed invalid limit data");
        }
    } else {
        addError(observation, "vulkan_physical_device_query_entry_point_unavailable",
                 "A required Vulkan physical-device query entry point was unavailable");
    }

    if(loaderVersion && negotiatedVersion && properties) {
        if(negotiatedVersionIsSupported(*negotiatedVersion, *loaderVersion) &&
           negotiatedVersionIsSupported(*negotiatedVersion, properties->apiVersion)) {
            observation.data["versions"] = {
                {"loader_supported_instance_api", apiVersion(*loaderVersion)},
                {"angle_negotiated_device_api", apiVersion(*negotiatedVersion)},
                {"physical_device_supported_api", apiVersion(properties->apiVersion)}};
        } else {
            addError(observation, "vulkan_negotiated_version_unsupported",
                     "ANGLE's negotiated Vulkan API version was incompatible with reported loader or physical-device support");
        }
    }

    if(enumerateInstanceExtensionProperties != nullptr && enabledInstanceExtensions) {
        auto available = enumerateInstanceExtensions(enumerateInstanceExtensionProperties);
        if(available && enabledExtensionsAreAvailable(*enabledInstanceExtensions, *available)) {
            observation.data["instance_extensions"] = {
                {"enabled", *enabledInstanceExtensions},
                {"available", extensionRecordsToJson(*available)}};
        } else if(!available) {
            addError(observation, "vulkan_available_instance_extensions_query_failed",
                     "The available Vulkan instance extension set could not be collected within bounds");
        } else {
            addError(observation, "vulkan_enabled_instance_extension_unavailable",
                     "ANGLE enabled a Vulkan instance extension not present in the available set");
        }
    } else if(enumerateInstanceExtensionProperties == nullptr) {
        addError(observation, "vulkan_instance_extension_query_entry_point_unavailable",
                 "The Vulkan instance extension query entry point was unavailable");
    }

    std::optional<std::vector<ExtensionRecord>> availableDeviceExtensions;
    if(physicalDevice != VK_NULL_HANDLE && enumerateDeviceExtensionProperties != nullptr) {
        availableDeviceExtensions = enumerateDeviceExtensions(
            enumerateDeviceExtensionProperties, physicalDevice);
        if(!availableDeviceExtensions) {
            addError(observation, "vulkan_available_device_extensions_query_failed",
                     "The available Vulkan device extension set could not be collected within bounds");
        } else if(enabledDeviceExtensions &&
                  enabledExtensionsAreAvailable(*enabledDeviceExtensions,
                                                *availableDeviceExtensions)) {
            observation.data["device_extensions"] = {
                {"enabled", *enabledDeviceExtensions},
                {"available", extensionRecordsToJson(*availableDeviceExtensions)}};
        } else if(enabledDeviceExtensions) {
            addError(observation, "vulkan_enabled_device_extension_unavailable",
                     "ANGLE enabled a Vulkan device extension not present in the available set");
        }
    } else if(enumerateDeviceExtensionProperties == nullptr) {
        addError(observation, "vulkan_device_extension_query_entry_point_unavailable",
                 "The Vulkan device extension query entry point was unavailable");
    }

    bool activeDriverIsMoltenVk = false;
    bool serializedMoltenVkEvidenceAvailable =
        !observation.data["physical_device"].is_null() &&
        !observation.data["versions"].is_null() &&
        !observation.data["instance_extensions"].is_null() &&
        !observation.data["device_extensions"].is_null();
    bool coreProperties2Supported = negotiatedVersion && properties &&
        standardApiVersionAtLeast(*negotiatedVersion, 1, 1) &&
        standardApiVersionAtLeast(properties->apiVersion, 1, 1);
    bool khrProperties2Enabled = enabledInstanceExtensions &&
        std::binary_search(enabledInstanceExtensions->begin(),
                           enabledInstanceExtensions->end(),
                           "VK_KHR_get_physical_device_properties2");
    PFN_vkGetPhysicalDeviceProperties2 getPhysicalDeviceProperties2 = nullptr;
    if(coreProperties2Supported && getPhysicalDeviceProperties2Core != nullptr) {
        getPhysicalDeviceProperties2 = getPhysicalDeviceProperties2Core;
    } else if(khrProperties2Enabled && getPhysicalDeviceProperties2Khr != nullptr) {
        getPhysicalDeviceProperties2 = getPhysicalDeviceProperties2Khr;
    }
    bool coreDriverPropertiesSupported = negotiatedVersion && properties &&
        standardApiVersionAtLeast(*negotiatedVersion, 1, 2) &&
        standardApiVersionAtLeast(properties->apiVersion, 1, 2);
    bool extensionDriverPropertiesSupported =
        getPhysicalDeviceProperties2 != nullptr && availableDeviceExtensions &&
        extensionSetContains(*availableDeviceExtensions,
                             "VK_KHR_driver_properties");
    bool driverPropertiesSupported = coreDriverPropertiesSupported ||
        extensionDriverPropertiesSupported;
    if(serializedMoltenVkEvidenceAvailable &&
       physicalDevice != VK_NULL_HANDLE && properties &&
       getPhysicalDeviceProperties2 != nullptr && driverPropertiesSupported) {
        VkPhysicalDeviceDriverProperties driverProperties{};
        driverProperties.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
        VkPhysicalDeviceProperties2 properties2{};
        properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        properties2.pNext = &driverProperties;
        getPhysicalDeviceProperties2(physicalDevice, &properties2);
        bool corePropertiesMatch =
            properties2.properties.apiVersion == properties->apiVersion &&
            properties2.properties.driverVersion == properties->driverVersion &&
            properties2.properties.vendorID == properties->vendorID &&
            properties2.properties.deviceID == properties->deviceID &&
            properties2.properties.deviceType == properties->deviceType &&
            std::strncmp(properties2.properties.deviceName, properties->deviceName,
                         VK_MAX_PHYSICAL_DEVICE_NAME_SIZE) == 0;
        activeDriverIsMoltenVk = corePropertiesMatch &&
            driverProperties.driverID == VK_DRIVER_ID_MOLTENVK;
    }

    if(physicalDevice != VK_NULL_HANDLE && getQueueFamilyProperties != nullptr &&
       activeQueueFamily) {
        std::uint32_t count = 0;
        getQueueFamilyProperties(physicalDevice, &count, nullptr);
        if(count <= MAX_QUEUE_FAMILY_COUNT) {
            std::vector<VkQueueFamilyProperties> families(count);
            std::uint32_t returnedCount = count;
            if(count != 0) {
                getQueueFamilyProperties(physicalDevice, &returnedCount, families.data());
            }
            std::uint32_t stableCount = 0;
            getQueueFamilyProperties(physicalDevice, &stableCount, nullptr);
            if(returnedCount == count && stableCount == count &&
               *activeQueueFamily < count &&
               families[*activeQueueFamily].queueCount > 0) {
                json encodedFamilies = json::array();
                for(std::size_t index = 0; index < families.size(); ++index) {
                    const auto& family = families[index];
                    encodedFamilies.push_back({{"index", index},
                                               {"queue_count", family.queueCount},
                                               {"flags", encodeQueueFlags(family.queueFlags)},
                                               {"timestamp_valid_bits", family.timestampValidBits},
                                               {"min_image_transfer_granularity", {{"width", family.minImageTransferGranularity.width}, {"height", family.minImageTransferGranularity.height}, {"depth", family.minImageTransferGranularity.depth}}}});
                }
                observation.data["queues"] = {
                    {"active_family_index", *activeQueueFamily},
                    {"families", std::move(encodedFamilies)}};
            } else {
                addError(observation, "vulkan_queue_family_data_invalid",
                         "The Vulkan queue family set changed or did not contain ANGLE's active family");
            }
        } else {
            addError(observation, "vulkan_queue_family_count_exceeds_limit",
                     "The Vulkan queue family set exceeded the report size limit");
        }
    } else if(getQueueFamilyProperties == nullptr) {
        addError(observation, "vulkan_queue_family_query_entry_point_unavailable",
                 "The Vulkan queue family query entry point was unavailable");
    }

    if(physicalDevice != VK_NULL_HANDLE && getMemoryProperties != nullptr) {
        VkPhysicalDeviceMemoryProperties memory{};
        getMemoryProperties(physicalDevice, &memory);
        if(memory.memoryHeapCount <= VK_MAX_MEMORY_HEAPS &&
           memory.memoryTypeCount <= VK_MAX_MEMORY_TYPES) {
            json heaps = json::array();
            for(std::uint32_t index = 0; index < memory.memoryHeapCount; ++index) {
                const auto& heap = memory.memoryHeaps[index];
                heaps.push_back({{"index", index},
                                 {"size_bytes", std::to_string(heap.size)},
                                 {"flags", encodeMemoryHeapFlags(heap.flags)}});
            }
            json types = json::array();
            bool valid = true;
            for(std::uint32_t index = 0; index < memory.memoryTypeCount; ++index) {
                const auto& type = memory.memoryTypes[index];
                if(type.heapIndex >= memory.memoryHeapCount) {
                    valid = false;
                    break;
                }
                types.push_back({{"index", index},
                                 {"heap_index", type.heapIndex},
                                 {"flags", encodeMemoryPropertyFlags(type.propertyFlags)}});
            }
            if(valid) {
                observation.data["memory"] = {
                    {"heaps", std::move(heaps)},
                    {"types", std::move(types)}};
            } else {
                addError(observation, "vulkan_memory_data_invalid",
                         "A Vulkan memory type referenced an invalid heap");
            }
        } else {
            addError(observation, "vulkan_memory_count_invalid",
                     "The Vulkan memory topology exceeded the native core limits");
        }
    } else if(getMemoryProperties == nullptr) {
        addError(observation, "vulkan_memory_query_entry_point_unavailable",
                 "The Vulkan memory query entry point was unavailable");
    }

    if(physicalDevice != VK_NULL_HANDLE && getPhysicalDeviceFeatures != nullptr &&
       enabledFeaturesAttribute && *enabledFeaturesAttribute != 0 &&
       static_cast<std::uintptr_t>(*enabledFeaturesAttribute) %
               alignof(VkPhysicalDeviceFeatures2) == 0) {
        VkPhysicalDeviceFeatures available{};
        getPhysicalDeviceFeatures(physicalDevice, &available);
        auto enabled = reinterpret_cast<const VkPhysicalDeviceFeatures2*>(
            *enabledFeaturesAttribute);
        if(enabled->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 &&
           validFeatures(available) && validFeatures(enabled->features)) {
            auto availableJson = encodeFeatures(available);
            auto enabledJson = encodeFeatures(enabled->features);
            if(enabledFeaturesAreAvailable(enabledJson, availableJson)) {
                observation.data["features"] = {
                    {"available", std::move(availableJson)},
                    {"enabled", std::move(enabledJson)}};
            } else {
                addError(observation, "vulkan_enabled_feature_unavailable",
                         "ANGLE enabled a core Vulkan feature not present in the available set");
            }
        } else {
            addError(observation, "vulkan_enabled_features_bridge_value_invalid",
                     "ANGLE exposed an invalid enabled Vulkan feature structure or boolean value");
        }
    } else if(enabledFeaturesAttribute && *enabledFeaturesAttribute != 0 &&
              static_cast<std::uintptr_t>(*enabledFeaturesAttribute) %
                      alignof(VkPhysicalDeviceFeatures2) != 0) {
        addError(observation, "vulkan_enabled_features_bridge_value_invalid",
                 "ANGLE exposed an invalid enabled Vulkan feature structure");
    } else if(getPhysicalDeviceFeatures == nullptr) {
        addError(observation, "vulkan_feature_query_entry_point_unavailable",
                 "The Vulkan core feature query entry point was unavailable");
    }

    if(physicalDevice != VK_NULL_HANDLE && getFormatProperties != nullptr) {
        json formats = json::array();
        for(const auto& reportFormat : REPORT_FORMATS) {
            VkFormatProperties format{};
            getFormatProperties(physicalDevice, reportFormat.format, &format);
            formats.push_back({{"name", reportFormat.name},
                               {"linear_tiling", encodeFormatFeatures(format.linearTilingFeatures)},
                               {"optimal_tiling", encodeFormatFeatures(format.optimalTilingFeatures)},
                               {"buffer", encodeFormatFeatures(format.bufferFeatures)}});
        }
        std::sort(formats.begin(), formats.end(), [](const auto& left, const auto& right) {
            return left["name"].template get<std::string>() <
                   right["name"].template get<std::string>();
        });
        observation.data["formats"] = std::move(formats);
    } else if(getFormatProperties == nullptr) {
        addError(observation, "vulkan_format_query_entry_point_unavailable",
                 "The Vulkan format-property query entry point was unavailable");
    }

    if(serializedMoltenVkEvidenceAvailable && instance != VK_NULL_HANDLE &&
       getInstanceProcAddress != nullptr && properties) {
        observation.moltenVk = collectMoltenVkCapabilities(
            reinterpret_cast<void*>(instance),
            reinterpret_cast<MoltenVkHostFunction>(getInstanceProcAddress),
            properties->driverVersion, activeDriverIsMoltenVk,
            enabledInstanceExtensions ? &*enabledInstanceExtensions : nullptr);
    }

    observation.complete = observation.errors.empty() &&
                           allDataGroupsComplete(observation.data);
    return observation;
}
