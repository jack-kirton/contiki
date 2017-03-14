// Includes {{{
#include "net/mac/das/das-mac.h"
/*#include "net/mac/mac-sequence.h"*/
#include "net/netstack.h"
#include "net/packetbuf.h"
#include "net/queuebuf.h"

#include "lib/list.h"
/*#include "lib/memb.h"*/
#include "lib/mmem.h"
#include "lib/random.h"
#include "sys/node-id.h"
#include "sys/clock.h"
#include "sys/ctimer.h"
#include "sys/rtimer.h"
#include "net/rime/packetqueue.h"

#include <stdlib.h>
#include <string.h>
// }}}

//TODO: Safety check all mmem_alloc calls
//TODO: Potentially remove mmem to use malloc instead?
//TODO: Add sent callback argument to aggregation callback

// Macros {{{
//Application definable macros
#if DAS_DEBUG
#include <stdio.h>
#include <assert.h>
#define PRINTF(...) printf(__VA_ARGS__)
#define ASSERT(condition) assert(condition)
#else
#define PRINTF(...)
#define ASSERT(condition)
#endif

#ifndef DAS_NUM_SLOTS
    #define DAS_NUM_SLOTS 100
#endif
#ifndef DAS_DISSEM_PERIOD_MILLI
    #define DAS_DISSEM_PERIOD_MILLI 500
#endif
#ifndef DAS_SLOT_PERIOD_MILLI
    #define DAS_SLOT_PERIOD_MILLI 100
#endif
#ifndef DAS_GUARD_PERIOD_MILLI
    #define DAS_GUARD_PERIOD_MILLI 0
#endif
#ifndef DAS_NEIGHBOUR_DISCOVERY_PERIODS
    #define DAS_NEIGHBOUR_DISCOVERY_PERIODS 5
#endif
#ifndef DAS_OUTGOING_QUEUE_SIZE
    #define DAS_OUTGOING_QUEUE_SIZE 8
#endif

//In-file macros
#define BOTTOM -1

/*#define DAS_PERIOD_MILLI (DAS_DISSEM_PERIOD_MILLI + DAS_NUM_SLOTS*DAS_SLOT_PERIOD_MILLI + (DAS_NUM_SLOTS+1)*DAS_GUARD_PERIOD_MILLI)*/
#define DAS_PERIOD_MILLI (DAS_DISSEM_PERIOD_MILLI + DAS_NUM_SLOTS*DAS_SLOT_PERIOD_MILLI)

#define DAS_DISSEM_PERIOD_SEC (DAS_DISSEM_PERIOD_MILLI/1000.0f)
#define DAS_SLOT_PERIOD_SEC (DAS_SLOT_PERIOD_MILLI/1000.0f)
#define DAS_GUARD_PERIOD_SEC (DAS_GUARD_PERIOD_MILLI/1000.0f)
#define DAS_PERIOD_SEC (DAS_PERIOD_MILLI/1000.0f)

//Helper macros
#define DISSEM_PTR(dissem_message) ((NeighbourInfoMessage*)MMEM_PTR(dissem_message))
#define DISSEM_SIZE(dissem_message) (sizeof(NeighbourInfoMessage) + sizeof(NeighbourInfo)*((NeighbourInfoMessage*)MMEM_PTR(dissem_message))->num_neighbours)
#define OUTGOING_PTR(packet) ((DasOutgoingPacket*)MMEM_PTR(packet))
#define NEIGHBOURINFO_PTR(neighbour_info) ((NeighbourInfo*)MMEM_PTR(neighbour_info))
#define NEIGHBOUR_PTR(neighbour) *((unsigned short*)MMEM_PTR(neighbour->data))
// }}}

// Data Structures and Enumerations {{{
typedef enum {
    NODE_TYPE_NORMAL,
    NODE_TYPE_SINK
} NodeType;

typedef enum {
    PACKET_TYPE_NORMAL = 1,
    PACKET_TYPE_DISSEM
} PacketType;

