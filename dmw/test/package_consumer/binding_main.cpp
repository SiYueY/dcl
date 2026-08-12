// SPDX-License-Identifier: Apache-2.0

#include <functional>

#include "dmw/fastdds/message_type.hpp"

namespace {

class ConsumerTopicDataType : public eprosima::fastdds::dds::TopicDataType {
public:
    ConsumerTopicDataType() { setName("dmw.package.ConsumerTopicDataType"); }

    bool serialize(void*, eprosima::fastrtps::rtps::SerializedPayload_t*) override { return true; }

    bool deserialize(eprosima::fastrtps::rtps::SerializedPayload_t*, void*) override {
        return true;
    }

    std::function<std::uint32_t()> getSerializedSizeProvider(void*) override {
        return [] { return 0U; };
    }

    void* createData() override { return nullptr; }
    void deleteData(void*) override {}
    bool getKey(void*, eprosima::fastrtps::rtps::InstanceHandle_t*, bool) override { return false; }
};

}  // namespace

int main() {
    const auto result = dmw::fastdds::make_message_type<ConsumerTopicDataType>();
    return result ? 0 : 1;
}
