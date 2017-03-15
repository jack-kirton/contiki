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
    #define DAS_NEIGHBOUR_DISCOVERY_PERIODS 2
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
    PACKET_TYPE_EMPTY_NORMAL,
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
    uint8_t normal;
    unsigned short id;
    unsigned short num_neighbours;
    NeighbourInfo neighbours[];
} NeighbourInfoMessage;

typedef struct Other {
    void* next;
    unsigned short id;
    LIST_STRUCT(ids);
} Other;
// }}}

// Function Prototypes {{{
// Timer Callbacks
static void dissem_sent_callback(void* ptr, int status, int transmissions);
static void dissem_send_timer_callback(struct rtimer* task, void* ptr);
static void dissem_timer_callback(void* ptr);
static void slot_timer_callback(void* ptr);
static void process_dissem();
static void process_collisions();
// }}}

// Global Variables {{{
static int is_init = 0;
static int is_on = 0;
static unsigned int period_count = 0;

static int parent;
static int slot;
static int hop;
static uint8_t normal = 1;

static volatile int slot_changed = 0;

LIST(my_n);
LIST(n_info);
LIST(n_par);
LIST(others);
LIST(children);
PACKETQUEUE(outgoing_packets, DAS_OUTGOING_QUEUE_SIZE);

static NodeType node_type;

static struct ctimer dissem_timer = {};
static struct rtimer dissem_send_timer = {};
static struct ctimer slot_timer = {};

uint8_t packet_seqno = 0;

static das_aggregation_callback aggregation_callback = NULL;
// }}}

// Helper Functions {{{
static int rank(list_t neighbour_list, unsigned short id) {
    Neighbour* el = list_head(neighbour_list);
    int i = 0;
    while(el) {
        if(el->id == id) {
            return i+1;
        }
        i++;
        el = list_item_next(el);
    }
    return BOTTOM;
}

static void add_neighbour_id(list_t list, unsigned short id) {
    Neighbour* el = list_head(list);
    while(el) {
        if(el->id == id) {
            return;
        }
        el = list_item_next(el);
    }

    Neighbour* new_neighbour = NULL;
    ASSERT(new_neighbour = malloc(sizeof(Neighbour)));
    new_neighbour->id = id;

    //Insert in sorted order
    el = list_head(list);
    if(!el) {
        list_add(list, new_neighbour);
        return;
    }
    else if(el->id > id) {
        list_push(list, new_neighbour);
        return;
    }
    Neighbour* el_next = list_head(list);
    el_next = list_item_next(el_next);
    while(el_next != NULL && el_next->id < id) {
        el = el_next;
        el_next = list_item_next(el_next);
    }
    list_insert(list, el, new_neighbour);
}

