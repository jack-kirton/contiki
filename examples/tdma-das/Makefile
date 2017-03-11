CONTIKI_PROJECT = tdma-das
all: $(CONTIKI_PROJECT)

CONTIKI = ../..
MODULES += core/net/mac/das
MODULES += core/net/rime
#CFLAGS += -DPROJECT_CONF_H=\"project-conf.h\"
CFLAGS += -DNETSTACK_CONF_RDC=nullrdc_driver -DNETSTACK_CONF_MAC=das_mac_driver
CFLAGS += -DNETSTACK_CONF_WITH_RIME=1 -DNETSTACK_CONF_WITH_PACKET_TYPE=1
CFLAGS += -DDAS_DEBUG=1
#CFLAGS += -DUIP_CONF_TCP=0 -DUIP_CONF_UDP=0

CFLAGS += -ffunction-sections
LDFLAGS += -Wl,--gc-sections,--undefined=_reset_vector__,--undefined=InterruptVectors,--undefined=_copy_data_init__,--undefined=_clear_bss_init__,--undefined=_end_of_init__

include $(CONTIKI)/Makefile.include
