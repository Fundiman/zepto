CXX       = g++
CXXFLAGS  = -std=c++20 -O2
SRC       = zepto.cpp
CAPI_SRC  = zepto_capi.cpp

TEST_SRCS = quick_test corrupt_test rs_test debug_test
TOOL_SRCS = debug_raw debug_rows
ALL_EXES  = zepto $(TEST_SRCS) $(TOOL_SRCS)

# ---- Platform detection via compiler (works across all OS / process bitness) ----
CXX_MACHINE := $(shell $(CXX) -dumpmachine 2>nul)
ifneq (,$(findstring x86_64,$(CXX_MACHINE)))
  CXXFLAGS += -msse4.2
endif

# ---- Platform-specific settings ----
UNAME_S := $(shell uname -s 2>nul)

ifneq ($(UNAME_S),)
  # Linux / macOS / FreeBSD
  DLL_LINK   = -shared -fPIC
  DELFILE    = rm -f
  RMDIR      = rm -rf
  PYTHON     = python3
  EXE_EXT    =
  ifeq ($(UNAME_S),Darwin)
    DLL_EXT  = .dylib
  else
    DLL_EXT  = .so
  endif
else
  # Windows (MinGW / MSYS / Cygwin)
  DLL_EXT    = .dll
  DLL_LINK   = -shared -static
  DELFILE    = del /f /q
  RMDIR      = cmd /c rd /s /q
  PYTHON     = python
  EXE_EXT    = .exe
endif

.PHONY: all lib repl python test tools clean

all: lib repl test tools

lib: zepto.o

repl: zepto$(EXE_EXT)

python: zepto$(DLL_EXT)

test: $(addsuffix $(EXE_EXT),$(TEST_SRCS))
	@echo "--- Running all tests ---"
	@$(word 1,$(TEST_SRCS))$(EXE_EXT)
	@$(word 2,$(TEST_SRCS))$(EXE_EXT)
	@$(word 3,$(TEST_SRCS))$(EXE_EXT)
	@$(word 4,$(TEST_SRCS))$(EXE_EXT)
	@echo "  All tests passed."

tools: $(addsuffix $(EXE_EXT),$(TOOL_SRCS))

zepto$(EXE_EXT): $(SRC) zepto.h
	$(CXX) $(CXXFLAGS) -DZEPTO_REPL $(SRC) -o $@

zepto.o: $(SRC) zepto.h
	$(CXX) $(CXXFLAGS) -c $(SRC) -o $@

zepto$(DLL_EXT): $(SRC) $(CAPI_SRC) zepto_capi.h zepto.h
	$(CXX) $(CXXFLAGS) $(DLL_LINK) -o $@ $(SRC) $(CAPI_SRC)

# ---- Test executables ----
quick_test$(EXE_EXT): quick_test.cpp $(SRC) zepto.h
	$(CXX) $(CXXFLAGS) -o $@ quick_test.cpp $(SRC)

corrupt_test$(EXE_EXT): corrupt_test.cpp $(SRC) zepto.h
	$(CXX) $(CXXFLAGS) -o $@ corrupt_test.cpp $(SRC)

rs_test$(EXE_EXT): rs_test.cpp $(SRC) zepto.h
	$(CXX) $(CXXFLAGS) -o $@ rs_test.cpp $(SRC)

debug_test$(EXE_EXT): debug_test.cpp $(SRC) zepto.h
	$(CXX) $(CXXFLAGS) -o $@ debug_test.cpp $(SRC)

# ---- Tool executables ----
debug_raw$(EXE_EXT): debug_raw.cpp $(SRC) zepto.h
	$(CXX) $(CXXFLAGS) -o $@ debug_raw.cpp $(SRC)

debug_rows$(EXE_EXT): debug_rows.cpp $(SRC) zepto.h
	$(CXX) $(CXXFLAGS) -o $@ debug_rows.cpp $(SRC)

clean:
	-$(DELFILE) *.o *.exe *.dll *.dylib *.so
	-$(DELFILE) *.zepto
	-$(RMDIR) test_features_dir
	-$(RMDIR) test_features2
	-$(RMDIR) test_features3
	-$(RMDIR) test_sql_features
	-$(RMDIR) build
	-$(RMDIR) zepto\__pycache__
