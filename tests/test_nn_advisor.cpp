/*
BALANCER_META:
  meta_version: 1
  component: NNAdvisor
  file_role: test
  path: tests/test_nn_advisor.cpp
  namespace: balancer::testing
  layer: Testing
  summary: >
    Functional tests for NNAdvisor. Covers: healthy-state inference file,
    overload/latency/node_fault alerts, kAlertNone synonym, empty-string
    healthy state, idempotent evaluate (same file, same alert), alert transition
    on file overwrite, revert to healthy, missing file error, malformed JSON
    error, missing active_alert field error, unknown alert name error,
    non-string active_alert error, non-object root error, and accessor tests.
  api_stability: internal
  related:
    headers:
      - include/balancer/NNAdvisor.h
      - include/balancer/FeatureSupervisor.h
      - include/balancer/BalancerFeatures.h
  hygiene:
    pragma_once: false
    defines_total: 0
    defines_unprefixed: 0
    undefs_total: 0
    includes_windows_h: false
*/

/**
 * @file test_nn_advisor.cpp
 * @brief Functional tests for NNAdvisor.
 */

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "FatPTest.h"

#include "balancer/BalancerConfig.h"
#include "balancer/BalancerFeatures.h"
#include "balancer/FeatureSupervisor.h"
#include "balancer/NNAdvisor.h"

#include "policies/RoundRobin.h"
#include "sim/SimulatedCluster.h"

using fat_p::testing::TestRunner;

namespace balancer::testing::nnadvisornss
{

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

inline sim::SimulatedClusterConfig makeClusterCfg()
{
    sim::SimulatedClusterConfig cfg;
    cfg.nodeCount         = 4;
    cfg.workerCount       = 1;
    cfg.overloadThreshold = 20;
    return cfg;
}

/// Write a JSON inference file with the given active_alert value.
inline void writeInferenceFile(const std::string& path, const std::string& alert)
{
    std::ofstream f(path, std::ios::trunc);
    f << "{\"active_alert\":\"" << alert << "\"}";
}

/// Write a JSON inference file with extra fields (confidence etc.).
inline void writeInferenceFileWithExtra(const std::string& path,
                                        const std::string& alert,
                                        float confidence)
{
    std::ofstream f(path, std::ios::trunc);
    f << "{\"active_alert\":\"" << alert
      << "\",\"confidence\":" << confidence << "}";
}

/// Returns a path inside the system temp directory with the given filename.
inline std::string tmpPath(const char* filename)
{
    return (std::filesystem::temp_directory_path() / filename).string();
}

/// RAII helper: removes a file when it goes out of scope.
struct TempFile
{
    std::string path;
    explicit TempFile(const char* filename) : path(tmpPath(filename)) {}
    ~TempFile() { std::remove(path.c_str()); }
};

/// Build a Balancer + FeatureSupervisor pair with N SimulatedNodes.
struct TestFixture
{
    sim::SimulatedCluster                 cluster;
    BalancerConfig                        cfg;
    std::unique_ptr<Balancer>             balancer;
    std::unique_ptr<FeatureSupervisor>    supervisor;

