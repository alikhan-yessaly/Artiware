MP_PATH ?= ../micropython
BOARD ?= PiBody
MP_PORT := $(MP_PATH)/ports/rp2
BOARD_DIR := $(PWD)/boards/$(BOARD)
BUILD_DIR := $(PWD)/build/$(BOARD)

# Optional per-board settings: FW_VERSION (banner / os.uname), MP_TAG (mp-prepare).
-include $(BOARD_DIR)/board.mk
ifdef FW_VERSION
export MICROPY_GIT_TAG := $(FW_VERSION)
endif
# cmake >= 4 rejects the old cmake_minimum_required() in some pico-sdk dependencies.
export CMAKE_ARGS += -DCMAKE_POLICY_VERSION_MINIMUM=3.5

all:
	$(MAKE) -C $(MP_PORT) \
		BOARD_DIR=$(BOARD_DIR) \
		BUILD=$(BUILD_DIR)

# One-time MicroPython setup for boards with patches (PiBody BLE REPL):
# new branch at MP_TAG with patches/micropython-$(MP_TAG) applied, then mpy-cross
# and submodules. Refuses to touch an existing branch.
MP_BRANCH ?= artiware-$(MP_TAG)
mp-prepare:
	git -C $(MP_PATH) fetch --tags
	git -C $(MP_PATH) checkout -b $(MP_BRANCH) $(MP_TAG)
	git -C $(MP_PATH) am $(PWD)/patches/micropython-$(MP_TAG)/*.patch
	$(MAKE) -C $(MP_PATH)/mpy-cross
	$(MAKE) -C $(MP_PORT) BOARD_DIR=$(BOARD_DIR) submodules

clean: 
	$(RM) -rf $(BUILD_DIR)

deploy: all
	sudo picotool load -x $(BUILD_DIR)/firmware.uf2

