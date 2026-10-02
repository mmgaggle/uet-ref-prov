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

# The core the same glue runs on:
#
#   CORE=ref    the reference provider's SES/PDS/TSS and its raw socket
#               NIC shim, compiled in (the default): libuet-fi.so
#   CORE=ernic  libuet_ernic, which drives the UET engine inside a
#               rocm-ernic device over libibverbs, compiled in from the
#               rocm-ernic checkout at ERNIC: ernic/libuet-fi.so
#
# "make ernic" is CORE=ernic. Both libraries are providers named "uet";
# FI_PROVIDER_PATH picks one. test_rma uses only libfabric, so it serves
# both.
CORE ?= ref
ERNIC ?= ../../rocm-ernic

TEST := test_rma

CORE_HDRS := $(wildcard $(TOP)/*.h $(TOP)/util/*.h $(TOP)/imp_shim/*.h \
		       $(TOP)/nic_shim/*.h $(TOP)/crypto/*.h)
CORE_INCS := -I$(TOP) -I$(TOP)/util -I$(TOP)/imp_shim -I$(TOP)/nic_shim \
	     -I$(TOP)/crypto -I$(TOP)/libfabric_headers \
	     -I$(TOP)/libfabric_headers/include
# same diagnostics as the top-level build of the core
CORE_CFLAGS := $(OPT) -fPIC -fvisibility=hidden -DENABLE_VERBS=0 -Wall \
	       -Wno-unused-variable -Wno-implicit-function-declaration \
	       -Wno-int-conversion -Wno-address-of-packed-member

ifeq ($(CORE),ernic)
LIB := ernic/libuet-fi.so
ERNIC_SRC := $(ERNIC)/guest/libuet_ernic/uet_ernic.c
CORE_OBJ := obj/ernic/uet_ernic.o
ERNIC_HDRS := $(ERNIC)/guest/libuet_ernic/uet_ernic.h \
	      $(ERNIC)/shared/uet_ernic_abi.h $(TOP)/uet_addr.h
# built like the glue, against the installed libfabric headers
ERNIC_INCS := -I$(ERNIC)/guest/libuet_ernic -I$(ERNIC)/shared -I$(TOP)
ERNIC_CFLAGS := $(OPT) -fPIC -fvisibility=hidden -std=c11 -D_GNU_SOURCE \
		-Wall -Wextra
CORE_LIBS := -libverbs
CORE_IF := ernic
CORE_CHECKS := obj/core_check.stamp obj/core_check_ernic.stamp
else ifeq ($(CORE),ref)
LIB := libuet-fi.so
CORE_SRC := $(filter-out $(TOP)/uet.c $(wildcard $(TOP)/nic_shim/*xdp*), \
	$(wildcard $(TOP)/*.c $(TOP)/util/*.c $(TOP)/imp_shim/*.c \
		   $(TOP)/nic_shim/*.c $(TOP)/crypto/*.c))
CORE_OBJ := $(patsubst $(TOP)/%.c,obj/core/%.o,$(CORE_SRC))
CORE_LIBS :=
CORE_IF := netdev
CORE_CHECKS := obj/core_check.stamp
else
$(error CORE must be ref or ernic)
endif

GLUE_SRC := uet_fi_init.c uet_fi_domain.c uet_fi_ep.c uet_fi_cq.c \
	    uet_fi_if_$(CORE_IF).c
GLUE_OBJ := $(GLUE_SRC:%.c=obj/%.o)
GLUE_HDRS := uet_fi.h uet_core.h $(TOP)/uet_addr.h $(TOP)/uet_payload.h
GLUE_CFLAGS := $(OPT) -fPIC -fvisibility=hidden -Wall -Wextra

all: $(LIB) $(TEST) $(CORE_CHECKS)

ernic:
	@$(MAKE) --no-print-directory CORE=ernic

# the one glue file that calls libuet_ernic beyond uet_api.h
obj/uet_fi_if_ernic.o: GLUE_CFLAGS += $(ERNIC_INCS)
obj/uet_fi_if_ernic.o: $(ERNIC_HDRS)

obj/ernic/uet_ernic.o: $(ERNIC_SRC) $(ERNIC_HDRS)
	@mkdir -p $(dir $@)
	@echo '  CC (ernic) $<'
	@$(CC) $(ERNIC_CFLAGS) $(ERNIC_INCS) -c -o $@ $<

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

# uet_core.h must agree with libuet_ernic's declarations too
obj/core_check_ernic.stamp: core_check_ernic.c uet_core.h $(ERNIC_HDRS)
	@mkdir -p $(dir $@)
	@echo '  CHECK     uet_core.h against uet_ernic.h'
	@$(CC) $(ERNIC_CFLAGS) $(ERNIC_INCS) -I. -Werror -fsyntax-only $<
	@touch $@

$(LIB): $(GLUE_OBJ) $(CORE_OBJ) libuet-fi.map
	@mkdir -p $(dir $@)
	@echo '  LD        $@'
	@$(CC) -shared -o $@ $(GLUE_OBJ) $(CORE_OBJ) \
		-Wl,--version-script=libuet-fi.map -Wl,-z,defs \
		$(LIBFABRIC_LIBS) $(CORE_LIBS) -lpthread

$(TEST): test_rma.c
	@echo '  CC        $@'
	@$(CC) $(OPT) -Wall -Wextra -o $@ $< $(LIBFABRIC_LIBS)

clean:
	@rm -rf obj ernic libuet-fi.so $(TEST)

.PHONY: all clean ernic
