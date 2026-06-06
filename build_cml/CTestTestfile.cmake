# CMake generated Testfile for 
# Source directory: /home/ayush/agentty
# Build directory: /home/ayush/agentty/build_cml
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[long_session_bench]=] "/home/ayush/agentty/build_cml/long_session_bench")
set_tests_properties([=[long_session_bench]=] PROPERTIES  TIMEOUT "600" _BACKTRACE_TRIPLES "/home/ayush/agentty/CMakeLists.txt;815;add_test;/home/ayush/agentty/CMakeLists.txt;0;")
add_test([=[fuzzy_match_smoke]=] "/home/ayush/agentty/build_cml/fuzzy_match_smoke")
set_tests_properties([=[fuzzy_match_smoke]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/ayush/agentty/CMakeLists.txt;901;add_test;/home/ayush/agentty/CMakeLists.txt;0;")
add_test([=[param_tag_repair_test]=] "/home/ayush/agentty/build_cml/param_tag_repair_test")
set_tests_properties([=[param_tag_repair_test]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/ayush/agentty/CMakeLists.txt;913;add_test;/home/ayush/agentty/CMakeLists.txt;0;")
add_test([=[midrun_freeze_test]=] "/home/ayush/agentty/build_cml/midrun_freeze_test")
set_tests_properties([=[midrun_freeze_test]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/ayush/agentty/CMakeLists.txt;950;add_test;/home/ayush/agentty/CMakeLists.txt;0;")
add_test([=[midrun_seam_test]=] "/home/ayush/agentty/build_cml/midrun_seam_test")
set_tests_properties([=[midrun_seam_test]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/ayush/agentty/CMakeLists.txt;988;add_test;/home/ayush/agentty/CMakeLists.txt;0;")
add_test([=[midrun_wire_test]=] "/home/ayush/agentty/build_cml/midrun_wire_test")
set_tests_properties([=[midrun_wire_test]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/ayush/agentty/CMakeLists.txt;1026;add_test;/home/ayush/agentty/CMakeLists.txt;0;")
add_test([=[table_render_test]=] "/home/ayush/agentty/build_cml/table_render_test")
set_tests_properties([=[table_render_test]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/ayush/agentty/CMakeLists.txt;1035;add_test;/home/ayush/agentty/CMakeLists.txt;0;")
add_test([=[visual_hash_coverage_test]=] "/home/ayush/agentty/build_cml/visual_hash_coverage_test")
set_tests_properties([=[visual_hash_coverage_test]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/ayush/agentty/CMakeLists.txt;1075;add_test;/home/ayush/agentty/CMakeLists.txt;0;")
subdirs("maya")
subdirs("_deps/nlohmann_json-build")
subdirs("_deps/simdjson-build")
