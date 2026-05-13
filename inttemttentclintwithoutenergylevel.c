/*
 * Intermittent UDP Client — FIXED VERSION
 * ========================================
 * Changes from original:
 *   1. Uses RPL auto-discovery (no hardcoded server IP)
 *   2. Checks node_is_reachable() before sending
 *   3. Waits for RPL to settle after reboot
 *   4. Adds jitter to prevent collisions
 *   5. Uses LOG_INFO for proper logging
 *   6. One-way only (no rx callback)
 *   7. Logs "from intermittent node" for easy identification
 */

#include "contiki.h"
#include "sys/etimer.h"
#include "dev/watchdog.h"
#include "lib/random.h"
#include "net/routing/routing.h"
#include "net/netstack.h"
#include "net/ipv6/simple-udp.h"

#include "sys/log.h"
#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO

/*---------------------------------------------------------------------------*/
/* Configuration                                                             */
/*---------------------------------------------------------------------------*/

#define UDP_CLIENT_PORT   8765
#define UDP_SERVER_PORT   5678

#define SEND_INTERVAL     (60 * CLOCK_SECOND)
#define POWER_FAIL_PROB   40   /* 40/255 ≈ 15% chance */

#define MIN_OFF_TIME      (30 * CLOCK_SECOND)
#define MAX_OFF_TIME      (120 * CLOCK_SECOND)

#define RPL_SETTLE_TIME   (60 * CLOCK_SECOND)

/*---------------------------------------------------------------------------*/
/* Globals                                                                   */
/*---------------------------------------------------------------------------*/

static struct simple_udp_connection udp_conn;

/*---------------------------------------------------------------------------*/
PROCESS(boot_process,       "Boot process");
PROCESS(udp_client_process, "Intermittent UDP client");
AUTOSTART_PROCESSES(&boot_process, &udp_client_process);

/*---------------------------------------------------------------------------*/
/* Boot Process — DEAD time + RPL settle                                     */
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(boot_process, ev, data)
{
  static struct etimer off_timer;
  static struct etimer settle_timer;
  clock_time_t off_time;

  PROCESS_BEGIN();

  off_time = MIN_OFF_TIME +
             (random_rand() % (MAX_OFF_TIME - MIN_OFF_TIME));

  LOG_INFO("Phase 1: DEAD for %lu seconds\n",
           (unsigned long)(off_time / CLOCK_SECOND));

  etimer_set(&off_timer, off_time);
  PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&off_timer));

  LOG_INFO("Phase 2: Waking up — waiting %u seconds for RPL\n",
           RPL_SETTLE_TIME / CLOCK_SECOND);

  etimer_set(&settle_timer, RPL_SETTLE_TIME);
  PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&settle_timer));

  LOG_INFO("Phase 3: RPL settled — ready to send\n");

  PROCESS_END();
}

/*---------------------------------------------------------------------------*/
/* UDP Client — sends only when RPL is ready                                 */
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(udp_client_process, ev, data)
{
  static struct etimer send_timer;
  static unsigned seq_id;
  static char payload[64];
  uip_ipaddr_t dest_ipaddr;

  PROCESS_BEGIN();

  PROCESS_WAIT_UNTIL(!process_is_running(&boot_process));

  simple_udp_register(&udp_conn,
                      UDP_CLIENT_PORT,
                      NULL,
                      UDP_SERVER_PORT,
                      NULL);

  LOG_INFO("Intermittent client started\n");

  etimer_set(&send_timer, random_rand() % SEND_INTERVAL);

  while(1) {
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&send_timer));

    if(NETSTACK_ROUTING.node_is_reachable() &&
       NETSTACK_ROUTING.get_root_ipaddr(&dest_ipaddr)) {

      /* ← التعديل هنا: نضيف "from intermittent node" في نهاية السطر */
      LOG_INFO("Sending request %u to ", seq_id);
      LOG_INFO_6ADDR(&dest_ipaddr);
      LOG_INFO_(" from intermittent node\n");

      snprintf(payload, sizeof(payload),
               "hello %u", seq_id++);

      simple_udp_sendto(&udp_conn, payload, strlen(payload), &dest_ipaddr);

      if((random_rand() & 0xFF) < POWER_FAIL_PROB) {
        LOG_INFO("### POWER FAILURE: rebooting node ###\n");
        watchdog_reboot();
      }

    } else {
      LOG_INFO("Not reachable yet\n");
    }

    etimer_set(&send_timer, SEND_INTERVAL
      - CLOCK_SECOND + (random_rand() % (2 * CLOCK_SECOND)));
  }

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
