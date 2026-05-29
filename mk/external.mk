PREBUILT_URL ?= https://github.com/sysprog21/semu/releases/download/prebuilt
PREBUILT_STRICT ?= 0
PREBUILT_IGNORE_SHA ?= 0
PREBUILT_VERBOSE ?= 0


KERNEL_DATA = Image
INITRD_DATA = rootfs.cpio
TEST_TOOLS_DATA = test-tools.img

# scripts/prebuilt is the provider-neutral artifact layer. Make exposes local
# build-system verbs here; CI adapters add cache/upload/publish behavior above it.
PREBUILT_SCRIPT_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))../scripts/prebuilt)
PREBUILT_INPUTS_SCRIPT := $(PREBUILT_SCRIPT_DIR)/artifact-inputs.sh
PREBUILT_RESOLVER := $(PREBUILT_SCRIPT_DIR)/resolve-artifacts.sh
PREBUILT_PLAN_MATERIALIZER := $(PREBUILT_SCRIPT_DIR)/plan-materialize.sh

.PHONY: prebuilt-classes prebuilt-inputs prebuilt-recipe-key prebuilt-plan
.PHONY: materialize-prebuilt-image materialize-prebuilt-rootfs materialize-prebuilt-test-tools
prebuilt-classes:
	$(Q)"$(PREBUILT_INPUTS_SCRIPT)" classes

prebuilt-inputs:
	$(Q)test -n "$(CLASS)" || { \
	    echo "CLASS is required, e.g. make prebuilt-inputs CLASS=image" >&2; \
	    exit 1; \
	}
	$(Q)"$(PREBUILT_INPUTS_SCRIPT)" inputs "$(CLASS)"

prebuilt-recipe-key:
	$(Q)test -n "$(CLASS)" || { \
	    echo "CLASS is required, e.g. make prebuilt-recipe-key CLASS=image" >&2; \
	    exit 1; \
	}
	$(Q)"$(PREBUILT_INPUTS_SCRIPT)" recipe-key "$(CLASS)"

prebuilt-plan:
	$(Q)PREBUILT_URL="$(PREBUILT_URL)" \
	    PREBUILT_STRICT="$(PREBUILT_STRICT)" \
	    PREBUILT_IGNORE_SHA="$(PREBUILT_IGNORE_SHA)" \
	    PREBUILT_LOCAL_FIRST=1 \
	    "$(PREBUILT_RESOLVER)"

materialize-prebuilt-image:
	$(Q)PREBUILT_URL="$(PREBUILT_URL)" \
	    PREBUILT_STRICT="$(PREBUILT_STRICT)" \
	    PREBUILT_IGNORE_SHA="$(PREBUILT_IGNORE_SHA)" \
	    PREBUILT_VERBOSE="$(PREBUILT_VERBOSE)" \
	    "$(PREBUILT_PLAN_MATERIALIZER)" image >/dev/null

materialize-prebuilt-rootfs:
	$(Q)PREBUILT_URL="$(PREBUILT_URL)" \
	    PREBUILT_STRICT="$(PREBUILT_STRICT)" \
	    PREBUILT_IGNORE_SHA="$(PREBUILT_IGNORE_SHA)" \
	    PREBUILT_VERBOSE="$(PREBUILT_VERBOSE)" \
	    "$(PREBUILT_PLAN_MATERIALIZER)" rootfs >/dev/null

materialize-prebuilt-test-tools:
	$(Q)PREBUILT_URL="$(PREBUILT_URL)" \
	    PREBUILT_STRICT="$(PREBUILT_STRICT)" \
	    PREBUILT_IGNORE_SHA="$(PREBUILT_IGNORE_SHA)" \
	    PREBUILT_VERBOSE="$(PREBUILT_VERBOSE)" \
	    "$(PREBUILT_PLAN_MATERIALIZER)" test-tools >/dev/null

$(KERNEL_DATA): materialize-prebuilt-image
	$(Q)test -f "$@" || { \
	    echo "prebuilt materializer did not produce $@" >&2; \
	    exit 1; \
	}

$(INITRD_DATA): materialize-prebuilt-rootfs
	$(Q)test -f "$@" || { \
	    echo "prebuilt materializer did not produce $@" >&2; \
	    exit 1; \
	}

$(TEST_TOOLS_DATA): materialize-prebuilt-test-tools
	$(Q)test -f "$@" || { \
	    echo "prebuilt materializer did not produce $@" >&2; \
	    exit 1; \
	}
