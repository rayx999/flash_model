# External 
SYSTEMC_HOME ?= 
MAGICNUM_HOME ?= 

# Compiler and flags
CXX      := /usr/bin/g++
CXXFLAGS := -std=c++20 -g -Wall -Wextra -DSC_ALLOW_DEPRECATED_IEEE_API

# Include directories
INCLUDES := -I$(SYSTEMC_HOME)/src \
            -I$(MAGICNUM_HOME)/include/magic_enum \
            -I.

# Linker flags and libraries
LDFLAGS  := -L$(SYSTEMC_HOME)/objdir/src
LIBS     := -lsystemc -lpthread

# Target output executable
TARGET   := ./build/test_suite

# Source files
SRCS     := $(wildcard *.cpp)

# Build rules
all: $(TARGET)

$(TARGET): $(SRCS)
	@mkdir -p $(dir $(TARGET))
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(SRCS) $(LDFLAGS) $(LIBS) -o $(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all clean


