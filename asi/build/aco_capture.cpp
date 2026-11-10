// SPDX-License-Identifier: MIT
// Compile only: use the runtime's exact feature chain and both pipeline layouts.
#include "opennr_rr/vk.hpp"
#include <cstdio>
#include <stdexcept>

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("usage: aco_capture shader.spv (ONR_ACO_CAPTURE must name the output prefix)");
        onr::Device device;
        std::string error;
        if (!device.init(error)) throw std::runtime_error(error);
        std::printf("capture device: %s / %s\n", device.info().name.c_str(), device.info().driver_info.c_str());
        auto kernel = device.load(argv[1], 32);
        device.destroy(kernel);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ACO capture: %s\n", e.what());
        return 1;
    }
}
