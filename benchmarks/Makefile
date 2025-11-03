# Makefile for NMP Memory Benchmarks

# Compiler settings
CC = gcc
CFLAGS = -O2 -static -Wall
M5_PATH = ../util/m5
GEM5_INCLUDE = ../include

# gem5 m5ops library and header paths
M5_INCLUDE = -I$(GEM5_INCLUDE)
M5_LIB = $(M5_PATH)/build/x86/out/libm5.a

# Targets
TARGETS = memory_stride_access

all: $(TARGETS)

# Build with m5ops (always linked with libm5.a)
memory_stride_access: memory_stride_access.c
	@if [ ! -f "$(M5_LIB)" ]; then \
		echo "Building m5ops library..."; \
		cd $(M5_PATH) && scons build/x86/out/libm5.a; \
	fi
	$(CC) $(CFLAGS) $(M5_INCLUDE) -o $@ $< $(M5_LIB)
	@echo "Built: $@ (with m5ops linked)"

# Clean
clean:
	rm -f $(TARGETS) *.o

# Install to disk image (requires mount)
install: memory_stride_access
	@if [ -d "/tmp/parsec_mount/home/cxl_benchmark" ]; then \
		echo "Installing to disk image..."; \
		sudo cp memory_stride_access /tmp/parsec_mount/home/cxl_benchmark/memory_stride_access; \
		sudo chmod +x /tmp/parsec_mount/home/cxl_benchmark/memory_stride_access; \
		echo "Installed: /home/cxl_benchmark/memory_stride_access"; \
	else \
		echo "ERROR: Disk image not mounted at /tmp/parsec_mount"; \
		echo "Run: sudo mount -o loop,offset=\$$((2048*512)) fs_files/parsec.img /tmp/parsec_mount"; \
	fi

.PHONY: all clean install
