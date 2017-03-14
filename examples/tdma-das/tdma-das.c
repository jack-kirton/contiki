#include "contiki.h"
#include "net/netstack.h"
#include "net/mac/das/das-mac.h"
#include "lib/mmem.h"
#include "sys/etimer.h"
#include "sys/ctimer.h"
#include "sys/clock.h"
#include "sys/node-id.h"

#include <stdio.h> /* For printf() */
#include <assert.h>
/*---------------------------------------------------------------------------*/
PROCESS(das_test_process, "DAS test process");
AUTOSTART_PROCESSES(&das_test_process);

/*static struct ctimer test_timer;*/

/*void test_timer_callback(void* ptr) {*/
    /*ctimer_reset(ptr);*/
    /*printf("TEST CTIMER WORKED\n");*/
/*}*/


/*---------------------------------------------------------------------------*/
PROCESS_THREAD(das_test_process, ev, data)
{
    PROCESS_BEGIN();

    if(node_id == 1) {
        das_set_sink(1);
    }
    else {
        das_set_sink(0);
    }

    NETSTACK_MAC.on();

    /*while(1) {*/
        /*printf("%lus\n", clock_seconds());*/
        /*[>clock_wait(CLOCK_SECOND);<]*/
        /*PROCESS_PAUSE();*/
    /*}*/

    /*struct etimer test_timer = {};*/

    /*etimer_set(&test_timer, CLOCK_SECOND);*/

    /*while(1) {*/
        /*printf("Waiting for etimer...\n");*/
        /*if(etimer_expired(&test_timer)) {*/
            /*printf("ETIMER FIRED\n");*/
            /*etimer_reset(&test_timer);*/
        /*}*/
        /*else {*/
            /*PROCESS_PAUSE();*/
        /*}*/
        /*PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&test_timer));*/
        /*printf("ETIMER FIRED\n");*/
        /*etimer_reset(&test_timer);*/
    /*}*/

    /*struct ctimer test_timer = {};*/

    /*printf("Setting test ctimer...\n");*/
    /*ctimer_set(&test_timer, 5*CLOCK_SECOND, test_timer_callback, &test_timer);*/
    /*while(1) {*/
        /*PROCESS_YIELD();*/
    /*}*/

    /*volatile unsigned long count = 0;*/
    /*while(1) {*/
        /*count++;*/
        /*if(count == 0) {*/
            /*printf("Count reset\n");*/
        /*}*/
        /*PROCESS_YIELD();*/
    /*}*/

    PROCESS_END();
}
/*---------------------------------------------------------------------------*/
