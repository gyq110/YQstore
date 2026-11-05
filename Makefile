.DEFAULT_GOAL := lib

SPDK_ROOT_DIR ?= /home/gyq2/spdk

# 收集当前目录的业务源码，排除临时测试程序
C_SRCS := zvfs.c shim.c
LIB_HEADERS := zvfs.h shim.h

include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

# 业务代码直接使用了 DPDK 哈希接口，需要这些编译和链接参数
CFLAGS += $(ENV_CFLAGS)
DPDK_LIB_LIST += rte_hash rte_rcu

# 本项目使用的 SPDK 库
SPDK_LIB_LIST = blob blob_bdev bdev_malloc event event_bdev

include $(SPDK_ROOT_DIR)/mk/spdk.app_vars.mk

LIBS += $(SPDK_LIB_LINKER_ARGS)
SYS_LIBS += -ldl

LDFLAGS += -Wl,-rpath,$(SPDK_ROOT_DIR)/build/lib
LDFLAGS += -Wl,-rpath,$(DPDK_LIB_DIR)

.PHONY: lib clean

lib: libstore.so

libstore.so: $(OBJS) $(LIB_HEADERS) $(SPDK_LIB_FILES) $(ENV_LIBS) Makefile
	$(CC) -shared -Wl,-z,defs -o $@ $(CFLAGS) $(LDFLAGS) \
		$(OBJS) $(LIBS) $(ENV_LDFLAGS) $(SYS_LIBS)

$(OBJS): $(LIB_HEADERS)

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) $(OBJS:.o=.d.tmp) libstore.so

include $(SPDK_ROOT_DIR)/mk/spdk.deps.mk