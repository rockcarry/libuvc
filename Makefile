CROSS_COMPILE ?=
GCC ?= $(CROSS_COMPILE)gcc

C_FLAGS := -Wall -O2
C_FLAGS += -fPIC -DPIC

C_INCLUDES := \
    -I$(shell pwd)/include

PROGS = libuvc.a

OMX_COMP_C_SRCS=$(wildcard ./*.c)
OMX_COMP_C_SRCS_NO_DIR=$(notdir $(OMX_COMP_C_SRCS))
OBJECTS=$(patsubst %.c, %.c.o, $(OMX_COMP_C_SRCS_NO_DIR))

OBJDIR ?= $(shell pwd)/obj
LIBDIR ?= $(shell pwd)/lib
INCDIR ?= $(shell pwd)/inc

OBJPROG = $(addprefix $(OBJDIR)/, $(PROGS))

.PHONY: clean prepare PROGS

all: prepare $(OBJPROG)

prepare:

clean:
	@rm -Rf $(OBJDIR)
	@rm -Rf $(LIBDIR)

$(OBJPROG): $(addprefix $(OBJDIR)/, $(OBJECTS))
	@mkdir -p $(LIBDIR)
	@echo "  LIBDIR $@"
	@$(AR) rcs $@ $(OBJDIR)/*.o
	@echo ""
	mv -f $(OBJDIR)/$(PROGS) $(LIBDIR)
	
$(OBJDIR)/%.c.o : %.c
	@mkdir -p obj
	@echo "  CC  $<"
	@$(GCC) $(C_FLAGS) $(C_INCLUDES) -c $< -o $@

