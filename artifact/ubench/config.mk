# Default to 64 bits, but allow overriding on command line
BITS     ?= 64

# Give name to output folder, and ensure it is created before any compilation
ODIR     := ./obj$(BITS)
__odir   := $(shell mkdir -p $(ODIR))

# Basic tool configuration for gcc/g++
CXX       = clang++-15
LD        = g++
CXXFLAGS += -MMD -O3 -m$(BITS) -g -std=c++20 -g -fPIC -march=native -mrtm -Wall -Werror
LDFLAGS  += -m$(BITS) -lpthread
