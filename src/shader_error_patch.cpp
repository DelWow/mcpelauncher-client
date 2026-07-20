#include "shader_error_patch.h"
#include <mcpelauncher/linker.h>
#include <game_window_manager.h>
#include <log.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace {

constexpr unsigned int GL_COMPILE_STATUS_VALUE = 0x8B81;
constexpr unsigned int GL_LINK_STATUS_VALUE = 0x8B82;
constexpr unsigned int GL_INFO_LOG_LENGTH_VALUE = 0x8B84;
constexpr unsigned int GL_SHADER_TYPE_VALUE = 0x8B4F;
constexpr int GL_TRUE_VALUE = 1;

constexpr std::size_t MAX_INFO_LOG_BYTES = 64 * 1024;
constexpr std::uint32_t MAX_FAILURE_EVENTS = 16;
constexpr std::size_t LOG_CHUNK_BYTES = 768;

std::atomic<std::uint32_t> failureEventCount{0};
std::mutex installationMutex;
#if defined(__APPLE__) && defined(__aarch64__) && !defined(NDEBUG)
bool fullInstallLogged = false;
#endif

struct CapturedInfoLog {
    int reportedBytes = 0;
    std::size_t capturedBytes = 0;
    bool truncated = false;
    std::string text;
};

std::optional<std::uint32_t> reserveFailureEvent() {
    std::uint32_t event = failureEventCount.load(std::memory_order_relaxed);
    while(event <= MAX_FAILURE_EVENTS) {
        if(failureEventCount.compare_exchange_weak(
               event, event + 1, std::memory_order_relaxed,
               std::memory_order_relaxed)) {
            if(event < MAX_FAILURE_EVENTS) {
                return event + 1;
            }
            Log::warn("Shader", "failure_event_limit=%u further_shader_diagnostics_suppressed=true",
                      MAX_FAILURE_EVENTS);
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::string escapeInfoLog(const char* data, std::size_t length) {
    static constexpr char HEX[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(std::min(length * 2, MAX_INFO_LOG_BYTES * 2));
    for(std::size_t index = 0; index < length; ++index) {
        auto value = static_cast<unsigned char>(data[index]);
        if(value == '\\') {
            result += "\\\\";
        } else if(value >= 0x20 && value <= 0x7E) {
            result.push_back(static_cast<char>(value));
        } else if(value == '\n') {
            result += "\\n";
        } else if(value == '\r') {
            result += "\\r";
        } else if(value == '\t') {
            result += "\\t";
        } else {
            result += "\\x";
            result.push_back(HEX[value >> 4]);
            result.push_back(HEX[value & 0x0F]);
        }
    }
    return result;
}

template <typename QueryLength, typename ReadLog>
CapturedInfoLog captureInfoLog(unsigned int object, QueryLength queryLength,
                               ReadLog readLog) {
    CapturedInfoLog capture;
    queryLength(object, GL_INFO_LOG_LENGTH_VALUE, &capture.reportedBytes);
    if(capture.reportedBytes <= 0) {
        return capture;
    }

    std::size_t bufferBytes = std::min<std::size_t>(
        static_cast<std::size_t>(capture.reportedBytes), MAX_INFO_LOG_BYTES);
    capture.truncated = static_cast<std::size_t>(capture.reportedBytes) > bufferBytes;
    std::vector<char> buffer(bufferBytes + 1, '\0');
    int returnedBytes = 0;
    readLog(object, static_cast<int>(bufferBytes), &returnedBytes, buffer.data());
    if(returnedBytes < 0) {
        returnedBytes = 0;
    }
    capture.truncated = capture.truncated ||
        static_cast<std::size_t>(returnedBytes) > bufferBytes;
    capture.capturedBytes = std::min<std::size_t>(
        static_cast<std::size_t>(returnedBytes), bufferBytes);
    if(capture.capturedBytes == 0) {
        capture.capturedBytes = std::char_traits<char>::length(buffer.data());
        capture.capturedBytes = std::min(capture.capturedBytes, bufferBytes);
    }
    capture.truncated = capture.truncated ||
        capture.capturedBytes >= bufferBytes;
    while(capture.capturedBytes != 0 &&
          buffer[capture.capturedBytes - 1] == '\0') {
        --capture.capturedBytes;
    }
    capture.text = escapeInfoLog(buffer.data(), capture.capturedBytes);
    return capture;
}

const char* shaderStageName(int type) {
    switch(type) {
    case 0x8B31: return "vertex";
    case 0x8B30: return "fragment";
    case 0x91B9: return "compute";
    case 0x8DD9: return "geometry";
    case 0x8E88: return "tessellation_control";
    case 0x8E87: return "tessellation_evaluation";
    default: return "unknown";
    }
}

void writeInfoLogChunks(std::uint32_t event, const std::string& text) {
    if(text.empty()) {
        Log::error("Shader", "event=%u info_log_chunk=empty", event);
        return;
    }
    std::size_t chunkCount = (text.size() + LOG_CHUNK_BYTES - 1) / LOG_CHUNK_BYTES;
    for(std::size_t index = 0; index < chunkCount; ++index) {
        auto offset = index * LOG_CHUNK_BYTES;
        auto chunk = text.substr(offset, LOG_CHUNK_BYTES);
        Log::error("Shader", "event=%u info_log_chunk=%zu/%zu text=%s", event,
                   index + 1, chunkCount, chunk.c_str());
    }
}

}  // namespace

void (*ShaderErrorPatch::glGetShaderiv)(unsigned int shader, unsigned int pname, int* params);
void (*ShaderErrorPatch::glGetShaderInfoLog)(unsigned int shader, int maxLength, int* length, char* log);
void (*ShaderErrorPatch::glCompileShader)(unsigned int shader);
void (*ShaderErrorPatch::glGetProgramiv)(unsigned int program, unsigned int pname, int* params);
void (*ShaderErrorPatch::glGetProgramInfoLog)(unsigned int program, int maxLength, int* length, char* log);
void (*ShaderErrorPatch::glLinkProgram)(unsigned int program);
bool ShaderErrorPatch::compileHookInstalled = false;
bool ShaderErrorPatch::linkHookInstalled = false;

void ShaderErrorPatch::install(void* handle) {
    static_cast<void>(handle);
    // TODO:
    //    hybris_hook("glCompileShader", (void*) glCompileShaderHook);
    //    hybris_hook("glLinkProgram", (void*) glLinkProgramHook);
}

bool ShaderErrorPatch::installGL(
    std::unordered_map<std::string, void*>& overrides,
    void* (*resolver)(const char*)) {
#if defined(__APPLE__) && defined(__aarch64__) && !defined(NDEBUG)
    if(resolver == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(installationMutex);

    if(compileHookInstalled) {
        overrides["glCompileShader"] =
            reinterpret_cast<void*>(&ShaderErrorPatch::glCompileShaderHook);
    } else {
        auto compile = reinterpret_cast<void (*)(unsigned int)>(
            resolver("glCompileShader"));
        auto getShader = reinterpret_cast<void (*)(unsigned int, unsigned int, int*)>(
            resolver("glGetShaderiv"));
        auto getShaderLog = reinterpret_cast<void (*)(unsigned int, int, int*, char*)>(
            resolver("glGetShaderInfoLog"));
        if(compile != nullptr && getShader != nullptr && getShaderLog != nullptr &&
           reinterpret_cast<void*>(compile) !=
               reinterpret_cast<void*>(&ShaderErrorPatch::glCompileShaderHook)) {
            glCompileShader = compile;
            glGetShaderiv = getShader;
            glGetShaderInfoLog = getShaderLog;
            overrides["glCompileShader"] =
                reinterpret_cast<void*>(&ShaderErrorPatch::glCompileShaderHook);
            compileHookInstalled = true;
        }
    }

    if(linkHookInstalled) {
        overrides["glLinkProgram"] =
            reinterpret_cast<void*>(&ShaderErrorPatch::glLinkProgramHook);
    } else {
        auto link = reinterpret_cast<void (*)(unsigned int)>(
            resolver("glLinkProgram"));
        auto getProgram = reinterpret_cast<void (*)(unsigned int, unsigned int, int*)>(
            resolver("glGetProgramiv"));
        auto getProgramLog = reinterpret_cast<void (*)(unsigned int, int, int*, char*)>(
            resolver("glGetProgramInfoLog"));
        if(link != nullptr && getProgram != nullptr && getProgramLog != nullptr &&
           reinterpret_cast<void*>(link) !=
               reinterpret_cast<void*>(&ShaderErrorPatch::glLinkProgramHook)) {
            glLinkProgram = link;
            glGetProgramiv = getProgram;
            glGetProgramInfoLog = getProgramLog;
            overrides["glLinkProgram"] =
                reinterpret_cast<void*>(&ShaderErrorPatch::glLinkProgramHook);
            linkHookInstalled = true;
        }
    }

    if(compileHookInstalled && linkHookInstalled) {
        if(!fullInstallLogged) {
            Log::info("Shader", "Installed bounded shader compile/link failure logging (max_info_log_bytes=%zu max_events=%u)",
                      MAX_INFO_LOG_BYTES, MAX_FAILURE_EVENTS);
            fullInstallLogged = true;
        }
        return true;
    }
    Log::warn("Shader", "Shader failure logging incomplete (compile_hook=%s link_hook=%s)",
              compileHookInstalled ? "installed" : "unavailable",
              linkHookInstalled ? "installed" : "unavailable");
    return false;
#else
    static_cast<void>(overrides);
    static_cast<void>(resolver);
    return false;
#endif
}

void ShaderErrorPatch::onGLContextCreated() {
    if(compileHookInstalled || linkHookInstalled) {
        return;
    }
    auto getProcAddr = GameWindowManager::getManager()->getProcAddrFunc();
    glCompileShader = (void (*)(unsigned int))getProcAddr("glCompileShader");
    glLinkProgram = (void (*)(unsigned int))getProcAddr("glLinkProgram");
    glGetShaderiv = (void (*)(unsigned int, unsigned int, int*))getProcAddr("glGetShaderiv");
    glGetShaderInfoLog = (void (*)(unsigned int, int, int*, char*))getProcAddr("glGetShaderInfoLog");
    glGetProgramiv = (void (*)(unsigned int, unsigned int, int*))getProcAddr("glGetProgramiv");
    glGetProgramInfoLog = (void (*)(unsigned int, int, int*, char*))getProcAddr("glGetProgramInfoLog");
}

void ShaderErrorPatch::glCompileShaderHook(unsigned int shader) {
    glCompileShader(shader);
    try {
        int status = GL_TRUE_VALUE;
        glGetShaderiv(shader, GL_COMPILE_STATUS_VALUE, &status);
        if(status == GL_TRUE_VALUE) {
            return;
        }
        auto event = reserveFailureEvent();
        if(!event) {
            return;
        }
        int shaderType = 0;
        glGetShaderiv(shader, GL_SHADER_TYPE_VALUE, &shaderType);
        auto capture = captureInfoLog(shader, glGetShaderiv, glGetShaderInfoLog);
        Log::error("Shader", "event=%u operation=shader_compile object_id=%u stage=%s status=failed reported_bytes=%d captured_bytes=%zu truncated=%s",
                   *event, shader, shaderStageName(shaderType), capture.reportedBytes,
                   capture.capturedBytes, capture.truncated ? "true" : "false");
        writeInfoLogChunks(*event, capture.text);
    } catch(...) {
        Log::error("Shader", "operation=shader_compile object_id=%u diagnostic_collection_failed=true",
                   shader);
    }
}

void ShaderErrorPatch::glLinkProgramHook(unsigned int program) {
    glLinkProgram(program);
    try {
        int status = GL_TRUE_VALUE;
        glGetProgramiv(program, GL_LINK_STATUS_VALUE, &status);
        if(status == GL_TRUE_VALUE) {
            return;
        }
        auto event = reserveFailureEvent();
        if(!event) {
            return;
        }
        auto capture = captureInfoLog(program, glGetProgramiv, glGetProgramInfoLog);
        Log::error("Shader", "event=%u operation=program_link object_id=%u stage=not_applicable status=failed reported_bytes=%d captured_bytes=%zu truncated=%s",
                   *event, program, capture.reportedBytes, capture.capturedBytes,
                   capture.truncated ? "true" : "false");
        writeInfoLogChunks(*event, capture.text);
    } catch(...) {
        Log::error("Shader", "operation=program_link object_id=%u diagnostic_collection_failed=true",
                   program);
    }
}
