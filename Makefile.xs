# Temporary makefile for building in the XenServer build system; this 
# will be removed once we've properly integrated mock.

include $(B_BASE)/common.mk
include $(B_BASE)/rpmbuild.mk

REPO                   = $(call gitloc,stunnel)

.PHONY: $(MY_OUTPUT_DIR)/SRPMS
$(MY_OUTPUT_DIR)/SRPMS:
	mkdir -p $(MY_OUTPUT_DIR)/SRPMS
	$(MAKE) RPM_SOURCESDIR=$(RPM_SOURCESDIR) RPM_SRPMSDIR=$(RPM_SRPMSDIR) -C $(REPO) -f Makefile.xcp srpm

build: $(MY_SOURCES)/MANIFEST $(MY_OUTPUT_DIR)/SRPMS
	mkdir -p $(MY_OUTPUT_DIR)/RPMS/$(DOMAIN0_ARCH_OPTIMIZED)
	$(RPMBUILD) --rebuild --target $(DOMAIN0_ARCH_OPTIMIZED) $(MY_OUTPUT_DIR)/SRPMS/stunnel-*.src.rpm
	mkdir -p $(MY_MAIN_PACKAGES)
	# Deliberately omit the debuginfo package (stunnel-debuginfo-4...)
	cp $(MY_OUTPUT_DIR)/RPMS/$(DOMAIN0_ARCH_OPTIMIZED)/stunnel-4*.rpm $(MY_MAIN_PACKAGES)

$(MY_SOURCES)/MANIFEST: $(MY_OUTPUT_DIR)/SRPMS
	rm -f $@
	mkdir -p $(MY_SOURCES)
	/bin/sh ./srpms-to-manifest stunnel $(MY_OUTPUT_DIR)/SRPMS > $@

.PHONY: clean
clean:
	rm -f $(MY_SOURCES)/MANIFEST $(MY_OUTPUT_DIR)/SRPMS/*
	rmdir $(MY_OUTPUT_DIR)/SRPMS

