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
TARGETS = memory_stride_access nmp_device_test nmp_ptr_chase_test device_cxl_test \
          host_offload device_offload

all: $(TARGETS)

# Build with m5ops (always linked with libm5.a)
memory_stride_access: memory_stride_access.c
	@if [ ! -f "$(M5_LIB)" ]; then \
		echo "Building m5ops library..."; \
		cd $(M5_PATH) && scons build/x86/out/libm5.a; \
	fi
	$(CC) $(CFLAGS) $(M5_INCLUDE) -o $@ $< $(M5_LIB)
	@echo "Built: $@ (with m5ops linked)"

nmp_device_test: nmp_device_test.c
	@if [ ! -f "$(M5_LIB)" ]; then \
		echo "Building m5ops library..."; \
		cd $(M5_PATH) && scons build/x86/out/libm5.a; \
	fi
	$(CC) $(CFLAGS) $(M5_INCLUDE) -o $@ $< $(M5_LIB)
	@echo "Built: $@ (with m5ops linked)"

nmp_ptr_chase_test: nmp_ptr_chase_test.c
	@if [ ! -f "$(M5_LIB)" ]; then \
		echo "Building m5ops library..."; \
		cd $(M5_PATH) && scons build/x86/out/libm5.a; \
	fi
	$(CC) $(CFLAGS) $(M5_INCLUDE) -o $@ $< $(M5_LIB)
	@echo "Built: $@ (with m5ops linked)"

# Device-side userspace test: mmap /dev/mem over the CXL range
# and round-trip a 64-bit pattern to validate cxl_mem_bus access.
device_cxl_test: device_cxl_test.c
	$(CC) $(CFLAGS) -o $@ $<
	@echo "Built: $@"

# Host->device CXL offload MVP: shared-memory mailbox + doorbell.
# Both link no m5ops; both depend on the shared mailbox contract header.
host_offload: host_offload.c cxl_mailbox.h
	$(CC) $(CFLAGS) -o $@ $<
	@echo "Built: $@"

device_offload: device_offload.c cxl_mailbox.h
	$(CC) $(CFLAGS) -o $@ $<
	@echo "Built: $@"

# Clean
clean:
	rm -f $(TARGETS) *.o

# Install to disk image (requires mount)
install: $(TARGETS)
	@if [ -d "/tmp/parsec_mount/home/cxl_benchmark" ]; then \
		echo "Installing to disk image..."; \
		sudo cp memory_stride_access /tmp/parsec_mount/home/cxl_benchmark/memory_stride_access; \
		sudo chmod +x /tmp/parsec_mount/home/cxl_benchmark/memory_stride_access; \
		echo "Installed: /home/cxl_benchmark/memory_stride_access"; \
		sudo cp nmp_device_test /tmp/parsec_mount/home/cxl_benchmark/nmp_device_test; \
		sudo chmod +x /tmp/parsec_mount/home/cxl_benchmark/nmp_device_test; \
		echo "Installed: /home/cxl_benchmark/nmp_device_test"; \
		sudo cp nmp_ptr_chase_test /tmp/parsec_mount/home/cxl_benchmark/nmp_ptr_chase_test; \
		sudo chmod +x /tmp/parsec_mount/home/cxl_benchmark/nmp_ptr_chase_test; \
		echo "Installed: /home/cxl_benchmark/nmp_ptr_chase_test"; \
		sudo cp host_offload /tmp/parsec_mount/home/cxl_benchmark/host_offload; \
		sudo chmod +x /tmp/parsec_mount/home/cxl_benchmark/host_offload; \
		echo "Installed: /home/cxl_benchmark/host_offload"; \
		sudo cp device_offload /tmp/parsec_mount/home/cxl_benchmark/device_offload; \
		sudo chmod +x /tmp/parsec_mount/home/cxl_benchmark/device_offload; \
		echo "Installed: /home/cxl_benchmark/device_offload"; \
	else \
		echo "ERROR: Disk image not mounted at /tmp/parsec_mount"; \
		echo "Run: sudo mount -o loop,offset=\$$((2048*512)) fs_files/parsec.img /tmp/parsec_mount"; \
	fi

.PHONY: all clean install