static void add_my_n(unsigned short id) {
    add_neighbour_id(my_n, id);
#if DAS_DEBUG
    Neighbour* el = NULL;
    el = list_head(my_n);
    PRINTF("my_n: ");
    while(el) {
        PRINTF("%u", el->id);
        el = list_item_next(el);
        if(el) PRINTF(", ");
    }
    PRINTF("\n");
#endif
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
                if(el->hop == BOTTOM || el->hop > info->hop) {
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

void add_n_par(unsigned short id) {
    Neighbour* potential_parent;
    ASSERT(potential_parent = malloc(sizeof(Neighbour)));
    potential_parent->id = id;
    list_add(n_par, potential_parent);
}

void add_other(NeighbourInfoMessage* message) {
    Other* other = list_head(others);
    while(other) {
        if(message->id == other->id) {
            break;
        }
        other = list_item_next(other);
    }

    if(!other) {
        ASSERT(other = malloc(sizeof(Other)));
        other->id = message->id;
        LIST_STRUCT_INIT(other, ids);
        list_add(others, other);
    }

    int i;
    for(i = 0; i < message->num_neighbours; i++) {
        add_neighbour_id(other->ids, message->neighbours[i].id);
    }
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

static Other* get_other(unsigned short id) {
    Other* el = list_head(others);
    while(el) {
        if(el->id == id) {
            return el;
        }
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
    DISSEM_PTR(&dissem_message)->normal = normal;
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
    assert(DISSEM_SIZE(&dissem_message) <= PACKETBUF_SIZE); //This will not catch all cases (assumes no header)
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
        ctimer_set(&dissem_timer, DAS_PERIOD_SEC*CLOCK_SECOND, dissem_timer_callback, NULL);
    }
    else {
        ctimer_reset(&dissem_timer);
    }

    PRINTF("DAS-mac: Dissem active\n");

    build_neighbourinfo_message();
    //Send dissem message at random in range of DAS_DISSEM_PERIOD
    rtimer_clock_t offset = (rtimer_clock_t)(((float)random_rand()/(float)RANDOM_RAND_MAX)*DAS_DISSEM_PERIOD_SEC*RTIMER_SECOND);
    rtimer_set(&dissem_send_timer, RTIMER_NOW() + offset, 0, dissem_send_timer_callback, NULL);

    if(period_count >= DAS_NEIGHBOUR_DISCOVERY_PERIODS) {
        process_dissem();
        process_collisions();
    }
    period_count++;
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

    if(packetqueue_len(&outgoing_packets) != 0) {
        mac_callback_t sent = NULL;
        void* ptr = NULL;
        packetbuf_clear();
        (*aggregation_callback)(&outgoing_packets, &sent, &ptr);
        packetbuf_set_addr(PACKETBUF_ADDR_ESENDER, &linkaddr_node_addr);
        packetbuf_set_attr(PACKETBUF_ATTR_MAC_SEQNO, packet_seqno);
        packetbuf_set_attr(PACKETBUF_ATTR_PACKET_TYPE, PACKET_TYPE_NORMAL);
        NETSTACK_RDC.send(sent, ptr);

        while(packetqueue_len(&outgoing_packets)) {
            packetqueue_dequeue(&outgoing_packets); //Dequeue also frees internal queuebuf
        }
    }
    else {
        //Send an empty normal message
        packetbuf_clear();
        packetbuf_set_addr(PACKETBUF_ADDR_ESENDER, &linkaddr_node_addr);
        packetbuf_set_attr(PACKETBUF_ATTR_PACKET_TYPE, PACKET_TYPE_EMPTY_NORMAL);
        NETSTACK_RDC.send(NULL, NULL);
    }
}
// }}}

// Process Things {{{
static void process_dissem() {
    if(slot == BOTTOM) {
        PRINTF("Processing dissem\n");
        int new_hop = BOTTOM;
        int new_parent = BOTTOM;
        int new_slot = BOTTOM;
        Neighbour* n = list_head(n_par);
        NeighbourInfo* parent_info = NULL;
        PRINTF("BEFORE LOOP\n");
        while(n) {
            PRINTF("LOOPING\n");
            NeighbourInfo* info = get_neighbourinfo(n->id);
            ASSERT(info);
            if(new_hop == BOTTOM || new_hop > info->hop) {
                new_hop = info->hop;
                new_parent = n->id;
                parent_info = info;
            }
            n = list_item_next(n);
        }
        PRINTF("Decided on parent and hop\n");
        //TODO: Make sure things aren't still NULL
        new_hop += 1;
        new_slot = parent_info->slot - rank(get_other(new_parent)->ids, new_parent) - 1; //TODO: Check get_other() doesn't return NULL
        PRINTF("par=%d, slot=%d, hop=%d\n", new_parent, new_slot, new_hop);
        update_slot_and_hop(new_slot, new_hop);
        update_parent(new_parent);

        n = list_head(my_n);
        while(n) {
            if(get_neighbourinfo(n->id)->slot == BOTTOM) { //TODO: Check that this isn't NULL
                add_neighbour_id(children, n->id);
            }
            n = list_item_next(n);
        }
    }
}

static void process_collisions() {
    return;
}
// }}}

// Message Handlers {{{
static void handle_dissem_message() {
    //TODO: packetbuf_dataptr() probably points to the header? Remove header in RDC or framer?
    NeighbourInfoMessage* message = packetbuf_dataptr();
    add_my_n(message->id);


    int i;
    if(message->normal) {
        NeighbourInfo* message_info = NULL;
        for(i = 0; i < message->num_neighbours; i++) {
            PRINTF("NeighbourInfo<id=%u,slot=%d,hop=%d>\n", message->neighbours[i].id, message->neighbours[i].slot, message->neighbours[i].hop);
            add_neighbour_info(&(message->neighbours[i]));
            if(message->neighbours[i].id == message->id) {
                message_info = &(message->neighbours[i]);
            }
        }
        if(slot == BOTTOM && message_info->slot != BOTTOM) {
            add_n_par(message_info->id);
            add_other(message);
        }
    }
    else {
        if(parent == message->id) {
            NeighbourInfo* message_info = NULL;
            for(i = 0; i < message->num_neighbours; i++) {
                PRINTF("NeighbourInfo<id=%u,slot=%d,hop=%d>\n", message->neighbours[i].id, message->neighbours[i].slot, message->neighbours[i].hop);
                add_neighbour_info(&(message->neighbours[i]));
                if(message->neighbours[i].id == message->id) {
                    message_info = &(message->neighbours[i]);
                }
            }

            if(slot >= message_info->slot) {
                update_slot(message_info->slot - 1);
                normal = 0;
            }
        }
    }
}

static void handle_normal_message() {
    //Either forward message or if sink propagate to the application
    if(node_type == NODE_TYPE_SINK) {
        NETSTACK_NETWORK.input();
    }
    else {
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
/*---------------------------------------------------------------------------*/
void das_set_aggregation_callback(das_aggregation_callback f) {
    aggregation_callback = f;
}
/*---------------------------------------------------------------------------*/
static void packet_send(mac_callback_t sent, void* ptr) {
    //TODO: Set more attributes
    packetbuf_set_addr(PACKETBUF_ADDR_ESENDER, &linkaddr_node_addr);
    packetbuf_set_attr(PACKETBUF_ATTR_HOPS, 1);
    packetqueue_enqueue_packetbuf(&outgoing_packets, 0, NULL);
    mac_call_sent_callback(sent, ptr, MAC_TX_DEFERRED, 0);
}
/*---------------------------------------------------------------------------*/
static void packet_input(void) {
    if(packetbuf_attr(PACKETBUF_ATTR_PACKET_TYPE) == PACKET_TYPE_DISSEM) {
        PRINTF("DAS-mac: Received dissem message\n");
        handle_dissem_message();
    }
    else if(packetbuf_attr(PACKETBUF_ATTR_PACKET_TYPE) == PACKET_TYPE_NORMAL) {
        PRINTF("DAS-mac: Received normal message\n");
        handle_normal_message();
    }
    else if(packetbuf_attr(PACKETBUF_ATTR_PACKET_TYPE) == PACKET_TYPE_EMPTY_NORMAL) {
        PRINTF("DAS-mac: Received empty normal message\n");
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

    if(aggregation_callback == NULL) {
        PRINTF("DAS-mac: Aggregation callback must be set before starting!\n");
        return 0;
    }

    parent = BOTTOM;
    slot = BOTTOM;
    hop = BOTTOM;

    //TODO:Clear these lists, init will not work
    /*list_init(my_n);*/
    /*list_init(n_info);*/
    /*list_init(outgoing_packets);*/

    add_my_n(node_id);

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
    list_init(n_par);
    list_init(others);
    list_init(children);
    packetqueue_init(&outgoing_packets);

    mmem_init();
    random_init(node_id);
    //XXX: NEVER INIT THESE HERE, IT BREAKS ALL TIMERS
    /*clock_init();*/
    /*ctimer_init();*/
    /*rtimer_init();*/

    //TODO: Possibly move this to RDC
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
