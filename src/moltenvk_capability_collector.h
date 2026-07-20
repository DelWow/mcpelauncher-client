#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

struct MoltenVkCapabilityError {
    const char* code;
    const char* message;
};

struct MoltenVkCapabilityObservation {
    nlohmann::json data = nullptr;
    std::vector<MoltenVkCapabilityError> errors;
    bool implementationDetected = false;
    bool complete = false;
};

using MoltenVkHostFunction = void (*)();

// Observes the MoltenVK implementation already selected by ANGLE. The query is
// read-only, never dereferences or serializes path-bearing configuration
// pointers, and never creates or changes Vulkan or MoltenVK state.
MoltenVkCapabilityObservation collectMoltenVkCapabilities(
    void* instance, MoltenVkHostFunction getInstanceProcAddress,
    std::uint32_t physicalDeviceDriverVersion, bool activeDriverIsMoltenVk,
    const std::vector<std::string>* enabledInstanceExtensions);
