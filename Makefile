# SPDX-License-Identifier: GPL-2.0-only
#
# OpenWrt package for the Lantiq VR9 TDM/PCM transport.
#
# Runs the VR9's TDM/PCM block as a PCM slave (peer-supplied bit clock and
# frame sync) and exposes it as /dev/tdm-pcm0 with kmod-lantiq-ssc-pcm's
# uapi. Build it out of tree with the SDK (tools/build.sh) or drop this
# directory into a feed.
#
include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=lantiq-tdm-pcm
PKG_VERSION:=0.1
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only
PKG_BUILD_DIR:=$(KERNEL_BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)

TDM_PCM_SRC:=$(abspath $(CURDIR)/src)

include $(INCLUDE_DIR)/package.mk

define KernelPackage/lantiq-tdm-pcm
  SUBMENU:=Other modules
  TITLE:=Lantiq VR9 TDM/PCM block as a PCM slave transport
  DEPENDS:=@TARGET_lantiq_xrx200
  FILES:=$(PKG_BUILD_DIR)/lantiq_tdm_pcm.ko
  # No AUTOLOAD: whoever uses the transport loads it.
endef

define KernelPackage/lantiq-tdm-pcm/description
  Runs the Lantiq VR9 TDM/PCM block as a PCM slave (bit clock and frame
  sync from the peer) and exposes it as /dev/tdm-pcm0, with the same
  interface as kmod-lantiq-ssc-pcm's /dev/ssc-pcm0 (uapi version 3).
endef

define Build/Prepare
	$(INSTALL_DIR) $(PKG_BUILD_DIR)/include/uapi
	$(CP) $(TDM_PCM_SRC)/*.c $(TDM_PCM_SRC)/*.h $(TDM_PCM_SRC)/Makefile \
		$(PKG_BUILD_DIR)/
	$(CP) $(TDM_PCM_SRC)/include/uapi/ssc_pcm.h $(PKG_BUILD_DIR)/include/uapi/
endef

define Build/Compile
	$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)" modules
endef

$(eval $(call KernelPackage,lantiq-tdm-pcm))
