#include <cassert>
#include <string>
#include <vector>

#include "dmw/arguments.hpp"

int main() {
    const std::vector<std::string> argv{
        "application", "--verbose", "--ros-args", "-r", "__node:=remapped", "-r",
        "/input:=/output", "-p", "gain:=0.5", "--"};
    const auto arguments = dmw::parse_arguments(argv);
    assert(arguments);
    assert(arguments.value().node_name_remap() == "remapped");
    assert(arguments.value().remaps().size() == 1);
    assert(arguments.value().remaps().front().from == "/input");
    assert(arguments.value().remaps().front().to == "/output");
    assert(arguments.value().parameter_overrides().size() == 1);
    assert(arguments.value().parameter_overrides().front().name == "gain");
    assert(arguments.value().parameter_overrides().front().value == "0.5");
    assert(arguments.value().unparsed_arguments().size() == 2);
    assert(arguments.value().unparsed_arguments().front() == "application");
    assert(arguments.value().unparsed_arguments().back() == "--verbose");

    const auto invalid = dmw::parse_arguments({"--ros-args", "-r", "invalid", "--"});
    assert(!invalid);
    assert(invalid.error().code() == dmw::ErrorCode::InvalidArgument);
    return 0;
}
