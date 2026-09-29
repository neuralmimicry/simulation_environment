CXX_SOURCES = nm_api_robot_controller.cpp
WEBOTS_HOME ?= /usr/local/webots
AARNN_INCLUDE_ROOT ?= $(realpath $(CURDIR)/../../../../aarnn_rust/include)
INCLUDE += -I"$(AARNN_INCLUDE_ROOT)"
LIBRARIES += -lcurl
CXXFLAGS += -std=c++17
include $(WEBOTS_HOME)/resources/Makefile.include
