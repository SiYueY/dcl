#ifndef DCLPY_DETAIL_BINDING_REGISTRY_HPP_
#define DCLPY_DETAIL_BINDING_REGISTRY_HPP_
#include <pybind11/pybind11.h>
#include <unordered_map>
#include <vector>
#include "type_support.hpp"
namespace dclpy::detail {
class BindingRegistry {
public:
    std::shared_ptr<const MessageBindingHandle> message(pybind11::handle type);
    std::shared_ptr<const ServiceBindingHandle> service_handle(pybind11::handle type);
    std::shared_ptr<const ActionBindingHandle> action_handle(pybind11::handle type);
    const DclpyServiceBindingV1* service(pybind11::handle type);
    const DclpyActionBindingV1* action(pybind11::handle type);
    // Called on the owner with GIL, after the complete native cleanup barrier.
    void clear();

private:
    void retain_provider(pybind11::handle type);
    std::unordered_map<const DclpyMessageBindingV1*, std::shared_ptr<const MessageBindingHandle>> messages_;
    std::vector<pybind11::object> providers_;
};

}  // namespace dclpy::detail
#endif  // DCLPY_DETAIL_BINDING_REGISTRY_HPP_
