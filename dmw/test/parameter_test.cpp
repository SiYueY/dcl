#include <cassert>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "dmw/arguments.hpp"
#include "dmw/context.hpp"
#include "dmw/node.hpp"
#include "dmw/parameter.hpp"
#include "dmw/parameter_change_set.hpp"
#include "dmw/parameter_descriptor.hpp"
#include "impl/parameter_store.hpp"

namespace {

using dmw::ErrorCode;
using dmw::ParameterDescriptor;
using dmw::ParameterValue;

ParameterValue integer(std::int64_t value) { return ParameterValue::make_integer(value); }
ParameterValue real(double value) { return ParameterValue::make_double(value); }

void test_value_types() {
    assert(ParameterValue{}.type() == dmw::ParameterType::NotSet);
    assert(!ParameterValue{}.is_set());

    const auto boolean = ParameterValue::make_bool(true);
    assert(boolean.type() == dmw::ParameterType::Bool);
    assert(boolean.as_bool());
    assert(boolean.is_set());

    const auto number = ParameterValue::make_integer(-7);
    assert(number.type() == dmw::ParameterType::Integer);
    assert(number.as_integer() == -7);

    const auto ratio = ParameterValue::make_double(1.5);
    assert(ratio.as_double() == 1.5);

    const auto text = ParameterValue::make_string("alpha");
    assert(text.as_string() == "alpha");

    const ParameterValue bytes = ParameterValue::make_byte_array({1, 2, 3});
    assert(bytes.as_byte_array().size() == 3);
    const ParameterValue bools = ParameterValue::make_bool_array({true, false});
    assert(bools.as_bool_array().size() == 2);
    const ParameterValue integers = ParameterValue::make_integer_array({1, 2});
    assert(integers.as_integer_array().front() == 1);
    const ParameterValue doubles = ParameterValue::make_double_array({1.0, 2.0});
    assert(doubles.as_double_array().size() == 2);
    const ParameterValue strings = ParameterValue::make_string_array({"a", "b"});
    assert(strings.as_string_array().back() == "b");

    assert(integer(3) == integer(3));
    assert(integer(3) != real(3.0));
}

void test_name_validation() {
    assert(dmw::impl::validate_parameter_name("plain"));
    assert(dmw::impl::validate_parameter_name("nested.name"));
    assert(dmw::impl::validate_parameter_name("_leading"));
    assert(!dmw::impl::validate_parameter_name(""));
    assert(!dmw::impl::validate_parameter_name(".leading"));
    assert(!dmw::impl::validate_parameter_name("trailing."));
    assert(!dmw::impl::validate_parameter_name("double..dot"));
    assert(!dmw::impl::validate_parameter_name("2starts_with_digit"));
    assert(!dmw::impl::validate_parameter_name("has-dash"));
    assert(
        dmw::impl::validate_parameter_name("").error().code() == ErrorCode::InvalidName);
}

void test_literal_parsing() {
    assert(dmw::impl::parse_parameter_literal("true").as_bool());
    assert(!dmw::impl::parse_parameter_literal("false").as_bool());
    assert(dmw::impl::parse_parameter_literal("-12").as_integer() == -12);
    assert(dmw::impl::parse_parameter_literal("2.5").as_double() == 2.5);
    assert(dmw::impl::parse_parameter_literal("1e3").as_double() == 1000.0);
    assert(dmw::impl::parse_parameter_literal("fast").as_string() == "fast");
    assert(
        dmw::impl::parse_parameter_literal("9223372036854775808").type() ==
        dmw::ParameterType::String);
    assert(
        dmw::impl::parse_parameter_literal("-9223372036854775808").as_integer() ==
        std::numeric_limits<std::int64_t>::min());
}

void test_store_declare_and_get() {
    dmw::impl::ParameterStoreState store(false, {});
    const auto declared = store.declare("use_sim_time", ParameterValue::make_bool(false), {}, false);
    assert(declared);
    assert(declared.value().name == "use_sim_time");
    assert(!declared.value().value.as_bool());

    const auto duplicate =
        store.declare("use_sim_time", ParameterValue::make_bool(true), {}, false);
    assert(!duplicate);
    assert(duplicate.error().code() == ErrorCode::AlreadyExists);

    assert(store.has("use_sim_time").value());
    assert(!store.has("missing").value());
    assert(store.get("use_sim_time").value().value.type() == dmw::ParameterType::Bool);
    assert(store.get("missing").error().code() == ErrorCode::NotFound);

    const auto many = store.get_many({"use_sim_time"});
    assert(many && many.value().size() == 1);
    assert(!store.get_many({"use_sim_time", "missing"}));

    const auto described = store.describe("use_sim_time");
    assert(described);
    assert(store.describe("missing").error().code() == ErrorCode::NotFound);

    assert(store.undeclare("use_sim_time"));
    assert(store.undeclare("use_sim_time").error().code() == ErrorCode::NotFound);
}

void test_typing_and_read_only() {
    dmw::impl::ParameterStoreState store(false, {});
    ParameterDescriptor fixed;
    assert(store.declare("count", integer(1), fixed, false));
    const auto mismatched = store.set_atomically({dmw::Parameter{"count", real(1.0)}});
    assert(!mismatched);
    assert(mismatched.error().code() == ErrorCode::InvalidArgument);
    assert(store.get("count").value().value.as_integer() == 1);

    ParameterDescriptor dynamic;
    dynamic.dynamic_typing = true;
    assert(store.declare("flexible", integer(1), dynamic, false));
    assert(store.set_atomically({dmw::Parameter{"flexible", ParameterValue::make_string("text")}}));
    assert(store.get("flexible").value().value.as_string() == "text");

    ParameterDescriptor read_only;
    read_only.read_only = true;
    assert(store.declare("frozen", integer(5), read_only, false));
    const auto rejected = store.set_atomically({dmw::Parameter{"frozen", integer(6)}});
    assert(!rejected);
    assert(rejected.error().code() == ErrorCode::InvalidState);
    assert(store.get("frozen").value().value.as_integer() == 5);

    // A fixed-type parameter needs a typed default value.
    const auto untyped = store.declare("untyped", ParameterValue{}, fixed, false);
    assert(!untyped);
    assert(untyped.error().code() == ErrorCode::InvalidArgument);
}

void test_ranges_and_steps() {
    dmw::impl::ParameterStoreState store(false, {});
    ParameterDescriptor ranged;
    ranged.integer_ranges.push_back(dmw::IntegerRange{0, 10, 2});
    assert(store.declare("level", integer(0), ranged, false));
    assert(store.set_atomically({dmw::Parameter{"level", integer(4)}}));
    const auto off_step = store.set_atomically({dmw::Parameter{"level", integer(5)}});
    assert(!off_step);
    assert(off_step.error().code() == ErrorCode::InvalidArgument);
    assert(!store.set_atomically({dmw::Parameter{"level", integer(11)}}));
    assert(!store.set_atomically({dmw::Parameter{"level", integer(-1)}}));
    assert(store.get("level").value().value.as_integer() == 4);

    ParameterDescriptor floating;
    floating.floating_point_ranges.push_back(dmw::FloatingPointRange{0.0, 1.0, 0.25});
    assert(store.declare("gain", real(0.0), floating, false));
    assert(store.set_atomically({dmw::Parameter{"gain", real(0.5)}}));
    assert(!store.set_atomically({dmw::Parameter{"gain", real(0.3)}}));
    assert(!store.set_atomically({dmw::Parameter{"gain", real(1.5)}}));
    assert(store.get("gain").value().value.as_double() == 0.5);

    ParameterDescriptor invalid;
    invalid.integer_ranges.push_back(dmw::IntegerRange{10, 0, 0});
    const auto rejected = store.declare("broken", integer(1), invalid, false);
    assert(!rejected);
    assert(rejected.error().code() == ErrorCode::InvalidArgument);
}

void test_atomic_set_and_change_set() {
    dmw::impl::ParameterStoreState store(false, {});
    assert(store.declare("alpha", integer(1), {}, false));
    assert(store.declare("beta", integer(2), {}, false));

    const auto taken = store.take_changes();
    assert(taken);
    assert(taken.value().new_parameters.size() == 2);

    // A failed atomic request must not apply any of its values.
    const auto failed = store.set_atomically({
        dmw::Parameter{"alpha", integer(10)},
        dmw::Parameter{"missing", integer(1)},
    });
    assert(!failed);
    assert(failed.error().code() == ErrorCode::NotFound);
    assert(store.get("alpha").value().value.as_integer() == 1);

    const auto duplicates = store.set_atomically({
        dmw::Parameter{"alpha", integer(3)},
        dmw::Parameter{"alpha", integer(4)},
    });
    assert(!duplicates);
    assert(duplicates.error().code() == ErrorCode::InvalidArgument);

    const auto committed = store.set_atomically({
        dmw::Parameter{"alpha", integer(3)},
        dmw::Parameter{"beta", integer(2)},
    });
    assert(committed);
    assert(committed.value().changed_parameters.size() == 1);
    assert(committed.value().changed_parameters.front().name == "alpha");
    assert(committed.value().new_parameters.empty());
    assert(store.get("alpha").value().value.as_integer() == 3);

    // validate() must not mutate anything.
    const auto validated = store.validate({dmw::Parameter{"alpha", integer(4)}});
    assert(validated);
    assert(store.get("alpha").value().value.as_integer() == 3);

    assert(store.undeclare("beta"));
    const auto deleted = store.take_changes();
    assert(deleted);
    assert(deleted.value().deleted_parameters.size() == 1);
    assert(deleted.value().deleted_parameters.front().name == "beta");
    assert(store.take_changes().value().empty());
}

void test_listing() {
    dmw::impl::ParameterStoreState store(false, {});
    assert(store.declare("a.b.c", integer(1), {}, false));
    assert(store.declare("a.b.d", integer(2), {}, false));
    assert(store.declare("a.e", integer(3), {}, false));

    const auto all = store.list({}, 0);
    assert(all);
    assert(all.value().names.size() == 3);
    assert(all.value().prefixes.empty());

    const auto shallow = store.list({}, 2);
    assert(shallow);
    assert(shallow.value().names.size() == 1);
    assert(shallow.value().names.front() == "a.e");
    assert(shallow.value().prefixes.size() == 1);
    assert(shallow.value().prefixes.front() == "a.b");

    const auto scoped = store.list({"a.b"}, 0);
    assert(scoped);
    assert(scoped.value().names.size() == 2);

    const auto empty = store.list({"nope"}, 0);
    assert(empty);
    assert(empty.value().names.empty());
}

void test_node_parameter_api_and_overrides() {
    auto context = dmw::Context::create({});
    assert(context);

    const auto arguments = dmw::parse_arguments({
        "--ros-args",
        "-p",
        "gain:=2.5",
        "-p",
        "/p/alpha:scoped:=7",
        "-p",
        "other:foreign:=1",
        "-p",
        "mode:=fast",
        "--",
    });
    assert(arguments);

    dmw::NodeOptions options;
    options.node_name = "alpha";
    options.node_namespace = "/p";
    options.arguments = arguments.value();
    auto node = context.value()->create_node(options);
    assert(node);
    assert(node.value()->fully_qualified_name() == "/p/alpha");

    // Only unscoped and matching scoped overrides are selected.
    assert(node.value()->parameter_overrides().size() == 3);

    const auto gain = node.value()->declare_parameter("gain", real(1.0));
    assert(gain);
    assert(gain.value().value.as_double() == 2.5);

    const auto scoped = node.value()->declare_parameter("scoped", integer(0));
    assert(scoped);
    assert(scoped.value().value.as_integer() == 7);

    const auto foreign = node.value()->declare_parameter("foreign", integer(0));
    assert(foreign);
    assert(foreign.value().value.as_integer() == 0);

    const auto ignored = node.value()->declare_parameter("gain", real(1.0), {}, true);
    assert(!ignored);
    assert(ignored.error().code() == ErrorCode::AlreadyExists);

    const auto overridden_once = node.value()->declare_parameter(
        "mode", ParameterValue::make_string("slow"), {}, false);
    assert(overridden_once);
    assert(overridden_once.value().value.as_string() == "fast");

    assert(node.value()->has_parameter("gain").value());
    assert(node.value()->get_parameter("gain").value().value.as_double() == 2.5);
    assert(node.value()->get_parameter("missing").error().code() == ErrorCode::NotFound);
    assert(node.value()->describe_parameter("gain"));
    assert(node.value()->list_parameters({}, 0).value().names.size() == 4);

    const auto changes = node.value()->take_parameter_changes();
    assert(changes);
    assert(changes.value().new_parameters.size() == 4);
    assert(node.value()->take_parameter_changes().value().empty());

    assert(node.value()->undeclare_parameter("mode"));
    const auto after_undeclare = node.value()->take_parameter_changes();
    assert(after_undeclare);
    assert(after_undeclare.value().deleted_parameters.size() == 1);

    const auto rejected = node.value()->set_parameters_atomically(
        {dmw::Parameter{"undeclared", integer(1)}});
    assert(!rejected);
    assert(rejected.error().code() == ErrorCode::NotFound);

    // allow_undeclared_parameters makes unknown parameters observable and
    // implicitly declared by an atomic set.
    dmw::NodeOptions permissive;
    permissive.node_name = "beta";
    permissive.allow_undeclared_parameters = true;
    auto permissive_node = context.value()->create_node(permissive);
    assert(permissive_node);
    const auto unset = permissive_node.value()->get_parameter("unknown");
    assert(unset);
    assert(unset.value().value.type() == dmw::ParameterType::NotSet);
    const auto created = permissive_node.value()->set_parameters_atomically(
        {dmw::Parameter{"unknown", integer(9)}});
    assert(created);
    assert(created.value().new_parameters.size() == 1);
    assert(permissive_node.value()->get_parameter("unknown").value().value.as_integer() == 9);
}

}  // namespace

int main() {
    test_value_types();
    test_name_validation();
    test_literal_parsing();
    test_store_declare_and_get();
    test_typing_and_read_only();
    test_ranges_and_steps();
    test_atomic_set_and_change_set();
    test_listing();
    test_node_parameter_api_and_overrides();
    return 0;
}