typedef struct Neighbour {
    void* next;
    unsigned short id;
} Neighbour;

typedef struct NeighbourInfo {
    void* next;
    unsigned short id;
    int hop;
    int slot;
} NeighbourInfo;

typedef struct __attribute__ ((__packed__)) NeighbourInfoMessage {
    unsigned short id;
    int num_neighbours;
    NeighbourInfo neighbours[];
} NeighbourInfoMessage;
// }}}

// Function Prototypes {{{
// Timer Callbacks
static void dissem_sent_callback(void* ptr, int status, int transmissions);
static void dissem_send_timer_callback(struct rtimer* task, void* ptr);
static void dissem_timer_callback(void* ptr);
static void slot_timer_callback(void* ptr);
// }}}

// Global Variables {{{
static int is_init = 0;
static int is_on = 0;

static int parent;
static int slot;
static int hop;

static volatile int slot_changed = 0;

LIST(my_n);
LIST(n_info);
PACKETQUEUE(outgoing_packets, DAS_OUTGOING_QUEUE_SIZE);

static NodeType node_type;

static struct ctimer dissem_timer = {};
static struct rtimer dissem_send_timer = {};
static struct ctimer slot_timer = {};

uint8_t packet_seqno = 0;

static das_aggregation_callback aggregation_callback = NULL;
// }}}

// Helper Functions {{{
static void add_neighbour_id(unsigned short id) {
    Neighbour* el = list_head(my_n);
    while(el) {
        if(el->id == id) {
            PRINTF("DAS-mac: my_n id already exists\n");
            return;
        }
        el = list_item_next(el);
    }

    PRINTF("Asserting...\n");
    ASSERT(el = malloc(sizeof(Neighbour)));
    el->id = id;
    list_add(my_n, el);
    PRINTF("DAS-mac: my_n length %d\n", list_length(my_n));
}

static void add_neighbour_info(NeighbourInfo* info) {
    NeighbourInfo* el = list_head(n_info);
    while(el) {
        if(el->id == info->id) {
            if(info->slot != BOTTOM) {
                if(el->slot == BOTTOM || el->slot > info->slot) {
                    el->slot = info->slot;
                }
            }
            if(info->hop != BOTTOM) {
                if(info->hop == BOTTOM || el->hop > info->hop) {
                    el->hop = info->hop;
                }
            }
            return;
        }
        el = list_item_next(el);
    }

    ASSERT(el = malloc(sizeof(NeighbourInfo)));
    /*memcpy(el, info, sizeof(NeighbourInfo));*/
    el->id = info->id;
    el->slot = info->slot;
    el->hop = info->hop;
    list_add(n_info, el);
    PRINTF("DAS-mac: n_info length %d\n", list_length(n_info));

#if DAS_DEBUG
    el = list_head(n_info);
    PRINTF("n_info: ");
    while(el) {
        PRINTF("NeighbourInfo<id=%u,slot=%d,hop=%d>", el->id, el->slot, el->hop);
        el = list_item_next(el);
        if(el) PRINTF(", ");
    }
    PRINTF("\n");
#endif
}

static int is_1hop_neighbour(int id) {
    Neighbour* el = list_head(my_n);
    while(el) {
        if(el->id == id) {
            return 1;
        }
        el = list_item_next(el);
    }
    return 0;
}

static NeighbourInfo* get_neighbourinfo(int id) {
    //TODO: If NeighbourInfo alloc with mmem, needs changing
    void* el = list_head(n_info);
    while(el) {
        if(((NeighbourInfo*)el)->id == id) {
            return (NeighbourInfo*)el;
        }
        el = list_item_next(el);
    }
    return NULL;
}

static struct mmem dissem_message;

