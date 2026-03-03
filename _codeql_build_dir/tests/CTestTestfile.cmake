# CMake generated Testfile for 
# Source directory: /home/runner/work/libheif/libheif/tests
# Build directory: /home/runner/work/libheif/libheif/_codeql_build_dir/tests
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[encode]=] "./encode")
set_tests_properties([=[encode]=] PROPERTIES  ENVIRONMENT "LIBHEIF_PLUGIN_PATH=/home/runner/work/libheif/libheif/_codeql_build_dir/libheif/plugins" SKIP_REGULAR_EXPRESSION "[1-9][0-9]* skipped" _BACKTRACE_TRIPLES "/home/runner/work/libheif/libheif/tests/CMakeLists.txt;21;add_test;/home/runner/work/libheif/libheif/tests/CMakeLists.txt;58;add_libheif_test;/home/runner/work/libheif/libheif/tests/CMakeLists.txt;0;")
add_test([=[extended_type]=] "./extended_type")
set_tests_properties([=[extended_type]=] PROPERTIES  ENVIRONMENT "LIBHEIF_PLUGIN_PATH=/home/runner/work/libheif/libheif/_codeql_build_dir/libheif/plugins" SKIP_REGULAR_EXPRESSION "[1-9][0-9]* skipped" _BACKTRACE_TRIPLES "/home/runner/work/libheif/libheif/tests/CMakeLists.txt;21;add_test;/home/runner/work/libheif/libheif/tests/CMakeLists.txt;59;add_libheif_test;/home/runner/work/libheif/libheif/tests/CMakeLists.txt;0;")
add_test([=[region]=] "./region")
set_tests_properties([=[region]=] PROPERTIES  ENVIRONMENT "LIBHEIF_PLUGIN_PATH=/home/runner/work/libheif/libheif/_codeql_build_dir/libheif/plugins" SKIP_REGULAR_EXPRESSION "[1-9][0-9]* skipped" _BACKTRACE_TRIPLES "/home/runner/work/libheif/libheif/tests/CMakeLists.txt;21;add_test;/home/runner/work/libheif/libheif/tests/CMakeLists.txt;60;add_libheif_test;/home/runner/work/libheif/libheif/tests/CMakeLists.txt;0;")
add_test([=[tai]=] "./tai")
set_tests_properties([=[tai]=] PROPERTIES  ENVIRONMENT "LIBHEIF_PLUGIN_PATH=/home/runner/work/libheif/libheif/_codeql_build_dir/libheif/plugins" SKIP_REGULAR_EXPRESSION "[1-9][0-9]* skipped" _BACKTRACE_TRIPLES "/home/runner/work/libheif/libheif/tests/CMakeLists.txt;21;add_test;/home/runner/work/libheif/libheif/tests/CMakeLists.txt;61;add_libheif_test;/home/runner/work/libheif/libheif/tests/CMakeLists.txt;0;")
add_test([=[text]=] "./text")
set_tests_properties([=[text]=] PROPERTIES  ENVIRONMENT "LIBHEIF_PLUGIN_PATH=/home/runner/work/libheif/libheif/_codeql_build_dir/libheif/plugins" SKIP_REGULAR_EXPRESSION "[1-9][0-9]* skipped" _BACKTRACE_TRIPLES "/home/runner/work/libheif/libheif/tests/CMakeLists.txt;21;add_test;/home/runner/work/libheif/libheif/tests/CMakeLists.txt;62;add_libheif_test;/home/runner/work/libheif/libheif/tests/CMakeLists.txt;0;")
add_test([=[cxx_wrapper]=] "./cxx_wrapper")
set_tests_properties([=[cxx_wrapper]=] PROPERTIES  ENVIRONMENT "LIBHEIF_PLUGIN_PATH=/home/runner/work/libheif/libheif/_codeql_build_dir/libheif/plugins" SKIP_REGULAR_EXPRESSION "[1-9][0-9]* skipped" _BACKTRACE_TRIPLES "/home/runner/work/libheif/libheif/tests/CMakeLists.txt;21;add_test;/home/runner/work/libheif/libheif/tests/CMakeLists.txt;63;add_libheif_test;/home/runner/work/libheif/libheif/tests/CMakeLists.txt;0;")
add_test([=[omaf]=] "./omaf")
set_tests_properties([=[omaf]=] PROPERTIES  ENVIRONMENT "LIBHEIF_PLUGIN_PATH=/home/runner/work/libheif/libheif/_codeql_build_dir/libheif/plugins" SKIP_REGULAR_EXPRESSION "[1-9][0-9]* skipped" _BACKTRACE_TRIPLES "/home/runner/work/libheif/libheif/tests/CMakeLists.txt;21;add_test;/home/runner/work/libheif/libheif/tests/CMakeLists.txt;88;add_libheif_test;/home/runner/work/libheif/libheif/tests/CMakeLists.txt;0;")
add_test([=[tiffdecode]=] "./tiffdecode")
set_tests_properties([=[tiffdecode]=] PROPERTIES  ENVIRONMENT "LIBHEIF_PLUGIN_PATH=/home/runner/work/libheif/libheif/_codeql_build_dir/libheif/plugins" SKIP_REGULAR_EXPRESSION "[1-9][0-9]* skipped" _BACKTRACE_TRIPLES "/home/runner/work/libheif/libheif/tests/CMakeLists.txt;30;add_test;/home/runner/work/libheif/libheif/tests/CMakeLists.txt;116;add_heifio_test;/home/runner/work/libheif/libheif/tests/CMakeLists.txt;0;")
