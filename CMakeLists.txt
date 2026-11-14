# SPDX-License-Identifier: Apache-2.0
# Top-level ESP-IDF project. The portable engine lives in components/fmengine and is
# pulled in by firmware/main/CMakeLists.txt; it has no IDF dependency, so the host gates in
# tools/run_host_tests.sh build the same sources.
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(cardputer_fm1)