    explicit TestFixture(int nodeCount = 4)
        : cluster([&]{
            sim::SimulatedClusterConfig c;
            c.nodeCount         = static_cast<uint32_t>(nodeCount);
            c.workerCount       = 1;
            c.overloadThreshold = 20;
            return c;
          }())
    {
        balancer   = std::make_unique<Balancer>(
            cluster.nodes(),
            std::make_unique<RoundRobin>(),
            cfg);
        supervisor = std::make_unique<FeatureSupervisor>(*balancer);
    }
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

FATP_TEST_CASE(reads_healthy_empty_string)
{
    TestFixture fx;
    TempFile    tf{"test_nn_healthy_empty.json"};
    writeInferenceFile(tf.path, "");

    NNAdvisor advisor{*fx.supervisor, tf.path};

    auto result = advisor.evaluate();
    FATP_ASSERT_TRUE(result.has_value(), "evaluate() should succeed for empty alert");
    FATP_ASSERT_TRUE(advisor.currentAlert().empty(),
        "currentAlert should be empty for healthy state");

    return true;
}

FATP_TEST_CASE(reads_alert_none_synonym)
{
    TestFixture fx;
    TempFile    tf{"test_nn_alert_none.json"};
    writeInferenceFile(tf.path, std::string(features::kAlertNone));

    NNAdvisor advisor{*fx.supervisor, tf.path};

    auto result = advisor.evaluate();
    FATP_ASSERT_TRUE(result.has_value(), "evaluate() should succeed for kAlertNone");
    // FeatureSupervisor normalises kAlertNone to empty string in activeAlert().
    FATP_ASSERT_TRUE(advisor.currentAlert().empty(),
        "currentAlert should be empty after kAlertNone transition");

    return true;
}

FATP_TEST_CASE(reads_overload_alert)
{
    TestFixture fx;
    TempFile    tf{"test_nn_overload.json"};
    writeInferenceFile(tf.path, std::string(features::kAlertOverload));

    NNAdvisor advisor{*fx.supervisor, tf.path};

    auto result = advisor.evaluate();
    FATP_ASSERT_TRUE(result.has_value(), "evaluate() should succeed for overload alert");
    FATP_ASSERT_EQ(std::string(advisor.currentAlert()),
                   std::string(features::kAlertOverload),
                   "currentAlert should be kAlertOverload");

    return true;
}

FATP_TEST_CASE(reads_latency_alert)
{
    TestFixture fx;
    TempFile    tf{"test_nn_latency.json"};
    writeInferenceFile(tf.path, std::string(features::kAlertLatency));

    NNAdvisor advisor{*fx.supervisor, tf.path};

    auto result = advisor.evaluate();
    FATP_ASSERT_TRUE(result.has_value(), "evaluate() should succeed for latency alert");
    FATP_ASSERT_EQ(std::string(advisor.currentAlert()),
                   std::string(features::kAlertLatency),
                   "currentAlert should be kAlertLatency");

    return true;
}

FATP_TEST_CASE(reads_node_fault_alert)
{
    TestFixture fx;
    TempFile    tf{"test_nn_node_fault.json"};
    writeInferenceFile(tf.path, std::string(features::kAlertNodeFault));

    NNAdvisor advisor{*fx.supervisor, tf.path};

    auto result = advisor.evaluate();
    FATP_ASSERT_TRUE(result.has_value(), "evaluate() should succeed for node_fault alert");
    FATP_ASSERT_EQ(std::string(advisor.currentAlert()),
                   std::string(features::kAlertNodeFault),
                   "currentAlert should be kAlertNodeFault");

    return true;
}

FATP_TEST_CASE(extra_fields_silently_ignored)
{
    TestFixture fx;
    TempFile    tf{"test_nn_extra_fields.json"};
    writeInferenceFileWithExtra(tf.path, std::string(features::kAlertOverload), 0.87f);

    NNAdvisor advisor{*fx.supervisor, tf.path};

    auto result = advisor.evaluate();
    FATP_ASSERT_TRUE(result.has_value(), "evaluate() should succeed when extra fields present");
    FATP_ASSERT_EQ(std::string(advisor.currentAlert()),
                   std::string(features::kAlertOverload),
                   "currentAlert should be kAlertOverload despite extra fields");

    return true;
}

FATP_TEST_CASE(idempotent_evaluate_same_file)
{
    // Two consecutive evaluate() calls with the same file content must both
    // succeed and leave the supervisor in the same alert state.
    TestFixture fx;
    TempFile    tf{"test_nn_idempotent.json"};
    writeInferenceFile(tf.path, std::string(features::kAlertOverload));

    NNAdvisor advisor{*fx.supervisor, tf.path};

    auto r1 = advisor.evaluate();
    FATP_ASSERT_TRUE(r1.has_value(), "first evaluate() should succeed");

    auto r2 = advisor.evaluate();
    FATP_ASSERT_TRUE(r2.has_value(), "second evaluate() should succeed");
    FATP_ASSERT_EQ(std::string(advisor.currentAlert()),
                   std::string(features::kAlertOverload),
                   "alert should remain kAlertOverload after repeated evaluate()");

    return true;
}

FATP_TEST_CASE(alert_transition_overload_to_latency)
{
    TestFixture fx;
    TempFile    tf{"test_nn_transition.json"};
    writeInferenceFile(tf.path, std::string(features::kAlertOverload));

    NNAdvisor advisor{*fx.supervisor, tf.path};
    FATP_ASSERT_TRUE(advisor.evaluate().has_value(), "initial overload evaluate should succeed");
    FATP_ASSERT_EQ(std::string(advisor.currentAlert()),
                   std::string(features::kAlertOverload),
                   "alert should be kAlertOverload after first evaluate");

    // Overwrite the file — next evaluate() reads it fresh unconditionally.
    writeInferenceFile(tf.path, std::string(features::kAlertLatency));

    auto r2 = advisor.evaluate();
    FATP_ASSERT_TRUE(r2.has_value(), "latency evaluate should succeed");
    FATP_ASSERT_EQ(std::string(advisor.currentAlert()),
                   std::string(features::kAlertLatency),
                   "alert should transition to kAlertLatency");

    return true;
}

FATP_TEST_CASE(revert_to_healthy)
{
    TestFixture fx;
    TempFile    tf{"test_nn_revert.json"};
    writeInferenceFile(tf.path, std::string(features::kAlertOverload));

    NNAdvisor advisor{*fx.supervisor, tf.path};
    FATP_ASSERT_TRUE(advisor.evaluate().has_value(), "overload evaluate should succeed");

    // Fresh advisor for the revert, forcing a new file read.
    writeInferenceFile(tf.path, "");
    NNAdvisor advisor2{*fx.supervisor, tf.path};
    auto r2 = advisor2.evaluate();
    FATP_ASSERT_TRUE(r2.has_value(), "healthy revert evaluate should succeed");
    FATP_ASSERT_TRUE(advisor2.currentAlert().empty(),
        "currentAlert should be empty after revert to healthy");

    return true;
}

FATP_TEST_CASE(missing_file_returns_error)
{
    TestFixture fx;
    NNAdvisor advisor{*fx.supervisor, tmpPath("this_file_does_not_exist_nnadvisor.json")};

    auto result = advisor.evaluate();
    FATP_ASSERT_TRUE(!result.has_value(), "evaluate() should fail for missing file");
    FATP_ASSERT_TRUE(result.error().find("failed to read/parse") != std::string::npos,
        "error message should mention read/parse failure");

    return true;
}

FATP_TEST_CASE(malformed_json_returns_error)
{
    TestFixture fx;
    TempFile    tf{"test_nn_malformed.json"};
    {
        std::ofstream f(tf.path);
        f << "this is not json {{{";
    }

    NNAdvisor advisor{*fx.supervisor, tf.path};
    auto result = advisor.evaluate();
    FATP_ASSERT_TRUE(!result.has_value(), "evaluate() should fail for malformed JSON");
    FATP_ASSERT_TRUE(result.error().find("failed to read/parse") != std::string::npos,
        "error message should mention parse failure");

    return true;
}

FATP_TEST_CASE(missing_active_alert_field_returns_error)
{
    TestFixture fx;
    TempFile    tf{"test_nn_no_field.json"};
    {
        std::ofstream f(tf.path);
        f << "{\"confidence\":0.9}";
    }

    NNAdvisor advisor{*fx.supervisor, tf.path};
    auto result = advisor.evaluate();
    FATP_ASSERT_TRUE(!result.has_value(),
        "evaluate() should fail when active_alert field is missing");
    FATP_ASSERT_TRUE(result.error().find("active_alert") != std::string::npos,
        "error message should name the missing field");

    return true;
}

FATP_TEST_CASE(unknown_alert_name_returns_error)
{
    TestFixture fx;
    TempFile    tf{"test_nn_unknown_alert.json"};
    writeInferenceFile(tf.path, "alert.made_up_by_nn");

    NNAdvisor advisor{*fx.supervisor, tf.path};
    auto result = advisor.evaluate();
    FATP_ASSERT_TRUE(!result.has_value(),
        "evaluate() should fail for an unknown alert name");
    FATP_ASSERT_TRUE(result.error().find("unknown alert") != std::string::npos,
        "error message should say 'unknown alert'");

    return true;
}

FATP_TEST_CASE(non_string_active_alert_returns_error)
{
    TestFixture fx;
    TempFile    tf{"test_nn_nonstring.json"};
    {
        std::ofstream f(tf.path);
        f << "{\"active_alert\":42}";
    }

    NNAdvisor advisor{*fx.supervisor, tf.path};
    auto result = advisor.evaluate();
    FATP_ASSERT_TRUE(!result.has_value(),
        "evaluate() should fail when active_alert is not a string");

    return true;
}

FATP_TEST_CASE(non_object_root_returns_error)
{
    TestFixture fx;
    TempFile    tf{"test_nn_array_root.json"};
    {
        std::ofstream f(tf.path);
        f << "[\"alert.overload\"]";
    }

    NNAdvisor advisor{*fx.supervisor, tf.path};
    auto result = advisor.evaluate();
    FATP_ASSERT_TRUE(!result.has_value(),
        "evaluate() should fail when JSON root is not an object");
    FATP_ASSERT_TRUE(result.error().find("root must be") != std::string::npos,
        "error message should describe the root type requirement");

    return true;
}

FATP_TEST_CASE(accessor_inference_file_path)
{
    TestFixture fx;
    const std::string path = tmpPath("test_nn_accessor.json");
    NNAdvisor advisor{*fx.supervisor, path};
    FATP_ASSERT_EQ(advisor.inferenceFilePath(), path,
        "inferenceFilePath() should return the path passed at construction");
    return true;
}

FATP_TEST_CASE(accessor_supervisor)
{
    TestFixture fx;
    NNAdvisor advisor{*fx.supervisor, tmpPath("x.json")};
    // Just verify it returns a reference to the same object.
    FATP_ASSERT_TRUE(&advisor.supervisor() == fx.supervisor.get(),
        "supervisor() should return the supervised FeatureSupervisor");
    return true;
}

} // namespace balancer::testing::nnadvisornss

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

namespace balancer::testing
{

bool test_NNAdvisor()
{
    FATP_PRINT_HEADER(NNADVISOR)

    TestRunner runner;

    FATP_RUN_TEST_NS(runner, nnadvisornss, reads_healthy_empty_string);
    FATP_RUN_TEST_NS(runner, nnadvisornss, reads_alert_none_synonym);
    FATP_RUN_TEST_NS(runner, nnadvisornss, reads_overload_alert);
    FATP_RUN_TEST_NS(runner, nnadvisornss, reads_latency_alert);
    FATP_RUN_TEST_NS(runner, nnadvisornss, reads_node_fault_alert);
    FATP_RUN_TEST_NS(runner, nnadvisornss, extra_fields_silently_ignored);
    FATP_RUN_TEST_NS(runner, nnadvisornss, idempotent_evaluate_same_file);
    FATP_RUN_TEST_NS(runner, nnadvisornss, alert_transition_overload_to_latency);
    FATP_RUN_TEST_NS(runner, nnadvisornss, revert_to_healthy);
    FATP_RUN_TEST_NS(runner, nnadvisornss, missing_file_returns_error);
    FATP_RUN_TEST_NS(runner, nnadvisornss, malformed_json_returns_error);
    FATP_RUN_TEST_NS(runner, nnadvisornss, missing_active_alert_field_returns_error);
    FATP_RUN_TEST_NS(runner, nnadvisornss, unknown_alert_name_returns_error);
    FATP_RUN_TEST_NS(runner, nnadvisornss, non_string_active_alert_returns_error);
    FATP_RUN_TEST_NS(runner, nnadvisornss, non_object_root_returns_error);
    FATP_RUN_TEST_NS(runner, nnadvisornss, accessor_inference_file_path);
    FATP_RUN_TEST_NS(runner, nnadvisornss, accessor_supervisor);

    return 0 == runner.print_summary();
}

} // namespace balancer::testing

#ifdef ENABLE_TEST_APPLICATION
int main()
{
    return balancer::testing::test_NNAdvisor() ? 0 : 1;
}
#endif
