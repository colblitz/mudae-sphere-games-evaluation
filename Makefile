# Makefile for mudae-sphere-games-evaluation
#
# Targets:
#   make build-harness         Build all four evaluator binaries
#   make build-oh              Build only the oh evaluator
#   make build-oc              Build only the oc evaluator
#   make build-oq              Build only the oq evaluator
#   make build-ot              Build only the ot evaluator
#   make generate-boards       Build + run the board generator
#   make clean                 Remove compiled binaries and .so files
#
# Evaluate a strategy (run via evaluate.py — see scripts/evaluate.py):
#   python scripts/evaluate.py --game oc --strategy strategies/oc/global_state.py
#   python scripts/evaluate.py --game oc --strategy strategies/oc/global_state.py --yes

# ---------------------------------------------------------------------------
# Compiler settings
# ---------------------------------------------------------------------------

CXX      ?= g++
CXXFLAGS  = -O3 -march=native -std=c++17 -Wall -Wextra
LDFLAGS   =
LIBS      = -llzma -ldl

# Repository root (injected into binaries as REPO_ROOT so they can locate
# boards/ relative to themselves regardless of working directory)
REPO_ROOT := $(shell pwd)

# ---------------------------------------------------------------------------
# Python include/lib (for the pybind11 / Python C API bridge)
# ---------------------------------------------------------------------------

PYTHON        ?= python3
PY_INCLUDES   := $(shell $(PYTHON) -c "import sysconfig; print(sysconfig.get_path('include'))")
PY_LIBDIR     := $(shell $(PYTHON) -c "import sysconfig; print(sysconfig.get_config_var('LIBDIR'))")
PY_LDLIB      := $(shell $(PYTHON) -c "import sysconfig; print(sysconfig.get_config_var('LDLIBRARY'))" | sed 's/lib//' | sed 's/\.so.*//' | sed 's/\.dylib.*//')
PY_CFLAGS     = -I$(PY_INCLUDES)
PY_LDFLAGS    = -L$(PY_LIBDIR) $(PY_LDLIB:%=-l%) -Wl,-rpath,$(PY_LIBDIR)

# ---------------------------------------------------------------------------
# OpenMP (optional; used by evaluate_ot for parallel board evaluation)
# ---------------------------------------------------------------------------

OPENMP_FLAG := $(shell $(CXX) -fopenmp -x c++ /dev/null -fsyntax-only 2>/dev/null && echo "-fopenmp")

# ---------------------------------------------------------------------------
# Common include path
# ---------------------------------------------------------------------------

INCLUDES = -Iharness/common -I. -DREPO_ROOT=\"$(REPO_ROOT)\"

# ---------------------------------------------------------------------------
# Binaries
# ---------------------------------------------------------------------------

BINARIES = harness/evaluate_oh harness/evaluate_oc harness/evaluate_oq harness/evaluate_ot harness/evaluate_ot_treewalk

.PHONY: all build-harness build-oh build-oc build-oq build-ot build-ot-treewalk generate-boards clean

all: build-harness

build-harness: build-oh build-oc build-oq build-ot build-ot-treewalk

build-oh: harness/evaluate_oh
build-oc: harness/evaluate_oc
build-oq: harness/evaluate_oq
build-ot: harness/evaluate_ot

harness/evaluate_oh: harness/evaluate_oh.cpp harness/common/*.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(PY_CFLAGS) $(OPENMP_FLAG) \
	    -o $@ $< \
	    $(LDFLAGS) $(LIBS) $(PY_LDFLAGS) $(OPENMP_FLAG)
	@echo "Built $@"

harness/evaluate_oc: harness/evaluate_oc.cpp harness/common/*.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(PY_CFLAGS) $(OPENMP_FLAG) \
	    -o $@ $< \
	    $(LDFLAGS) $(LIBS) $(PY_LDFLAGS) $(OPENMP_FLAG)
	@echo "Built $@"

harness/evaluate_oq: harness/evaluate_oq.cpp harness/common/*.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(PY_CFLAGS) $(OPENMP_FLAG) \
	    -o $@ $< \
	    $(LDFLAGS) $(LIBS) $(PY_LDFLAGS) $(OPENMP_FLAG)
	@echo "Built $@"

harness/evaluate_ot: harness/evaluate_ot.cpp harness/common/*.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(PY_CFLAGS) $(OPENMP_FLAG) \
	    -o $@ $< \
	    $(LDFLAGS) $(LIBS) $(PY_LDFLAGS) $(OPENMP_FLAG)
	@echo "Built $@"

build-ot-treewalk: harness/evaluate_ot_treewalk

harness/evaluate_ot_treewalk: harness/evaluate_ot_treewalk.cpp harness/common/*.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(PY_CFLAGS) $(OPENMP_FLAG) \
	    -o $@ $< \
	    $(LDFLAGS) $(LIBS) $(PY_LDFLAGS) $(OPENMP_FLAG)
	@echo "Built $@"

# ---------------------------------------------------------------------------
# Board generation
# ---------------------------------------------------------------------------

scripts/generate_boards: scripts/generate_boards.cpp
	$(CXX) $(CXXFLAGS) -DREPO_ROOT=\"$(REPO_ROOT)\" \
	    -o $@ $< \
	    -llzma
	@echo "Built $@"

generate-boards: scripts/generate_boards
	scripts/generate_boards
	@echo "Board files written to boards/"

# ---------------------------------------------------------------------------
# Clean
# ---------------------------------------------------------------------------

clean:
	rm -f $(BINARIES) scripts/generate_boards strategies/**/*.so
	@echo "Cleaned build artifacts"
