#include "net/mac/das/das-framer.h"
#include "net/packetbuf.h"


#ifdef DAS_DEBUG
#include <stdio.h>
#define PRINTF(...) printf(__VA_ARGS__)
#define PRINTADDR(addr) PRINTF(" %02x%02x:%02x%02x:%02x%02x:%02x%02x ", ((uint8_t *)addr)[0], ((uint8_t *)addr)[1], ((uint8_t *)addr)[2], ((uint8_t *)addr)[3], ((uint8_t *)addr)[4], ((uint8_t *)addr)[5], ((uint8_t *)addr)[6], ((uint8_t *)addr)[7])
#else
#define PRINTF(...)
#define PRINTADDR(addr)
#endif

typedef struct DasHeader {
    /*linkaddr_t receiver;*/
    linkaddr_t esender;
    linkaddr_t sender;
    uint8_t seqno;
    uint8_t type;
    uint16_t hops;
} DasHeader;

/*---------------------------------------------------------------------------*/
static int header_length()
{
    return sizeof(DasHeader);
}
/*---------------------------------------------------------------------------*/
static int create() {
    DasHeader *hdr;

    if(packetbuf_hdralloc(sizeof(DasHeader))) {
        hdr = packetbuf_hdrptr();
        linkaddr_copy(&(hdr->sender), &linkaddr_node_addr);
        linkaddr_copy(&(hdr->esender), packetbuf_addr(PACKETBUF_ADDR_ESENDER));
        hdr->seqno = packetbuf_attr(PACKETBUF_ATTR_MAC_SEQNO);
        hdr->type = packetbuf_attr(PACKETBUF_ATTR_PACKET_TYPE);
        hdr->hops = packetbuf_attr(PACKETBUF_ATTR_HOPS);
        return sizeof(DasHeader);
    }
    PRINTF("DAS-framer: too large header: %u\n", sizeof(DasHeader));
    return FRAMER_FAILED;
}
/*---------------------------------------------------------------------------*/
static int parse(void) {
    DasHeader *hdr;
    hdr = packetbuf_dataptr();
    if(packetbuf_hdrreduce(sizeof(DasHeader))) {
        packetbuf_set_addr(PACKETBUF_ADDR_SENDER, &(hdr->sender));
        packetbuf_set_addr(PACKETBUF_ADDR_ESENDER, &(hdr->esender));
        packetbuf_set_attr(PACKETBUF_ATTR_MAC_SEQNO, hdr->seqno);
        packetbuf_set_attr(PACKETBUF_ATTR_PACKET_TYPE, hdr->type);
        packetbuf_set_attr(PACKETBUF_ATTR_HOPS, hdr->hops);
        /*packetbuf_set_addr(PACKETBUF_ADDR_RECEIVER, &(hdr->receiver));*/

        /*PRINTF("DAS-framer: ");*/
        /*PRINTADDR(packetbuf_addr(PACKETBUF_ADDR_SENDER));*/
        /*PRINTADDR(packetbuf_addr(PACKETBUF_ADDR_RECEIVER));*/
        /*PRINTF("%u (%u)\n", packetbuf_datalen(), sizeof(DasHeader));*/

        return sizeof(DasHeader);
    }
    PRINTF("DAS-framer: Failed to parse header\n");
    return FRAMER_FAILED;
}
/*---------------------------------------------------------------------------*/
const struct framer das_framer = {
  header_length,
  create,
  parse
};
