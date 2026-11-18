.DEFAULT_GOAL := lib

SPDK_ROOT_DIR ?= /home/gyq2/spdk

C_SRCS := zvfs.c shim.c inode.c clear_all_inode.c
LIB_HEADERS := zvfs.h shim.h inode.h

include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

LIB_OBJS := $(filter-out clear_all_inode.o,$(OBJS))

CFLAGS += $(ENV_CFLAGS)
DPDK_LIB_LIST += rte_hash rte_rcu

SPDK_LIB_LIST = blob blob_bdev bdev_nvme event event_bdev

include $(SPDK_ROOT_DIR)/mk/spdk.app_vars.mk

LIBS += $(SPDK_LIB_LINKER_ARGS)
SYS_LIBS += -ldl

LDFLAGS += -Wl,-rpath,$(SPDK_ROOT_DIR)/build/lib
LDFLAGS += -Wl,-rpath,$(DPDK_LIB_DIR)

.PHONY: lib tools clean

lib: libstore.so

tools: clear_all_inode

libstore.so: $(LIB_OBJS) $(LIB_HEADERS) $(SPDK_LIB_FILES) $(ENV_LIBS) Makefile
	$(CC) -shared -Wl,-z,defs -o $@ $(CFLAGS) $(LDFLAGS) \
		$(LIB_OBJS) $(LIBS) $(ENV_LDFLAGS) $(SYS_LIBS)

clear_all_inode: clear_all_inode.o $(SPDK_LIB_FILES) $(ENV_LIBS) Makefile
	$(CC) -o $@ $(CFLAGS) $(LDFLAGS) \
		clear_all_inode.o $(LIBS) $(ENV_LDFLAGS) $(SYS_LIBS)

$(OBJS): $(LIB_HEADERS)

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) $(OBJS:.o=.d.tmp) libstore.so clear_all_inode

include $(SPDK_ROOT_DIR)/mk/spdk.deps.mk
ifeq ($(TSAN),1)
CFLAGS += -O1 -g -fsanitize=thread
LDFLAGS += -fsanitize=thread
endif
