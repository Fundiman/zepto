CXX       = g++
CXXFLAGS  = -std=c++20 -O2
SRC       = zepto.cpp
CAPI_SRC  = zepto_capi.cpp

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
  RM         = rm -f
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
  RM         = del /f /q
  PYTHON     = python
  EXE_EXT    = .exe
endif

.PHONY: all lib repl python clean

all: lib repl

lib: zepto.o

repl: zepto$(EXE_EXT)

python: zepto$(DLL_EXT)

zepto$(EXE_EXT): $(SRC) zepto.h
	$(CXX) $(CXXFLAGS) -DZEPTO_REPL $(SRC) -o $@

zepto.o: $(SRC) zepto.h
	$(CXX) $(CXXFLAGS) -c $(SRC) -o $@

zepto$(DLL_EXT): $(SRC) $(CAPI_SRC) zepto_capi.h zepto.h
	$(CXX) $(CXXFLAGS) $(DLL_LINK) -o $@ $(SRC) $(CAPI_SRC)

clean:
	$(RM) zepto.o zepto$(EXE_EXT) zepto$(DLL_EXT)
