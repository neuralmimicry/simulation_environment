CXX_SOURCES = nm_ecology_supervisor.cpp
WEBOTS_HOME ?= /usr/local/webots
.DEFAULT_GOAL := all
CXXFLAGS += -std=c++17
.PHONY: test-policy
test-policy:
	@policy_test="$$(mktemp)"; trap 'rm -f "$$policy_test"' EXIT; \
	$(CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -I. habitat_policy_tests.cpp -o "$$policy_test" && "$$policy_test"
include $(WEBOTS_HOME)/resources/Makefile.include
