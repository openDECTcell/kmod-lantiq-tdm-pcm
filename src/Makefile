# SPDX-License-Identifier: GPL-2.0-only
#
# kbuild for the Lantiq VR9 TDM/PCM transport module.
#
obj-m += lantiq_tdm_pcm.o
lantiq_tdm_pcm-objs := tdm_pcm.o tdm_pcm_fallback.o

ccflags-y += -I$(src)/include/uapi