static void build_neighbourinfo_message() {
    int len = list_length(my_n);
    if(!mmem_alloc(&dissem_message, sizeof(NeighbourInfoMessage) + sizeof(NeighbourInfo)*len)) {
        PRINTF("DAS-mac: Could not allocate NeighbourInfoMessage. Aborting...\n");
        //TODO: Abort program here
    }
    DISSEM_PTR(&dissem_message)->id = node_id;
    DISSEM_PTR(&dissem_message)->num_neighbours = len;
    PRINTF("NUM_NEIGHBOURS IS %d\n", DISSEM_PTR(&dissem_message)->num_neighbours);
    PRINTF("N_INFO size %d\n", list_length(n_info));
    NeighbourInfo* el = list_head(n_info);
    int i = 0;
    while(el) {
        if(i >= len) {
            break;
        }
        else if(!is_1hop_neighbour(el->id)) {
            PRINTF("IS NOT 1HOP NEIGHBOUR\n");
            el = list_item_next(el);
            continue;
        }
        else {
            DISSEM_PTR(&dissem_message)->neighbours[i].id = el->id;
            DISSEM_PTR(&dissem_message)->neighbours[i].slot = el->slot;
            DISSEM_PTR(&dissem_message)->neighbours[i].hop = el->hop;
            i++;
            el = list_item_next(el);
        }
    }
    PRINTF("DAS-mac: Built NeighbourInfoMessage with size %d\n", i);
}

static void update_slot(int new_slot) {
    slot = new_slot;
    void* el = list_head(n_info);
    while(el) {
        if(((NeighbourInfo*)el)->id == node_id) {
            ((NeighbourInfo*)el)->slot = new_slot;
            break;
        }
        el = list_item_next(el);
    }
    slot_changed = 1;
}

static void update_hop(int new_hop) {
    hop = new_hop;
    void* el = list_head(n_info);
    while(el) {
        if(((NeighbourInfo*)el)->id == node_id) {
            ((NeighbourInfo*)el)->hop = new_hop;
            break;
        }
        el = list_item_next(el);
    }
}

static void update_slot_and_hop(int new_slot, int new_hop) {
    slot = new_slot;
    hop = new_hop;
    void* el = list_head(n_info);
    while(el) {
        if(((NeighbourInfo*)el)->id == node_id) {
            ((NeighbourInfo*)el)->slot = new_slot;
            ((NeighbourInfo*)el)->hop = new_hop;
            break;
        }
        el = list_item_next(el);
    }
    slot_changed = 1;
}

static void update_parent(int parent_id) {
    parent = parent_id;
}
// }}}

// Timer Functions {{{
static void dissem_sent_callback(void* ptr, int status, int transmissions) {
    //Free message after sending complete
    mmem_free(&dissem_message);
    PRINTF("DAS-mac: Dissem sent\n");
}

static void dissem_send_timer_callback(struct rtimer* task, void* ptr) {
    //TODO: Check this does not fill the packetbuf
    packetbuf_clear();
    /*packetbuf_copyfrom(MMEM_PTR(&dissem_message), DISSEM_SIZE(&dissem_message));*/
    assert(DISSEM_SIZE(&dissem_message) <= PACKETBUF_SIZE); //This will not catch all cases (assumes no header)
    /*memcpy(packetbuf_dataptr(), MMEM_PTR(&dissem_message), DISSEM_SIZE(&dissem_message));*/
    packetbuf_copyfrom(MMEM_PTR(&dissem_message), DISSEM_SIZE(&dissem_message));

    packetbuf_set_addr(PACKETBUF_ADDR_SENDER, &linkaddr_node_addr);
    packetbuf_set_attr(PACKETBUF_ATTR_PACKET_TYPE, PACKET_TYPE_DISSEM);
    NETSTACK_RDC.send(dissem_sent_callback, NULL);
}

