KERNEL_DATA = Image
INITRD_DATA = rootfs.cpio
TEST_TOOLS_DATA = test-tools.img

PREBUILT_URL ?= https://github.com/sysprog21/semu/releases/download/prebuilt

# Local Make defaults to consuming the rolling release for missing guest
# artifacts. Source builds stay explicit through `make build-image`; CI owns
# recipe-key/cache decisions separately under .ci/prebuilt.
define download_prebuilt_artifact
$(1):
	$$(Q)printf '  GET\t$$@\n'
	$$(Q)curl --fail --retry 3 --retry-delay 1 --progress-bar \
	    -L -o "$$@.bz2.part" "$$(PREBUILT_URL)/$$@.bz2" \
	    || { rm -f "$$@.bz2.part"; exit 1; }
	$$(Q)mv "$$@.bz2.part" "$$@.bz2"
	$$(Q)bunzip2 -c "$$@.bz2" > "$$@.tmp" \
	    || { rm -f "$$@.tmp"; exit 1; }
	$$(Q)mv "$$@.tmp" "$$@"
	$$(Q)rm -f "$$@.bz2"
endef

$(foreach artifact,$(KERNEL_DATA) $(INITRD_DATA) $(TEST_TOOLS_DATA),$(eval $(call download_prebuilt_artifact,$(artifact))))
