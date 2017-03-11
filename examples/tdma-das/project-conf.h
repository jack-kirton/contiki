
#define WITH_DAS 0

#define MMEM_CONF_SIZE 1024

#if WITH_DAS /* Use TDMA DAS */

#undef NETSTACK_CONF_RDC
#undef NETSTACK_CONF_MAC
#define NETSTACK_CONF_RDC nullrdc_driver
#define NETSTACK_CONF_MAC das_mac_driver

//#undef NETSTACK_CONF_WITH_RIME
//#undef PACKETBUF_CONF_WITH_PACKET_TYPE
//#define NETSTACK_CONF_WITH_RIME 1
//#define PACKETBUF_CONF_WITH_PACKET_TYPE 1

//#undef UIP_CONF_TCP
//#undef UIP_CONF_UDP
//#define UIP_CONF_TCP 0
//#define UIP_CONF_UDP 0

#define DEBUG 1
#define DAS_DEBUG 1

#else /* No TDMA DAS */

//#undef NETSTACK_CONF_RDC
//#undef NETSTACK_CONF_MAC
//#define NETSTACK_CONF_RDC nullrdc_driver
//#define NETSTACK_CONF_MAC csma_driver

#endif
