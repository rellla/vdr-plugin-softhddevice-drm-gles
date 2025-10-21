# Unit Tests for softhddevice-drm-gles

This directory contains unit tests for the softhddevice-drm-gles VDR plugin using the Catch2 testing framework.

## Prerequisites

You need to have Catch2 v3 (since Ubuntu 24.04/Debian 13) and lcov installed on your system. On Debian/Ubuntu systems:

```bash
sudo apt-get install catch2 lcov
```

## Building and Running Tests

### Run tests

```bash
make test
```

### Run specific tests

```bash
./test_runner "[pes]"          # Run all PES tests
./test_runner "cPes - MPEG2"   # Run MPEG2-related tests
```

### List all available tests

```bash
./test_runner --list-tests
```

## Code Coverage

### Generate coverage report

```bash
make coverage
```

This will:
1. Build the tests with coverage instrumentation
2. Run all tests
3. Generate a coverage report showing which lines of code were executed

### Generate HTML coverage report

```bash
make coverage-html
```

This generates an interactive HTML report in `coverage-html/index.html` that you can open in a browser to see detailed coverage information with color-coded source files.
