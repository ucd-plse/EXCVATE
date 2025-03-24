PIN_DIR=$(wildcard ../../submodules/pin-*)
TOOLS := $(basename $(wildcard *.cpp))

all: $(TOOLS)

$(TOOLS):
	$(MAKE) obj-intel64/$@.so -C $(PIN_DIR)/source/tools/EXCVATE

.PHONY: all $(TOOLS)