static void dissem_timer_callback(void* ptr) {
    if(slot_changed && node_type != NODE_TYPE_SINK) {
        slot_changed = 0;
        ctimer_set(&slot_timer, (DAS_DISSEM_PERIOD_SEC + slot*DAS_SLOT_PERIOD_SEC)*CLOCK_SECOND, slot_timer_callback, NULL);
    }

    //If the timer has not been setup yet
    if(dissem_timer.f == NULL) {
        PRINTF("DAS-mac: Setting dissem timer\n");
        ctimer_set(&dissem_timer, DAS_PERIOD_SEC*CLOCK_SECOND, dissem_timer_callback, NULL);
    }
    else {
        PRINTF("DAS-mac: Resetting dissem timer\n");
        ctimer_reset(&dissem_timer);
    }

    PRINTF("DAS-mac: Dissem active\n");

    build_neighbourinfo_message();
    //Send dissem message at random in range of DAS_DISSEM_PERIOD
    PRINTF("DAS-mac: Setting dissem clock\n");
    rtimer_clock_t offset = (rtimer_clock_t)(((float)random_rand()/(float)RANDOM_RAND_MAX)*DAS_DISSEM_PERIOD_SEC*RTIMER_SECOND);
    rtimer_set(&dissem_send_timer, RTIMER_NOW() + offset, 0, dissem_send_timer_callback, NULL);
}

/*static void normal_sent_callback(void* ptr, int status, int transmissions) {*/
    /*[>mac_call_sent_callback(OUTGOING_PTR(ptr)->sent, OUTGOING_PTR(ptr)->ptr, status, transmissions);<]*/
    /*[>queuebuf_free(OUTGOING_PTR(ptr)->qb);<]*/
    /*[>mmem_free(ptr);<]*/
    /*//TODO: PRINTF debug information*/
/*}*/

static void slot_timer_callback(void* ptr) {
    ctimer_reset(&slot_timer);
    PRINTF("DAS-mac: Slot active (%u)\n", slot);
    //TODO: Only want to send one aggregated packet of data per slot

    (*aggregation_callback)(&outgoing_packets);
    packetbuf_set_addr(PACKETBUF_ADDR_ESENDER, &linkaddr_node_addr);
    packetbuf_set_attr(PACKETBUF_ATTR_MAC_SEQNO, packet_seqno);
    packetbuf_set_attr(PACKETBUF_ATTR_PACKET_TYPE, PACKET_TYPE_NORMAL);
    NETSTACK_RDC.send(NULL, NULL);

    while(packetqueue_len(&outgoing_packets)) {
        packetqueue_dequeue(&outgoing_packets); //Dequeue also frees internal queuebuf
    }

    /*DasOutgoingPacket aggregated_packet = {};*/
    /*if(aggregated_packet.qb && aggregated_packet.sent) {*/
        /*if(++packet_seqno == 0) {*/
            /*packet_seqno++;*/
        /*}*/
        /*packetbuf_clear();*/
        /*queuebuf_to_packetbuf(aggregated_packet.qb);*/
        /*packetbuf_set_addr(PACKETBUF_ADDR_ESENDER, &linkaddr_node_addr);*/
        /*packetbuf_set_attr(PACKETBUF_ATTR_MAC_SEQNO, packet_seqno);*/
        /*packetbuf_set_attr(PACKETBUF_ATTR_PACKET_TYPE, PACKET_TYPE_NORMAL);*/
        /*NETSTACK_RDC.send(aggregated_packet.sent, NULL);*/
        /*[>mac_call_sent_callback(OUTGOING_PTR(&aggregated_packet)->sent, OUTGOING_PTR()->ptr, status, transmissions);<]*/
    /*}*/

    /*if(aggregated_packet.qb) {*/
        /*queuebuf_free(aggregated_packet.qb);*/
    /*}*/

    /*struct mmem* packet = list_head(outgoing_packets);*/
    /*while(packet) {*/
        /*queuebuf_free(OUTGOING_PTR(packet)->qb);*/
        /*mmem_free(packet);*/
        /*list_pop(outgoing_packets); //TODO: Check that this does not break (popping before list_item_next())*/
        /*packet = list_item_next(packet);*/
    /*}*/
}
// }}}

