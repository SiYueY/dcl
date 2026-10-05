#include "module.hpp"
#include "wait_set.hpp"
#include <pybind11/stl.h>
namespace py = pybind11;
namespace dclpy::detail {
void bind_wait_set(py::module_& module) {
    py::class_<NativeWaitable, std::shared_ptr<NativeWaitable>>(module, "_Waitable")
        .def_property_readonly("entity_id", &NativeWaitable::entity_id)
        .def("quiescent", &NativeWaitable::quiescent, py::call_guard<py::gil_scoped_release>());
    py::class_<WorkPin, std::shared_ptr<WorkPin>>(module, "_WorkPin")
        .def("release", &WorkPin::release, py::call_guard<py::gil_scoped_release>());
    py::class_<DispatchPin, std::shared_ptr<DispatchPin>>(module, "_DispatchPin")
        .def("release", &DispatchPin::release, py::call_guard<py::gil_scoped_release>());
    py::class_<NativeReady>(module, "_Ready")
        .def_readonly("registration", &NativeReady::registration)
        .def_readonly("attachment_generation", &NativeReady::attachment_generation)
        .def_readonly("detail_mask", &NativeReady::detail_mask)
        .def_readonly("pin", &NativeReady::pin);
    py::class_<ReadyBatch>(module, "_ReadyBatch")
        .def_property_readonly("entries", &ReadyBatch::entries)
        .def("release", &ReadyBatch::release, py::call_guard<py::gil_scoped_release>());
    py::class_<NativeWaitSet, std::shared_ptr<NativeWaitSet>>(module, "_WaitSet")
        .def(py::init<std::shared_ptr<NativeContext>>(), py::call_guard<py::gil_scoped_release>())
        .def("add", &NativeWaitSet::add, py::call_guard<py::gil_scoped_release>())
        .def("remove", &NativeWaitSet::remove, py::call_guard<py::gil_scoped_release>())
        .def("set_interest", &NativeWaitSet::set_interest, py::call_guard<py::gil_scoped_release>())
        .def("wake", &NativeWaitSet::wake, py::call_guard<py::gil_scoped_release>())
        .def("close", &NativeWaitSet::close, py::call_guard<py::gil_scoped_release>())
        .def("wait", [](NativeWaitSet& self, std::optional<double> timeout) {
            const auto deadline = deadline_from_timeout(timeout);
            dmw::WaitTimeout native_timeout = dmw::WaitTimeout::infinite();
            if (deadline) {
                const auto remaining = *deadline - std::chrono::steady_clock::now();
                native_timeout = remaining <= std::chrono::steady_clock::duration::zero()
                    ? dmw::WaitTimeout::poll()
                    : unwrap(dmw::WaitTimeout::finite(std::chrono::duration_cast<std::chrono::nanoseconds>(remaining)));
            }
            py::gil_scoped_release release;
            return self.wait(native_timeout);
        }, py::arg("timeout") = py::none());
}

}  // namespace dclpy::detail
