#ifndef DCLPY_DETAIL_TYPE_SUPPORT_HPP_
#define DCLPY_DETAIL_TYPE_SUPPORT_HPP_

#include <utility>

namespace pybind11 { class handle; class object; class module_; }
#include <memory>

#include "dclpy/interface_binding.h"
#include "dmw/fastdds/message_type.hpp"
#include "error.hpp"

namespace dclpy::detail {

struct MessageBindingHandle {
    const DclpyMessageBindingV1* binding;
    dmw::MessageType type;
};

struct ServiceBindingHandle {
    const DclpyServiceBindingV1* binding;
    dmw::ServiceType type;
    std::shared_ptr<const MessageBindingHandle> request;
    std::shared_ptr<const MessageBindingHandle> response;
};

struct ActionBindingHandle {
    const DclpyActionBindingV1* binding;
    dmw::ActionType type;
    std::shared_ptr<const MessageBindingHandle> goal;
    std::shared_ptr<const MessageBindingHandle> result;
    std::shared_ptr<const MessageBindingHandle> feedback;
    std::shared_ptr<const ServiceBindingHandle> send_goal;
    std::shared_ptr<const ServiceBindingHandle> cancel_goal;
    std::shared_ptr<const ServiceBindingHandle> get_result;
    std::shared_ptr<const MessageBindingHandle> feedback_message;
    std::shared_ptr<const MessageBindingHandle> status_message;
};

// Contains no Python object. The Context's GIL-owned registry must outlive all
// samples, worker jobs, completion records and retained native payloads.
class OwnedSample {
public:
    OwnedSample(std::shared_ptr<const MessageBindingHandle> binding, void* sample)
    : binding_(std::move(binding)), sample_(sample) {
        if (!binding_ || !sample_) throw std::invalid_argument("Invalid OwnedSample");
    }
    OwnedSample(const OwnedSample&) = delete;
    OwnedSample& operator=(const OwnedSample&) = delete;
    OwnedSample(OwnedSample&& other) noexcept
    : binding_(std::move(other.binding_)), sample_(std::exchange(other.sample_, nullptr)) {}
    OwnedSample& operator=(OwnedSample&& other) noexcept {
        if (this != &other) {
            reset();
            binding_ = std::move(other.binding_);
            sample_ = std::exchange(other.sample_, nullptr);
        }
        return *this;
    }
    ~OwnedSample() noexcept { reset(); }
    void* get() const noexcept { return sample_; }
    const std::shared_ptr<const MessageBindingHandle>& binding() const noexcept { return binding_; }
    OwnedSample clone() const;
    // Both entry points require the GIL; clone() and destruction do not.
    static OwnedSample freeze(std::shared_ptr<const MessageBindingHandle>, pybind11::handle);
    pybind11::object materialize() const;

private:
    void reset() noexcept {
        if (sample_) binding_->binding->destroy_sample(std::exchange(sample_, nullptr));
    }
    std::shared_ptr<const MessageBindingHandle> binding_;
    void* sample_;
};

void bind_type_support(pybind11::module_&);

}  // namespace dclpy::detail

#endif  // DCLPY_DETAIL_TYPE_SUPPORT_HPP_
