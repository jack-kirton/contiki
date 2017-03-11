// Includes {{{
#include "net/mac/das/das-mac.h"
/*#include "net/mac/das/das-common.h"*/
/*#include "net/mac/das/das-queue.h"*/
/*#include "net/mac/mac-sequence.h"*/
#include "net/netstack.h"
#include "net/packetbuf.h"
#include "net/queuebuf.h"

#include "lib/list.h"
/*#include "lib/memb.h"*/
#include "lib/mmem.h"
#include "lib/random.h"
#include "sys/node-id.h"
/*#include "sys/etimer.h"*/
#include "sys/clock.h"
#include "sys/ctimer.h"
#include "sys/rtimer.h"
/*#include "net/rime/packetqueue.h"*/
// }}}

//TODO: Safety check all mmem_alloc calls
//TODO: Use Packetqueue instead of current list

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
#define NEIGHBOUR_PTR(neighbour_info) ((NeighbourInfo*)MMEM_PTR(neighbour_info))
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

typedef struct NeighbourInfoMessage {
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
/*static void normal_sent_callback(void* ptr, int status, int transmissions);*/
static void slot_timer_callback(void* ptr);
static void postslot_timer_callback(void* ptr);
// }}}

// Global Variables {{{
static int is_init = 0;
static int is_on = 0;

static int parent;
static int slot;
static int hop;

static volatile int slot_active = 0;
static volatile int slot_changed = 0;

LIST(my_n);
LIST(n_info);
LIST(outgoing_packets);

static NodeType node_type;

static struct ctimer dissem_timer = {};
static struct rtimer dissem_send_timer = {};
static struct ctimer slot_timer = {};
static struct ctimer postslot_timer = {};

uint8_t packet_seqno = 0;

static das_aggregation_callback aggregation_callback = NULL;
// }}}

// Helper Functions {{{
static int is_1hop_neighbour(int id) {
    //TODO: If alloc with mmem, needs changing
    void* el = list_head(my_n);
    while(el) {
        if(((Neighbour*)el)->id == id) {
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
    DISSEM_PTR(&dissem_message)->id = 0;
    DISSEM_PTR(&dissem_message)->num_neighbours = len;
    void* el = list_head(n_info);
    int i = 0;
    while(el) {
        if(i >= len) {
            break;
        }
        else if(!is_1hop_neighbour(((NeighbourInfo*)el)->id)) {
            el = list_item_next(el);
            continue;
        }
        else {
            memcpy(&(DISSEM_PTR(&dissem_message)->neighbours[i]), el, sizeof(NeighbourInfo));
            i++;
            el = list_item_next(el);
        }
    }
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
    PRINTF("DAS-mac: Dissem sending...\n");
    /*struct mmem* dissem_message = (struct mmem*)ptr;*/
    //TODO: Check this does not fill the packetbuf
    packetbuf_clear();
    packetbuf_copyfrom(MMEM_PTR(&dissem_message), DISSEM_SIZE(&dissem_message));

    packetbuf_set_addr(PACKETBUF_ADDR_SENDER, &linkaddr_node_addr);
    packetbuf_set_attr(PACKETBUF_ATTR_PACKET_TYPE, PACKET_TYPE_DISSEM);
    NETSTACK_RDC.send(dissem_sent_callback, NULL);
    /*mmem_free(dissem_message);*/
}

static void dissem_timer_callback(void* ptr) {
    if(slot_changed) {
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

    //TODO: Set this up to randomise dissem message send times
    //Send dissem message at random in range of DAS_DISSEM_PERIOD
    PRINTF("DAS-mac: Setting dissem clock\n");
    rtimer_clock_t offset = (rtimer_clock_t)(((float)random_rand()/(float)RANDOM_RAND_MAX)*DAS_DISSEM_PERIOD_SEC*RTIMER_SECOND);
    rtimer_set(&dissem_send_timer, RTIMER_NOW() + offset, 0, dissem_send_timer_callback, NULL);
    /*dissem_send_timer_callback(NULL, NULL);*/
}

/*static void normal_sent_callback(void* ptr, int status, int transmissions) {*/
    /*[>mac_call_sent_callback(OUTGOING_PTR(ptr)->sent, OUTGOING_PTR(ptr)->ptr, status, transmissions);<]*/
    /*[>queuebuf_free(OUTGOING_PTR(ptr)->qb);<]*/
    /*[>mmem_free(ptr);<]*/
    /*//TODO: PRINTF debug information*/
/*}*/

static void slot_timer_callback(void* ptr) {
    //Setup postslot timer
    //TODO: Change to ctimer
    rtimer_clock_t when = RTIMER_NOW() + DAS_SLOT_PERIOD_SEC*RTIMER_SECOND;
    rtimer_set(&postslot_timer, when, 0, postslot_timer_callback, NULL);
    slot_active = 1;
    PRINTF("DAS-mac: Slot active (%u)\n", slot);
    //TODO: Only want to send one aggregated packet of data per slot
    /*while(slot_active) { //Instead of a process?*/
        /*struct mmem* packet = (struct mmem*)list_pop(outgoing_packets);*/
        /*if(!packet) break;*/

        /*queuebuf_to_packetbuf(OUTGOING_PTR(packet)->qb);*/

        /*//Something to do with the framer being unable to handle zero*/
        /*if(++packet_seqno == 0)*/
            /*packet_seqno++;*/

        /*packetbuf_set_addr(PACKETBUF_ADDR_ESENDER, &linkaddr_node_addr);*/
        /*packetbuf_set_attr(PACKETBUF_ATTR_MAC_SEQNO, packet_seqno);*/
        /*packetbuf_set_attr(PACKETBUF_ATTR_PACKET_TYPE, PACKET_TYPE_NORMAL);*/
        /*NETSTACK_RDC.send(normal_sent_callback, packet);*/
    /*}*/

    /*list_init(outgoing_ptrs);*/
    /*struct mmem* packet = list_head(outgoing_packets);*/
    /*if(!packet) return; //TODO: Check this is the correct action*/
    /*while(packet) {*/
        /*list_add(outgoing_ptrs, queuebuf_dataptr(OUTGOING_PTR(packet)->qb));*/
        /*packet = list_item_next(packet);*/
    /*}*/

    DasOutgoingPacket aggregated_packet = {};
    (*aggregation_callback)(outgoing_packets, &aggregated_packet);
    if(aggregated_packet.qb && aggregated_packet.sent) {
        if(++packet_seqno == 0) {
            packet_seqno++;
        }
        packetbuf_clear();
        queuebuf_to_packetbuf(aggregated_packet.qb);
        packetbuf_set_addr(PACKETBUF_ADDR_ESENDER, &linkaddr_node_addr);
        packetbuf_set_attr(PACKETBUF_ATTR_MAC_SEQNO, packet_seqno);
        packetbuf_set_attr(PACKETBUF_ATTR_PACKET_TYPE, PACKET_TYPE_NORMAL);
        NETSTACK_RDC.send(aggregated_packet.sent, NULL);
        /*mac_call_sent_callback(OUTGOING_PTR(&aggregated_packet)->sent, OUTGOING_PTR()->ptr, status, transmissions);*/
    }

    if(aggregated_packet.qb) {
        queuebuf_free(aggregated_packet.qb);
    }

    struct mmem* packet = list_head(outgoing_packets);
    while(packet) {
        queuebuf_free(OUTGOING_PTR(packet)->qb);
        mmem_free(packet);
        list_pop(outgoing_packets); //TODO: Check that this does not break (popping before list_item_next())
        packet = list_item_next(packet);
    }
}

static void postslot_timer_callback(void* ptr) {
    //Setup dissem timer
    //TODO: Check the when is correct
    rtimer_clock_t when = RTIMER_NOW() + (DAS_NUM_SLOTS-slot-1)*DAS_SLOT_PERIOD_SEC*RTIMER_SECOND;
    rtimer_set(&dissem_timer, when, 0, dissem_timer_callback, NULL);
    slot_active = 0;
}
// }}}

// Other Functions {{{
static void handle_dissem_message() {
    //TODO: Handle dissem messages from packetbuf
}

static void handle_normal_message() {
    //Either forward message or if sink propagate to the application
    if(node_type == NODE_TYPE_SINK) {
        NETSTACK_NETWORK.input();
    }
    else {
        //TODO: Also check for repeated sequence numbers?
        packetbuf_compact();
        packetbuf_set_addr(PACKETBUF_ADDR_ESENDER, &linkaddr_node_addr);
        packetbuf_set_attr(PACKETBUF_ATTR_HOPS, packetbuf_attr(PACKETBUF_ATTR_HOPS) + 1);
        //TODO: Check if empty normal message?
        struct mmem* packet = NULL;
        mmem_alloc(packet, sizeof(DasOutgoingPacket));
        OUTGOING_PTR(packet)->qb = queuebuf_new_from_packetbuf();
        OUTGOING_PTR(packet)->sent = NULL;
        OUTGOING_PTR(packet)->ptr = NULL;
        list_add(outgoing_packets, packet);
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

static void packet_send(mac_callback_t sent, void *ptr) {
    struct mmem* packet = NULL;
    packetbuf_set_addr(PACKETBUF_ADDR_SENDER, &linkaddr_node_addr);
    packetbuf_set_attr(PACKETBUF_ATTR_HOPS, 0);
    if(!mmem_alloc(packet, sizeof(DasOutgoingPacket))) {
        PRINTF("DAS-mac: Could not allocate queued packet data structure. Dropping...\n");
        mac_call_sent_callback(sent, ptr, MAC_TX_ERR_FATAL, 1);
        return;
    }
    OUTGOING_PTR(packet)->qb = queuebuf_new_from_packetbuf();
    OUTGOING_PTR(packet)->sent = sent;
    OUTGOING_PTR(packet)->ptr = ptr;
    list_add(outgoing_packets, (void*)packet);
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
    packetbuf_compact();
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

/*static void packet_input(void)*/
/*{*/
    /*//TODO Filter between application level messages and TDMA messages*/
    /*if(NETSTACK_FRAMER.parse() < 0)*/
    /*{*/
        /*//Framer couldn't parse frame*/
        /*PRINTF("Failed to parse %u\n", packetbuf_datalen());*/
        /*return;*/
    /*}*/

    /*//TODO Check for TDMA messages*/
    /*if(packetbuf_attr(PACKETBUF_ATTR_PACKET_TYPE) == DISSEM_MESSAGE)*/
    /*{*/
        /*//TODO TDMA stuff*/
    /*}*/
    /*else if(packetbuf_attr(PACKETBUF_ATTR_PACKET_TYPE) == NORMAL_MESSAGE)*/
    /*{*/
        /* If node is sink, NETSTACK_NETWORK.input()
         * Else, add packet to outgoing packets (after setting addr, attrs, etc)
         */
        /*if(type == SINK_NODE)*/
        /*{*/
            /*NETSTACK_LLSEC.input();*/
        /*}*/
        /*else if(type == NORMAL_NODE)*/
        /*{*/
            /*//TODO Add packet to outgoing*/
            /* Copy packet data out of packetbuf
             * Clear packetbuf
             * Copy data into data section
             * Set header attrs and addr
             * Enqueue in packetqueue
             */
            /*
             *void* data_ptr = packetbuf_dataptr();
             *void* hdr_ptr = packetbuf_hdrptr();
             *[>uint16_t hdr_len = packetbuf_hdrlen();<] //Will be zero for incoming packets
             *memcpy(hdr_ptr, data_ptr, PACKETBUF_HDR_SIZE); //XXX This probably doesn't work
             *memcpy(data_ptr, ((uint8_t*)data_ptr)+PACKETBUF_HDR_SIZE, data_len-PACKETBUF_HDR_SIZE);
             */

            /*uint8_t temp_packet[PACKETBUF_SIZE];*/
            /*uint16_t data_len = packetbuf_datalen();*/
            /*packetbuf_copyto(&temp_packet);*/
            /*packetbuf_clear();*/
            /*packetbuf_copyfrom(&temp_packet, data_len);*/
            /*das_queue_outgoing_add(NULL, NULL);*/
            /*packetbuf_clear();*/
        /*}*/
        /*else if(type == SOURCE_NODE)*/
        /*{*/
            /*//Source node does nothing*/
        /*}*/
        /*else*/
        /*{*/
            /*//Unknown node type*/
        /*}*/
    /*}*/
    /*else*/
    /*{*/
        /*//Unknown packet type*/
    /*}*/

    /*//Not using LLSEC*/
    /*[>NETSTACK_LLSEC.input();<]*/
    /*[>NETSTACK_NETWORK.input();<]*/
/*}*/
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

    dissem_timer_callback(NULL);
    /*ctimer_set(&dissem_timer, DAS_PERIOD_SEC*CLOCK_SECOND, dissem_timer_callback, NULL);*/
    is_on = 1;
    return 0;
}
/*---------------------------------------------------------------------------*/
static int off(int keep_radio_on)
{
    PRINTF("DAS-mac: Turning off not implemented\n");
    return 1;
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
    list_init(outgoing_packets);

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
