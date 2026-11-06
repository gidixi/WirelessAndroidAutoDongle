AA_SRCD_VERSION = 1.0
AA_SRCD_SITE = $(BR2_EXTERNAL_AA_WIRELESS_DONGLE_PATH)/package/aa-srcd
AA_SRCD_SITE_METHOD = local
AA_SRCD_DEPENDENCIES = openssl x264 lvgl
AA_SRCD_LICENSE = MIT
AA_SRCD_LICENSE_FILES = $(BR2_EXTERNAL_AA_WIRELESS_DONGLE_PATH)/../../LICENSE

# The lvgl package staged our lv_conf.h next to the headers, so the daemon sees
# exactly the same LVGL configuration the library was compiled with.
AA_SRCD_LVGL_CFLAGS = -DLV_CONF_INCLUDE_SIMPLE -I$(STAGING_DIR)/usr/include/lvgl \
	-I$(STAGING_DIR)/usr/include/lvgl-conf

# Explicit WITH_/AA_SRCD_EXTRA_ values keep the Makefile from probing the host
# with pkg-config during a cross build. The local site method copies the source
# tree verbatim, including any host build outputs, so clean them first.
define AA_SRCD_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) clean
	$(TARGET_MAKE_ENV) $(MAKE) $(TARGET_CONFIGURE_OPTS) -C $(@D) \
		WITH_X264=1 WITH_LVGL=1 \
		AA_SRCD_EXTRA_CXXFLAGS="$(AA_SRCD_LVGL_CFLAGS)" \
		AA_SRCD_EXTRA_LIBS="-lx264 -llvgl -lssl -lcrypto -lpthread -lm -ldl"
endef

define AA_SRCD_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/aa-srcd $(TARGET_DIR)/usr/bin/aa-srcd
endef

$(eval $(generic-package))
