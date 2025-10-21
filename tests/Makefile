#
# Makefile for unit tests using Catch2 and coverage with lcov
#

# Compiler settings
CXX = g++
CXXFLAGS = -std=c++17 -g -Wall -Wextra -I.. -isystem ../../../../include
LDFLAGS =

# Coverage settings
COVERAGE_CXXFLAGS = --coverage -fprofile-arcs -ftest-coverage
COVERAGE_LDFLAGS = --coverage

# Add library dependencies
CXXFLAGS += $(shell pkg-config --cflags libavcodec libavutil catch2)
LIBS = $(shell pkg-config --libs libavcodec libavutil catch2-with-main)

# Source files
TEST_SOURCES = catch_main.cpp test_pes.cpp test_stubs.cpp
SOURCE_UNDER_TEST = ../pes.cpp ../logger.cpp

# Object files
TEST_OBJS = $(TEST_SOURCES:.cpp=.o)
SOURCE_OBJS = $(SOURCE_UNDER_TEST:.cpp=.o)

# Test executable
TEST_EXEC = test_runner

# Main target
all: $(TEST_EXEC)

# Link test executable
$(TEST_EXEC): $(TEST_OBJS) $(SOURCE_OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

# Compile test sources
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# Compile sources under test
../%.o: ../%.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# Run tests
.PHONY: test
test: $(TEST_EXEC)
	valgrind ./$(TEST_EXEC) -r console -r junit::out=test-results.xml

# Clean
.PHONY: clean
clean:
	@echo "Cleaning test build artifacts..."
	rm -f $(TEST_OBJS) $(SOURCE_OBJS) $(TEST_EXEC) test-results.xml
	@echo "Cleaning coverage data..."
	rm -f *.gcda *.gcno ../*.gcda ../*.gcno coverage.info
	rm -rf coverage-html

# Coverage targets
.PHONY: coverage
coverage: CXXFLAGS += $(COVERAGE_CXXFLAGS)
coverage: LDFLAGS += $(COVERAGE_LDFLAGS)
coverage: clean $(TEST_EXEC)
	@echo "Running tests with coverage..."
	./$(TEST_EXEC)
	@echo "Generating coverage report..."
	lcov --capture --directory . --directory .. --output-file coverage.info
	lcov --remove coverage.info '/usr/*' '*/tests/*' --output-file coverage.info
	lcov --list coverage.info

.PHONY: coverage-html
coverage-html: coverage
	@echo "Generating HTML coverage report..."
	genhtml coverage.info --output-directory coverage-html
	@echo "Coverage report generated in coverage-html/index.html"

.PHONY: help
help:
	@echo "Available targets:"
	@echo "  all              - Build test executable (default)"
	@echo "  test             - Build and run tests with console output"
	@echo "  coverage         - Build with coverage, run tests, and generate coverage report"
	@echo "  coverage-html    - Generate HTML coverage report (includes coverage)"
	@echo "  clean            - Remove all build artifacts, test results, and coverage data"
	@echo "  help             - Show this help message"
