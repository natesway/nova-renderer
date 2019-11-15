//! \brief A collection of constants for Nova to use

#pragma once

#include <string>

namespace nova::renderer {
    const std::string MODEL_MATRIX_BUFFER_NAME = "NovaModelMatrixUBO";
    const std::string PER_FRAME_DATA_NAME = "NovaPerFrameUBO";

    const uint32_t AMD_PCI_VENDOR_ID = 0x1022;
    const uint32_t INTEL_PCI_VENDOR_ID = 8086;
    const uint32_t NVIDIA_PCI_VENDOR_ID = 0x10DE;
}
