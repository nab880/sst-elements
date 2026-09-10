#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <fstream>
#include <sstream>

#include "../../quetz_accelerator_event_writer.h"

using SST::Quetz::AcceleratorEventWriter;

static std::string readFile(const char* path)
{
    std::ifstream input(path);
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

TEST_CASE("event identifiers match the root schema") {
    CHECK(AcceleratorEventWriter::isIdentifier("accelerator.fft"));
    CHECK(AcceleratorEventWriter::isIdentifier("fft-256_v1"));
    CHECK_FALSE(AcceleratorEventWriter::isIdentifier(""));
    CHECK_FALSE(AcceleratorEventWriter::isIdentifier("Accelerator.fft"));
    CHECK_FALSE(AcceleratorEventWriter::isIdentifier("fft/256"));
    CHECK_FALSE(AcceleratorEventWriter::isIdentifier("_fft"));
}

TEST_CASE("records flush immediately and preserve lifecycle metadata") {
    const char* path = "/tmp/quetz-test-accelerator-events.jsonl";
    AcceleratorEventWriter writer;
    std::string error;
    REQUIRE(writer.configure(path, "accelerator.fft", "fft", error));
    REQUIRE(writer.emitRequested(17, 3, error));
    CHECK(readFile(path) ==
        "{\"schema_version\":1,\"sequence\":0,\"sim_time_ns\":17,"
        "\"source\":\"accelerator.fft\",\"kind\":\"accelerator-requested\","
        "\"payload\":{\"operation_id\":3,\"operation\":\"fft\"}}\n");
    REQUIRE(writer.emitCompleted(29, 3, error));
    const std::string text = readFile(path);
    CHECK(text.find("\"sequence\":1") != std::string::npos);
    CHECK(text.find("\"kind\":\"accelerator-completed\"") != std::string::npos);
    CHECK(text.back() == '\n');
}

TEST_CASE("rejection is a paired terminal error") {
    const char* path = "/tmp/quetz-test-accelerator-error.jsonl";
    AcceleratorEventWriter writer;
    std::string error;
    REQUIRE(writer.configure(path, "accelerator.fft", "fft", error));
    REQUIRE(writer.emitRequested(4, 9, error));
    REQUIRE(writer.emitError(4, 9, "operation-rejected", error));
    const std::string text = readFile(path);
    CHECK(text.find("\"kind\":\"accelerator-error\"") != std::string::npos);
    CHECK(text.find("\"operation_id\":9") != std::string::npos);
    CHECK(text.find("\"operation\":\"fft\"") != std::string::npos);
    CHECK(text.find("\"code\":\"operation-rejected\"") != std::string::npos);
}

TEST_CASE("invalid configured identifiers fail closed") {
    std::string error;
    AcceleratorEventWriter bad;
    CHECK_FALSE(bad.configure("/tmp/quetz-test-invalid-events.jsonl",
                              "Accelerator.fft", "fft", error));
    CHECK_FALSE(error.empty());
    AcceleratorEventWriter disabled;
    CHECK(disabled.configure("", "unused", "unused", error));
    CHECK(disabled.emitRequested(1, 1, error));
}

TEST_CASE("CPU checkpoints record observed busy state in lifecycle order") {
    const char* path = "/tmp/quetz-test-cpu-checkpoints.jsonl";
    AcceleratorEventWriter writer;
    std::string error;
    REQUIRE(writer.configure(path, "accelerator.fft", "fft", error));
    REQUIRE(writer.emitRequested(10, 1, error));
    REQUIRE(writer.emitCpuCheckpoint(11, 1, 2166136261u, true, error));
    CHECK(readFile(path).find("\"value\":2166136261,\"busy\":true") != std::string::npos);
    REQUIRE(writer.emitCompleted(20, 1, error));
    REQUIRE(writer.emitCpuCheckpoint(21, 1, 2677368773u, false, error));
    const std::string text = readFile(path);
    CHECK(text.find("\"sequence\":3,\"sim_time_ns\":21") != std::string::npos);
    CHECK(text.find("\"value\":2677368773,\"busy\":false") != std::string::npos);
    CHECK(text.find("\"kind\":\"cpu-checkpoint\"") != std::string::npos);
}
