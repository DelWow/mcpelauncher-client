#pragma once

#include "moltenvk_capability_collector.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <vector>

struct VulkanCapabilityError {
    const char* code;
    const char* message;
};

struct VulkanCapabilityObservation {
    nlohmann::json data = nullptr;
    std::vector<VulkanCapabilityError> errors;
    MoltenVkCapabilityObservation moltenVk;
    bool queryEntryPointAvailable = false;
    bool bridgeDataAvailable = false;
    bool complete = false;
};

using VulkanHostFunction = void (*)();

// Observes ANGLE's active Vulkan device through EGL_ANGLE_device_vulkan. This
// function only performs Vulkan capability queries; it never creates, destroys,
// locks, submits to, or waits on Vulkan objects, and it never serializes handles.
VulkanCapabilityObservation collectAngleVulkanCapabilities(
    void* eglDevice, VulkanHostFunction queryDeviceAttribute,
    VulkanHostFunction getEglError);
