#include "contiki.h"
#include "sys/etimer.h"
#include "dev/watchdog.h"
#include "random.h"
 
#include "net/ipv6/simple-udp.h"
#include "net/ipv6/uiplib.h"
 
#include <stdio.h>
#include <string.h>
 
/*---------------------------------------------------------------------------*/
/* Configuration                                                             */
/*---------------------------------------------------------------------------*/
 
/* UDP */
#define UDP_CLIENT_PORT  8765
#define UDP_SERVER_PORT  5678
 
/* Server IPv6 address (CHANGE to your server address) */
#define SERVER_IP_ADDR "fd00::9480"
 
/* Intermittency parameters */
#define POWER_FAIL_PROB   40   /* Probability (0–255) */
#define SEND_INTERVAL     (60 * CLOCK_SECOND)
 
/* OFF (dead) time after reboot */
#define MIN_OFF_TIME      (20 * CLOCK_SECOND)
#define MAX_OFF_TIME      (90 * CLOCK_SECOND)
 
/*---------------------------------------------------------------------------*/
/* Globals                                                                    */
/*---------------------------------------------------------------------------*/
 
static struct simple_udp_connection udp_conn;
static uip_ipaddr_t server_ipaddr;
 
/*---------------------------------------------------------------------------*/
/* Emulated Power Failure                                                     */
/*---------------------------------------------------------------------------*/
static void
maybe_emulate_power_failure(void)
{
  if((random_rand() & 0xFF) < POWER_FAIL_PROB) {
    printf("### EMULATED POWER FAILURE: rebooting node ###\n");
    watchdog_reboot();   /* HARD reset – no communication afterward */
  }
}
 
/*---------------------------------------------------------------------------*/
/* Boot-Time Offline (Dead) Period                                            */
/*---------------------------------------------------------------------------*/
PROCESS(offline_boot_process, "Offline boot delay");
PROCESS(udp_client_process, "Intermittent UDP client");
AUTOSTART_PROCESSES(&offline_boot_process, &udp_client_process);
 
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(offline_boot_process, ev, data)
{
  static struct etimer off_timer;
  clock_time_t off_time;
 
  PROCESS_BEGIN();
 
  /* Random OFF duration */
  off_time = MIN_OFF_TIME +
            (random_rand() % (MAX_OFF_TIME - MIN_OFF_TIME));
 
  printf("Node OFF (emulated) for %lu ticks\n", (unsigned long)off_time);
  printf("No communication during this period\n");
 
  /* During this timer:
   * - Network stack not used
   * - No radio
   * - No UDP, RPL, or MAC traffic
   */
  etimer_set(&off_timer, off_time);
  PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&off_timer));
 
  printf("Node waking up and joining network\n");
 
  PROCESS_END();
}
 
/*---------------------------------------------------------------------------*/
/* UDP Receive Callback                                                       */
/*---------------------------------------------------------------------------*/
static void
udp_rx_callback(struct simple_udp_connection *c,
                const uip_ipaddr_t *sender_addr,
                uint16_t sender_port,
                const uip_ipaddr_t *receiver_addr,
                uint16_t receiver_port,
                const uint8_t *data,
                uint16_t datalen)
{
  printf("Received response '%.*s'\n", datalen, (char *)data);
}
 
/*---------------------------------------------------------------------------*/
/* UDP Client Process                                                         */
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(udp_client_process, ev, data)
{
  static struct etimer send_timer;
  static unsigned seq_id;
  char payload[64];
 
  PROCESS_BEGIN();
 
  /* Setup server address */
  if(!uiplib_ipaddrconv(SERVER_IP_ADDR, &server_ipaddr)) {
    printf("Invalid server IP address\n");
    PROCESS_EXIT();
  }
 
  /* Register UDP connection */
  simple_udp_register(&udp_conn,
                      UDP_CLIENT_PORT,
                      NULL,
                      UDP_SERVER_PORT,
                      udp_rx_callback);
 
  printf("UDP client started\n");
 
  etimer_set(&send_timer, SEND_INTERVAL);
 
  while(1) {
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&send_timer));
 
    snprintf(payload, sizeof(payload),
             " hello %u from intermittent node", seq_id++);
 
    printf("Sending request: %s\n", payload);
 
    simple_udp_sendto(&udp_conn,
                      payload,
                      strlen(payload),
                      &server_ipaddr);
 
    /* Possible power loss right after communication */
    maybe_emulate_power_failure();
 
    etimer_reset(&send_timer);
  }
 
  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
