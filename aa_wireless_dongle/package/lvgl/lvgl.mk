################################################################################
#
# LVGL, vendored as a Buildroot package: upstream Buildroot has no lvgl package
# and we do not want Qt on the image just to draw a dashboard.
#
# We compile the subset of sources listed in lvgl-sources.sh with our own
# lv_conf.h and install a static library plus headers. No display driver is
# built: aa-srcd renders offscreen and hands the pixels to the encoder.
#
################################################################################

LVGL_VERSION = v9.4.0
LVGL_SITE = $(call github,lvgl,lvgl,$(LVGL_VERSION))
LVGL_LICENSE = MIT
LVGL_LICENSE_FILES = LICENCE.txt
LVGL_INSTALL_STAGING = YES

# Headers land in .../include/lvgl, our lv_conf.h in .../include/lvgl-conf so
# that -DLV_CONF_INCLUDE_SIMPLE picks it up from the include path.
LVGL_STAGING_INC = $(STAGING_DIR)/usr/include/lvgl
LVGL_CONF_INC = $(STAGING_DIR)/usr/include/lvgl-conf

define LVGL_BUILD_CMDS
	mkdir -p $(@D)/build
	cd $(@D) && for source_file in $$(sh $(LVGL_PKGDIR)/lvgl-sources.sh $(@D)); do \
		object_file="$(@D)/build/$$(echo "$$source_file" | tr '/' '_').o"; \
		$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_CPPFLAGS) \
			-DLV_CONF_INCLUDE_SIMPLE \
			-I$(@D) -I$(@D)/src -I$(LVGL_PKGDIR) \
			-w -O2 -std=gnu11 \
			-c "$$source_file" -o "$$object_file" || exit 1; \
	done
	$(TARGET_AR) rcs $(@D)/build/liblvgl.a $(@D)/build/*.o
endef

define LVGL_INSTALL_STAGING_CMDS
	mkdir -p $(LVGL_STAGING_INC) $(LVGL_CONF_INC) $(STAGING_DIR)/usr/lib
	cp -a $(@D)/src $(LVGL_STAGING_INC)/
	cp -a $(@D)/lvgl.h $(@D)/lv_version.h $(LVGL_STAGING_INC)/
	cp -a $(LVGL_PKGDIR)/lv_conf.h $(LVGL_CONF_INC)/
	cp -a $(@D)/build/liblvgl.a $(STAGING_DIR)/usr/lib/
endef

$(eval $(generic-package))
