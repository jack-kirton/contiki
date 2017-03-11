#ifndef DAS_MAC_H_
#define DAS_MAC_H_

#include "net/mac/mac.h"
//#include "dev/radio.h"
#include "lib/list.h"
#include "net/rime/packetqueue.h"

extern const struct mac_driver das_mac_driver;

typedef struct DasOutgoingPacket {
    void* next;
    struct queuebuf* qb;
    mac_callback_t sent;
    void* ptr;
} DasOutgoingPacket;

//Callback for the mac layer to aggregate packets from multiple sources into a single packet
typedef void (*das_aggregation_callback)(struct packetqueue* packets);

//Set before on is called
void das_set_sink(int active);
void das_set_aggregation_callback(das_aggregation_callback f);

#endif /* DAS_MAC_H_ */
