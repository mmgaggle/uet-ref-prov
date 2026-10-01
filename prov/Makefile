# libfabric DL provider "uet" (libuet-fi.so) over the UEC reference provider.
#
# The core (SES/PDS/TSS/NIC shim) is compiled here against the vendored
# libfabric 1.20 headers, exactly as for libuet_fabric.so; the glue is
# compiled against the installed libfabric headers. Both go into one
# library that exports only fi_prov_ini and links the installed
# libfabric. No LIBFABRIC build tree is needed.
#
#   make                 libuet-fi.so, test_rma, prototype check
#   FI_PROVIDER_PATH=$PWD fi_info -p uet

TOP := ..
CC ?= gcc
OPT ?= -O2 -g
LIBFABRIC_LIBS ?= -lfabric

LIB := libuet-fi.so
TEST := test_rma

CORE_SRC := $(filter-out $(TOP)/uet.c $(wildcard $(TOP)/nic_shim/*xdp*), \
	$(wildcard $(TOP)/*.c $(TOP)/util/*.c $(TOP)/imp_shim/*.c \
		   $(TOP)/nic_shim/*.c $(TOP)/crypto/*.c))
CORE_OBJ := $(patsubst $(TOP)/%.c,obj/core/%.o,$(CORE_SRC))
CORE_HDRS := $(wildcard $(TOP)/*.h $(TOP)/util/*.h $(TOP)/imp_shim/*.h \
		       $(TOP)/nic_shim/*.h $(TOP)/crypto/*.h)
CORE_INCS := -I$(TOP) -I$(TOP)/util -I$(TOP)/imp_shim -I$(TOP)/nic_shim \
	     -I$(TOP)/crypto -I$(TOP)/libfabric_headers \
	     -I$(TOP)/libfabric_headers/include
# same diagnostics as the top-level build of the core
CORE_CFLAGS := $(OPT) -fPIC -fvisibility=hidden -DENABLE_VERBS=0 -Wall \
	       -Wno-unused-variable -Wno-implicit-function-declaration \
	       -Wno-int-conversion -Wno-address-of-packed-member

GLUE_SRC := uet_fi_init.c uet_fi_domain.c uet_fi_ep.c uet_fi_cq.c
GLUE_OBJ := $(GLUE_SRC:%.c=obj/%.o)
GLUE_HDRS := uet_fi.h uet_core.h $(TOP)/uet_addr.h
GLUE_CFLAGS := $(OPT) -fPIC -fvisibility=hidden -Wall -Wextra

all: $(LIB) $(TEST) obj/core_check.stamp

obj/core/%.o: $(TOP)/%.c $(CORE_HDRS)
	@mkdir -p $(dir $@)
	@echo '  CC (core) $<'
	@$(CC) $(CORE_CFLAGS) $(CORE_INCS) -c -o $@ $<

obj/%.o: %.c $(GLUE_HDRS)
	@mkdir -p $(dir $@)
	@echo '  CC        $<'
	@$(CC) $(GLUE_CFLAGS) -c -o $@ $<

# uet_core.h must agree with uet_api.h
obj/core_check.stamp: core_check.c uet_core.h $(CORE_HDRS)
	@mkdir -p $(dir $@)
	@echo '  CHECK     uet_core.h against uet_api.h'
	@$(CC) $(CORE_CFLAGS) $(CORE_INCS) -I. -fsyntax-only $<
	@touch $@

$(LIB): $(GLUE_OBJ) $(CORE_OBJ) libuet-fi.map
	@echo '  LD        $@'
	@$(CC) -shared -o $@ $(GLUE_OBJ) $(CORE_OBJ) \
		-Wl,--version-script=libuet-fi.map -Wl,-z,defs \
		$(LIBFABRIC_LIBS) -lpthread

$(TEST): test_rma.c
	@echo '  CC        $@'
	@$(CC) $(OPT) -Wall -Wextra -o $@ $< $(LIBFABRIC_LIBS)

clean:
	@rm -rf obj $(LIB) $(TEST)

.PHONY: all clean
