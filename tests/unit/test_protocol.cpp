#include <gtest/gtest.h>

#include <string>

#include "support/test_helpers.hpp"
#include "telemetry/protocol/protocol.hpp"

using namespace telemetry;
using protocol::ParseError;

namespace {

ParseError error_of(const std::string& frame) { return protocol::decode(frame).error; }

}  // namespace

TEST(Protocol, EncodeDecodeRoundTrip) {
    const auto original = test::make_message("motor-007", 123456, 3021.75, 1729500000123456,
                                             SensorType::MotorSpeed, SensorStatus::Degraded);
    const std::string frame = protocol::encode(original);
    EXPECT_EQ(frame.find('\n'), std::string::npos) << "encoded frame must be a single line";

    const auto result = protocol::decode(frame);
    ASSERT_TRUE(result.ok()) << protocol::to_string(result.error) << " " << result.detail;
    const auto& m = *result.message;
    EXPECT_EQ(m.sensor_id, original.sensor_id);
    EXPECT_EQ(m.type, original.type);
    EXPECT_DOUBLE_EQ(m.value, original.value);
    EXPECT_EQ(m.timestamp_us, original.timestamp_us);
    EXPECT_EQ(m.sequence, original.sequence);
    EXPECT_EQ(m.status, original.status);
}

TEST(Protocol, RoundTripPreservesAllSensorTypes) {
    for (SensorType type : kAllSensorTypes) {
        auto m = test::make_message("s1", 1, -12.5, 1000, type);
        const auto result = protocol::decode(protocol::encode(m));
        ASSERT_TRUE(result.ok());
        EXPECT_EQ(result.message->type, type);
    }
}

TEST(Protocol, StatusIsOptionalAndDefaultsToOk) {
    const auto result =
        protocol::decode(R"({"id":"t1","type":"temperature","value":1,"ts":5,"seq":0})");
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.message->status, SensorStatus::Ok);
}

TEST(Protocol, UnknownFieldsAreIgnored) {
    const auto result = protocol::decode(
        R"({"id":"t1","type":"temperature","value":1,"ts":5,"seq":0,"firmware":"2.1"})");
    EXPECT_TRUE(result.ok());
}

TEST(Protocol, IntegerValueIsAccepted) {
    const auto result =
        protocol::decode(R"({"id":"t1","type":"pressure","value":101,"ts":5,"seq":0})");
    ASSERT_TRUE(result.ok());
    EXPECT_DOUBLE_EQ(result.message->value, 101.0);
}

TEST(Protocol, RejectsStructuralErrors) {
    EXPECT_EQ(error_of(""), ParseError::Empty);
    EXPECT_EQ(error_of("not json"), ParseError::MalformedJson);
    EXPECT_EQ(error_of(R"({"id":"t1","type":"temperature")"), ParseError::MalformedJson);
    EXPECT_EQ(error_of("[1,2,3]"), ParseError::NotAnObject);
    EXPECT_EQ(error_of("42"), ParseError::NotAnObject);
}

TEST(Protocol, RejectsMissingFields) {
    EXPECT_EQ(error_of(R"({"type":"temperature","value":1,"ts":5,"seq":0})"),
              ParseError::MissingField);
    EXPECT_EQ(error_of(R"({"id":"t1","value":1,"ts":5,"seq":0})"), ParseError::MissingField);
    EXPECT_EQ(error_of(R"({"id":"t1","type":"temperature","ts":5,"seq":0})"),
              ParseError::MissingField);
    EXPECT_EQ(error_of(R"({"id":"t1","type":"temperature","value":1,"seq":0})"),
              ParseError::MissingField);
    EXPECT_EQ(error_of(R"({"id":"t1","type":"temperature","value":1,"ts":5})"),
              ParseError::MissingField);
}

TEST(Protocol, RejectsWrongTypes) {
    EXPECT_EQ(error_of(R"({"id":7,"type":"temperature","value":1,"ts":5,"seq":0})"),
              ParseError::WrongFieldType);
    EXPECT_EQ(error_of(R"({"id":"t1","type":"temperature","value":"1","ts":5,"seq":0})"),
              ParseError::WrongFieldType);
    EXPECT_EQ(error_of(R"({"id":"t1","type":"temperature","value":1,"ts":5,"seq":-1})"),
              ParseError::WrongFieldType);
    EXPECT_EQ(error_of(R"({"id":"t1","type":"temperature","value":1,"ts":5,"seq":1.5})"),
              ParseError::WrongFieldType);
    EXPECT_EQ(error_of(R"({"id":"t1","type":"temperature","value":1,"ts":5,"seq":0,"status":1})"),
              ParseError::WrongFieldType);
}

TEST(Protocol, RejectsInvalidValues) {
    EXPECT_EQ(error_of(R"({"id":"","type":"temperature","value":1,"ts":5,"seq":0})"),
              ParseError::InvalidSensorId);
    EXPECT_EQ(error_of(R"({"id":"has space","type":"temperature","value":1,"ts":5,"seq":0})"),
              ParseError::InvalidSensorId);
    EXPECT_EQ(error_of(R"({"id":"t1","type":"humidity","value":1,"ts":5,"seq":0})"),
              ParseError::UnknownSensorType);
    EXPECT_EQ(error_of(R"({"id":"t1","type":"temperature","value":1,"ts":5,"seq":0,"status":"meh"})"),
              ParseError::UnknownStatus);
    EXPECT_EQ(error_of(R"({"id":"t1","type":"temperature","value":1,"ts":0,"seq":0})"),
              ParseError::InvalidTimestamp);
    EXPECT_EQ(error_of(R"({"id":"t1","type":"temperature","value":1,"ts":-5,"seq":0})"),
              ParseError::InvalidTimestamp);
    EXPECT_EQ(error_of(R"({"id":"t1","type":"temperature","value":1,"ts":1.5,"seq":0})"),
              ParseError::InvalidTimestamp);
    EXPECT_EQ(error_of(R"({"id":"t1","type":"temperature","value":1,"ts":18446744073709551615,"seq":0})"),
              ParseError::InvalidTimestamp);
}

TEST(Protocol, RejectsNonFiniteValue) {
    // JSON has no NaN/Infinity literals, and nlohmann::json reports a number overflow
    // such as 1e999 as a parse error, so non-finite values cannot reach the decoder's
    // isfinite() check (kept as defense in depth).
    EXPECT_EQ(error_of(R"({"id":"t1","type":"temperature","value":1e999,"ts":5,"seq":0})"),
              ParseError::MalformedJson);
    EXPECT_EQ(error_of(R"({"id":"t1","type":"temperature","value":NaN,"ts":5,"seq":0})"),
              ParseError::MalformedJson);
}

TEST(Protocol, SensorIdRules) {
    EXPECT_TRUE(protocol::is_valid_sensor_id("temp-001"));
    EXPECT_TRUE(protocol::is_valid_sensor_id("A.b_C-9"));
    EXPECT_TRUE(protocol::is_valid_sensor_id(std::string(protocol::kMaxSensorIdLength, 'x')));
    EXPECT_FALSE(protocol::is_valid_sensor_id(std::string(protocol::kMaxSensorIdLength + 1, 'x')));
    EXPECT_FALSE(protocol::is_valid_sensor_id(""));
    EXPECT_FALSE(protocol::is_valid_sensor_id("bad/id"));
    EXPECT_FALSE(protocol::is_valid_sensor_id("quote\""));
}
