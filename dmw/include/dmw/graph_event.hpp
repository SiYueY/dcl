#ifndef DMW_GRAPH_EVENT_HPP_
#define DMW_GRAPH_EVENT_HPP_

#include <memory>
#include <utility>

#include "dmw/graph.hpp"
#include "dmw/result.hpp"
#include "dmw/visibility_control.hpp"

namespace dmw {

class Context;
class WaitSet;

/// Level-triggered graph change notification owned by one Context.
///
/// Readiness is `current revision > cursor`.  A WaitSet reporting readiness does
/// not advance the cursor; only a successful take() does.
class DMW_PUBLIC GraphEvent {
public:
    ~GraphEvent() noexcept;

    GraphEvent(const GraphEvent&) = delete;
    GraphEvent& operator=(const GraphEvent&) = delete;
    GraphEvent(GraphEvent&&) = delete;
    GraphEvent& operator=(GraphEvent&&) = delete;

    Result<bool> take(GraphChangeInfo& info);

private:
    friend class Context;
    friend class WaitSet;

    class Impl;

    explicit GraphEvent(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

}  // namespace dmw

#endif  // DMW_GRAPH_EVENT_HPP_
