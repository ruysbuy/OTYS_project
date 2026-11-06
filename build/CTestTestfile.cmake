# CMake generated Testfile for 
# Source directory: /home/xilinx/Desktop/otus/project
# Build directory: /home/xilinx/Desktop/otus/project/build
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[integration_container]=] "/home/xilinx/Desktop/otus/project/tests/integration_container.sh" "/home/xilinx/Desktop/otus/project/build/minidocker" "/home/xilinx/Desktop/otus/project/build/minidockerd")
set_tests_properties([=[integration_container]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/xilinx/Desktop/otus/project/CMakeLists.txt;41;add_test;/home/xilinx/Desktop/otus/project/CMakeLists.txt;0;")
add_test([=[daemon_protocol]=] "/home/xilinx/Desktop/otus/project/tests/daemon_protocol.sh" "/home/xilinx/Desktop/otus/project/build/minidocker" "/home/xilinx/Desktop/otus/project/build/minidockerd")
set_tests_properties([=[daemon_protocol]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/xilinx/Desktop/otus/project/CMakeLists.txt;44;add_test;/home/xilinx/Desktop/otus/project/CMakeLists.txt;0;")
