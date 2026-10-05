#include "module.hpp"

PYBIND11_MODULE(_dclpy, module) {
    using namespace dclpy::detail;
    bind_errors(module);
    bind_qos(module);
    bind_context(module);
    bind_parameters(module);
    bind_type_support(module);
    bind_build_info(module);
    bind_wait_set(module);
    bind_publisher(module);
    bind_subscription(module);
    bind_services(module);
    bind_native_io(module);
    bind_timer(module);
    bind_graph(module);
    bind_actions(module);
}
