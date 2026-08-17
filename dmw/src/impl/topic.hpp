#ifndef DMW_IMPL__FASTDDS__TOPIC_H_
#define DMW_IMPL__FASTDDS__TOPIC_H_

#include <string>
#include <utility>

#include <fastdds/dds/topic/Topic.hpp>

namespace dmw {
namespace impl {

class Context;

/// One endpoint reference to a DDS type registration.
class TypeRegistration {
public:
    TypeRegistration() noexcept = default;
    ~TypeRegistration() noexcept;
    TypeRegistration(const TypeRegistration&) = delete;
    TypeRegistration& operator=(const TypeRegistration&) = delete;
    TypeRegistration(TypeRegistration&& other) noexcept;
    TypeRegistration& operator=(TypeRegistration&& other) noexcept;

private:
    friend class Context;
    friend class Topic;
    TypeRegistration(Context* context, std::string type_name) noexcept
    : context_(context), type_name_(std::move(type_name)) {}
    void reset() noexcept;
    void disarm() noexcept;
    Context* context_{nullptr};
    std::string type_name_;
};

/// Move-only endpoint ownership of a DDS Topic and its type reference.
class Topic {
public:
    Topic() noexcept = default;
    ~Topic() noexcept;
    Topic(const Topic&) = delete;
    Topic& operator=(const Topic&) = delete;
    Topic(Topic&& other) noexcept;
    Topic& operator=(Topic&& other) noexcept;
    eprosima::fastdds::dds::Topic* get() const noexcept { return topic_; }

private:
    friend class Context;
    Topic(
        Context* context, eprosima::fastdds::dds::Topic* topic, std::string topic_name,
        TypeRegistration type_registration) noexcept
    : context_(context),
      topic_(topic),
      topic_name_(std::move(topic_name)),
      type_registration_(std::move(type_registration)) {}
    void reset() noexcept;
    Context* context_{nullptr};
    eprosima::fastdds::dds::Topic* topic_{nullptr};
    std::string topic_name_;
    TypeRegistration type_registration_;
};

}  // namespace impl
}  // namespace dmw

#endif  // DMW_IMPL__FASTDDS__TOPIC_H_
