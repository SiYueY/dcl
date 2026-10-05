#include "error.hpp"

#include <pybind11/pybind11.h>
#include <array>

namespace py = pybind11;
namespace dclpy::detail {
namespace {
constexpr std::array<const char*, 18> kMiddlewareExceptions{
    "InvalidArgumentError", "InvalidStateError", "InvalidNameError", "TypeMismatchError",
    "AlreadyExistsError", "NotFoundError", "AlreadyRegisteredError", "NotRegisteredError",
    "BusyError", "DclpyTimeoutError", "UnsupportedError", "IncompatibleQosError",
    "ParentDestroyedError", "ResourceExhaustedError", "MiddlewareError", "ContextShutdownError",
    "InterruptedError", "ProtocolFaultError"};

void set_exception(const char* name, const char* message) {
    // No global PyObject: the module owns exception classes and interpreter
    // teardown cannot leave a static Python reference destructor behind.
    auto module = py::module_::import("dclpy._dclpy");
    PyErr_SetString(module.attr(name).ptr(), message);
}
}

void bind_errors(py::module_& module) {
    auto base = py::reinterpret_steal<py::object>(
        PyErr_NewException("dclpy.DclpyError", PyExc_RuntimeError, nullptr));
    if (!base) throw py::error_already_set();
    module.attr("DclpyError") = base;
    auto add = [&](const char* name) {
        auto qualified = std::string("dclpy.") + name;
        auto error = py::reinterpret_steal<py::object>(
            PyErr_NewException(qualified.c_str(), base.ptr(), nullptr));
        if (!error) throw py::error_already_set();
        module.attr(name) = error;
    };
    for (const auto* name : kMiddlewareExceptions) add(name);
    add("EntityClosedError");
    add("ExecutorStoppedError");
    py::register_local_exception_translator([](std::exception_ptr pointer) {
        try {
            if (pointer) std::rethrow_exception(pointer);
        } catch (const MiddlewareFailure& error) {
            const auto code = static_cast<std::size_t>(error.code);
            set_exception(code < kMiddlewareExceptions.size() ? kMiddlewareExceptions[code]
                                                              : "MiddlewareError", error.what());
        } catch (const EntityClosed& error) {
            set_exception("EntityClosedError", error.what());
        } catch (const ContextShutdown& error) {
            set_exception("ContextShutdownError", error.what());
        }
    });
}

}  // namespace dclpy::detail
