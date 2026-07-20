#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>

class ShaderErrorPatch {
private:
    static void (*glGetShaderiv)(unsigned int shader, unsigned int pname, int* params);
    static void (*glGetShaderInfoLog)(unsigned int shader, int maxLength, int* length, char* log);

    static void (*glCompileShader)(unsigned int shader);
    static void glCompileShaderHook(unsigned int shader);

    static void (*glGetProgramiv)(unsigned int program, unsigned int pname, int* params);
    static void (*glGetProgramInfoLog)(unsigned int program, int maxLength, int* length, char* log);

    static void (*glLinkProgram)(unsigned int program);
    static void glLinkProgramHook(unsigned int program);

    static bool compileHookInstalled;
    static bool linkHookInstalled;

public:
    static void install(void* handle);

    // Installs process-lifetime wrappers into FakeEGL's guest GL resolver. The
    // resolver must return the currently effective function. Install this
    // after stable earlier wrappers such as GLCorePatch so repeated setup
    // keeps the forwarding chain acyclic.
    static bool installGL(std::unordered_map<std::string, void*>& overrides,
                          void* (*resolver)(const char*));

    static void onGLContextCreated();
};
