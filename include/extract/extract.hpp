#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Extract {

    /// bbport: the Lossless.dll to read the shaders from (no folder search).
    void setDllPath(const std::string& path);

    ///
    /// Extract all known shaders.
    ///
    /// @throws std::runtime_error if shader extraction fails.
    ///
    void extractShaders();

    ///
    /// Get a shader by name.
    ///
    /// @param name The name of the shader to get.
    /// @return The shader bytecode.
    ///
    /// @throws std::runtime_error if the shader is not found.
    ///
    std::vector<uint8_t> getShader(const std::string& name);

    ///
    /// bbport: the names of all shaders the framegen asks for.
    ///
    std::vector<std::string> getShaderNames();

    ///
    /// bbport: the Lossless.dll resource id of a shader (the id of its DXBC resource).
    ///
    /// @return The id, or 0 for an unknown name.
    ///
    uint32_t getShaderId(const std::string& name);

    ///
    /// bbport: the bytes of any RCDATA resource of the loaded Lossless.dll.
    ///
    /// @return The resource, or an empty vector when the dll has none with that id.
    ///
    /// @throws std::runtime_error if the shaders are not loaded.
    ///
    std::vector<uint8_t> getResource(uint32_t id);

}
