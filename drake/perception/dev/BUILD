# -*- python -*-
# This file contains rules for Bazel; see drake/doc/bazel.rst.

load(
    "//tools:drake.bzl",
    "drake_cc_googletest",
    "drake_cc_library",
)
load("//tools:lint.bzl", "add_lint_tests")

package(default_visibility = ["//visibility:public"])

drake_cc_library(
    name = "neural_network",
    srcs = [
    ],
    hdrs = [
        "neural_network.h",
    ],
    deps = [
        "//drake/systems/framework",
    ],
)

drake_cc_library(
    name = "feedforward_neural_network",
    srcs = [
        "feedforward_neural_network.cc",
    ],
    hdrs = [
        "feedforward_neural_network.h",
    ],
    deps = [
        ":neural_network",
        "//drake/systems/framework",
    ],
)

# === test/ ===

drake_cc_googletest(
    name = "feedforward_neural_network_test",
    srcs = ["test/feedforward_neural_network_test.cc"],
    deps = [
        ":feedforward_neural_network",
        "//drake/common/test_utilities:eigen_matrix_compare",
    ],
)

add_lint_tests()
