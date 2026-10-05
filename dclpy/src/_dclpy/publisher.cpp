#include "module.hpp"
#include "topic.hpp"

namespace py = pybind11;
namespace dclpy::detail {
void bind_publisher(py::module_& module) {
    auto publisher = py::class_<NativePublisher, std::shared_ptr<NativePublisher>>(module, "_Publisher");
    publisher.def(py::init<std::shared_ptr<NativeNode>, std::shared_ptr<const MessageBindingHandle>,
                          const std::string&, const dmw::Qos&>(),
                  py::call_guard<py::gil_scoped_release>())
        .def_property_readonly("entity_id", &NativePublisher::entity_id)
        .def("quiescent", &NativePublisher::quiescent, py::call_guard<py::gil_scoped_release>())
        .def("write", &NativePublisher::write, py::call_guard<py::gil_scoped_release>())
        .def("retire_binding", &NativePublisher::retire_binding)
        .def("actual_qos", [](NativePublisher& self) {
            return unwrap(self.backing()->operation()->actual_qos());
        }, py::call_guard<py::gil_scoped_release>());
    bind_lifecycle(publisher);
}
}  // namespace dclpy::detail