// Other Functions {{{
static void handle_dissem_message() {
    //TODO: packetbuf_dataptr() probably points to the header? Remove header in RDC or framer?
    NeighbourInfoMessage* message = packetbuf_dataptr();
    add_neighbour_id(message->id);
    int i;
    for(i = 0; i < message->num_neighbours; i++) {
        PRINTF("NeighbourInfo<id=%u,slot=%d,hop=%d>\n", message->neighbours[i].id, message->neighbours[i].slot, message->neighbours[i].hop);
        /*add_neighbour_info(&((message->neighbours)[i]));*/
        add_neighbour_info(&(message->neighbours[i]));
    }
}

static void handle_normal_message() {
    //Either forward message or if sink propagate to the application
    if(node_type == NODE_TYPE_SINK) {
        NETSTACK_NETWORK.input();
    }
    else {
        //TODO: Use packetqueue instead of current list
        //TODO: Also check for repeated sequence numbers?
        /*packetbuf_compact(); //TODO: Does not move up header*/
        /*packetbuf_set_addr(PACKETBUF_ADDR_SENDER, &linkaddr_node_addr);*/
        /*packetbuf_set_attr(PACKETBUF_ATTR_HOPS, packetbuf_attr(PACKETBUF_ATTR_HOPS) + 1);*/
        /*//TODO: Check if empty normal message?*/
        /*struct mmem* packet = NULL;*/
        /*mmem_alloc(packet, sizeof(DasOutgoingPacket));*/
        /*OUTGOING_PTR(packet)->qb = queuebuf_new_from_packetbuf();*/
        /*OUTGOING_PTR(packet)->sent = NULL;*/
        /*OUTGOING_PTR(packet)->ptr = NULL;*/
        /*list_add(outgoing_packets, packet);*/
        packetqueue_enqueue_packetbuf(&outgoing_packets, 0, NULL);
    }
}
// }}}

// Exported Functions {{{
/*---------------------------------------------------------------------------*/
void das_set_sink(int active) {
    //Set before on is called
    node_type = (active) ? NODE_TYPE_SINK : NODE_TYPE_NORMAL;
}

void das_set_aggregation_callback(das_aggregation_callback f) {
    aggregation_callback = f;
}

static void packet_send(mac_callback_t sent, void* ptr) {
    //TODO: Set more attributes
    packetbuf_set_addr(PACKETBUF_ADDR_ESENDER, &linkaddr_node_addr);
    packetbuf_set_attr(PACKETBUF_ATTR_HOPS, 0);
    packetqueue_enqueue_packetbuf(&outgoing_packets, 0, NULL);
    mac_call_sent_callback(sent, ptr, MAC_TX_DEFERRED, 0);
}

/*static void send_packet(mac_callback_t sent, void *ptr)*/
/*{*/
    /*//TODO Save sent for each packet in packetqueue*/
    /*int retval = MAC_TX_DEFERRED;*/
    /*[>const linkaddr_t *addr = packetbuf_addr(PACKETBUF_ADDR_RECEIVER);<]*/

    /*//Something to do with the framer being unable to handle zero*/
    /*if(++packet_seqno == 0)*/
        /*packet_seqno++;*/

    /*packetbuf_set_attr(PACKETBUF_ATTR_MAC_SEQNO, packet_seqno);*/
    /*packetbuf_set_attr(PACKETBUF_ATTR_PACKET_TYPE, PACKET_TYPE_NORMAL);*/

    /*if(NETSTACK_FRAMER.create() < 0)*/
    /*{*/
        /*retval = MAC_TX_ERR;*/
    /*}*/
    /*else*/
    /*{*/
        /*//Add packet to queue*/
        /*if(das_queue_outgoing_add(sent, ptr)<0)*/
        /*{*/
            /*retval = MAC_TX_ERR;*/
        /*}*/
    /*}*/

    /*if(retval != MAC_TX_DEFERRED)*/
    /*{*/
        /*mac_call_sent_callback(sent, ptr, retval, 1);*/
    /*}*/
/*}*/


/*---------------------------------------------------------------------------*/
static void packet_input(void) {
    //TODO: Does RDC call the framer? Probably not
    /*if(NETSTACK_FRAMER.parse() < 0) {*/
        /*PRINTF("DAS-mac: Framer failed to parse %u\n", packetbuf_datalen());*/
        /*return;*/
    /*}*/
    if(packetbuf_attr(PACKETBUF_ATTR_PACKET_TYPE) == PACKET_TYPE_DISSEM) {
        PRINTF("DAS-mac: Received dissem message\n");
        handle_dissem_message();
    }
    else if(packetbuf_attr(PACKETBUF_ATTR_PACKET_TYPE) == PACKET_TYPE_NORMAL) {
        PRINTF("DAS-mac: Received normal message\n");
        handle_normal_message();
    }
    else {
        PRINTF("DAS-mac: Received unknown packet type\n");
    }
}
/*---------------------------------------------------------------------------*/
static int on(void)
{
    PRINTF("DAS-mac: Turning on...\n");
    if(is_on) {
        PRINTF("DAS-mac: Already on\n");
        return 0;
    }
    parent = BOTTOM;
    slot = BOTTOM;
    hop = BOTTOM;

    //TODO:Clear these lists, init will not work
    /*list_init(my_n);*/
    /*list_init(n_info);*/
    /*list_init(outgoing_packets);*/

    add_neighbour_id(node_id);

    NeighbourInfo self_info = {};
    self_info.id = node_id;
    self_info.slot = BOTTOM;
    self_info.hop = BOTTOM;
    add_neighbour_info(&self_info);

    if(node_type == NODE_TYPE_SINK) {
        update_slot_and_hop(DAS_NUM_SLOTS, 0);
    }

    dissem_timer_callback(NULL);
    is_on = 1;
    return 1;
}
/*---------------------------------------------------------------------------*/
static int off(int keep_radio_on)
{
    PRINTF("DAS-mac: Turning off not implemented\n");
    return 0;
}
/*---------------------------------------------------------------------------*/
static unsigned short channel_check_interval(void)
{
    return 0;
}
/*---------------------------------------------------------------------------*/
static void init(void)
{
    if(is_init) return;
    node_type = NODE_TYPE_NORMAL;
    list_init(my_n);
    list_init(n_info);
    packetqueue_init(&outgoing_packets);

    mmem_init();
    random_init(node_id);
    //XXX: NEVER INIT THESE HERE, IT BREAKS ALL TIMERS
    /*clock_init();*/
    /*ctimer_init();*/
    /*rtimer_init();*/

    //Initialise radio
    radio_value_t radio_rx_mode = 0;
    radio_value_t radio_tx_mode = 0;
    radio_rx_mode &= ~RADIO_RX_MODE_ADDRESS_FILTER;
    radio_rx_mode &= ~RADIO_RX_MODE_AUTOACK;
    radio_rx_mode &= ~RADIO_RX_MODE_POLL_MODE;
    if(NETSTACK_RADIO.set_value(RADIO_PARAM_RX_MODE, radio_rx_mode) != RADIO_RESULT_OK)
    {
        PRINTF("DAS-mac: Radio does not support required rx mode. Abort init.\n");
        return;
    }
    if(NETSTACK_RADIO.get_value(RADIO_PARAM_TX_MODE, &radio_tx_mode) != RADIO_RESULT_OK)
    {
        PRINTF("DAS-mac: Radio does not support getting tx mode. Abort init.\n");
        return;
    }
    radio_tx_mode &= ~RADIO_TX_MODE_SEND_ON_CCA;
    if(NETSTACK_RADIO.set_value(RADIO_PARAM_TX_MODE, radio_tx_mode) != RADIO_RESULT_OK)
    {
        PRINTF("DAS-mac: Radio does not support required tx mode. Abort init.\n");
        return;
    }
    NETSTACK_RDC.on();
    NETSTACK_RADIO.on();
    is_init = 1;
}
/*---------------------------------------------------------------------------*/
const struct mac_driver das_mac_driver = {
    "DAS-mac",
    init,
    packet_send,
    packet_input,
    on,
    off,
    channel_check_interval,
};
/*---------------------------------------------------------------------------*/
// }}}
