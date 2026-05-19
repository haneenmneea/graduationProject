/*
 * Intermittent UDP Client — Energy Simulation for FIT IoT-LAB
 * =============================================================
 * الهدف: تشغيل تجربة حقيقية وجمع لوقز
 * اللوقز ستُستخدم لاحقاً لتدريب نموذج ML يتوقع متى يحفظ checkpoint
 *
 * ما يفعله الكود:
 *   1. Node تصحى بطاقة عشوائية (30-60%)
 *   2. تنتظر OFF time عشوائي (30-120s) ثم RPL settle (60s)
 *   3. كل 60s:
 *      - تخسر 1% idle
 *      - تكلفة إرسال متغيرة (3-8%) تحاكي جودة الإشارة
 *      - احتمال 33% تُشحن (0-3%) تحاكي حصاد الطاقة
 *   4. تطبع ENERGY_LEVEL في كل دورة  <- أهم سطر للـ ML
 *   5. عند 10% -> POWER FAILURE -> watchdog_reboot()
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
#define LOG_LEVEL  LOG_LEVEL_INFO

/*---------------------------------------------------------------------------*/
/* Configuration                                                             */
/*---------------------------------------------------------------------------*/

#define UDP_CLIENT_PORT      8765
#define UDP_SERVER_PORT      5678
#define SEND_INTERVAL        (60 * CLOCK_SECOND)

/* Timing */
#define MIN_OFF_TIME         (30  * CLOCK_SECOND)
#define MAX_OFF_TIME         (120 * CLOCK_SECOND)
#define RPL_SETTLE_TIME      (60  * CLOCK_SECOND)

/* Energy */
#define ENERGY_THRESHOLD     10
#define ENERGY_COST_IDLE      1
#define ENERGY_COST_SEND_MIN  3
#define ENERGY_COST_SEND_MAX  8
#define ENERGY_HARVEST_PROB  85   /* 85/255 ≈ 33% */
#define ENERGY_HARVEST_MAX    3
#define ENERGY_INIT_MIN      30
#define ENERGY_INIT_MAX      60

/*---------------------------------------------------------------------------*/
/* Globals                                                                   */
/*---------------------------------------------------------------------------*/

static struct simple_udp_connection udp_conn;
static uint8_t energy_level;

/*---------------------------------------------------------------------------*/
/* try_harvest                                                               */
/*---------------------------------------------------------------------------*/
static void
try_harvest(void)
{
  if((random_rand() & 0xFF) < ENERGY_HARVEST_PROB) {
    uint8_t h = random_rand() % (ENERGY_HARVEST_MAX + 1);
    if(h > 0) {
      energy_level = ((uint16_t)energy_level + h > 100)
                     ? 100 : energy_level + h;
    }
  }
}

/*---------------------------------------------------------------------------*/
/* update_energy                                                             */
/*---------------------------------------------------------------------------*/
static void
update_energy(uint8_t cost)
{
  energy_level = (energy_level >= cost) ? energy_level - cost : 0;

  try_harvest();

  LOG_INFO("ENERGY_LEVEL: %u\n", energy_level);

  if(energy_level < ENERGY_THRESHOLD) {
    LOG_INFO("### POWER FAILURE: energy=%u ###\n", energy_level);
    watchdog_reboot();
  }
}

/*---------------------------------------------------------------------------*/
PROCESS(boot_process,       "Boot");
PROCESS(udp_client_process, "Intermittent UDP client");
AUTOSTART_PROCESSES(&boot_process, &udp_client_process);

/*---------------------------------------------------------------------------*/
/* Boot Process                                                              */
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(boot_process, ev, data)
{
  static struct etimer t;
  clock_time_t off_time;

  PROCESS_BEGIN();

  energy_level = ENERGY_INIT_MIN +
                 (random_rand() % (ENERGY_INIT_MAX - ENERGY_INIT_MIN + 1));

  LOG_INFO("Node waking up — energy=%u%%\n", energy_level);

  off_time = MIN_OFF_TIME + (random_rand() % (MAX_OFF_TIME - MIN_OFF_TIME));
  LOG_INFO("DEAD for %lu s\n", (unsigned long)(off_time / CLOCK_SECOND));

  etimer_set(&t, off_time);
  PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&t));

  LOG_INFO("Waking up — waiting %u s for RPL\n",
           RPL_SETTLE_TIME / CLOCK_SECOND);

  etimer_set(&t, RPL_SETTLE_TIME);
  PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&t));

  LOG_INFO("Node joining network — energy=%u%%\n", energy_level);

  PROCESS_END();
}

/*---------------------------------------------------------------------------*/
/* UDP Client                                                                */
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(udp_client_process, ev, data)
{
  static struct etimer send_timer;
  static unsigned      seq_id;
  static char          payload[64];
  uip_ipaddr_t         dest_ipaddr;

  PROCESS_BEGIN();

  /* انتظر boot_process ينتهي قبل البدء */
  PROCESS_WAIT_UNTIL(!process_is_running(&boot_process));

  /* لا callback — one-way traffic فقط */
  simple_udp_register(&udp_conn, UDP_CLIENT_PORT, NULL,
                      UDP_SERVER_PORT, NULL);

  LOG_INFO("UDP client started — energy=%u%%\n", energy_level);

  etimer_set(&send_timer, random_rand() % SEND_INTERVAL);

  while(1) {
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&send_timer));

    /* خصم idle */
    update_energy(ENERGY_COST_IDLE);

    if(NETSTACK_ROUTING.node_is_reachable() &&
       NETSTACK_ROUTING.get_root_ipaddr(&dest_ipaddr)) {

      uint8_t send_cost = ENERGY_COST_SEND_MIN +
                          (random_rand() % (ENERGY_COST_SEND_MAX -
                                            ENERGY_COST_SEND_MIN + 1));

      snprintf(payload, sizeof(payload), "hello %u", seq_id);

      LOG_INFO("Sending request %u to ", seq_id);
      LOG_INFO_6ADDR(&dest_ipaddr);
      LOG_INFO_(" | energy=%u%%\n", energy_level);

      seq_id++;

      simple_udp_sendto(&udp_conn, payload, strlen(payload), &dest_ipaddr);

      /* خصم تكلفة الارسال */
      update_energy(send_cost);

    } else {
      LOG_INFO("Not reachable yet — energy=%u%%\n", energy_level);
    }

    etimer_set(&send_timer,
               SEND_INTERVAL - CLOCK_SECOND +
               (random_rand() % (2 * CLOCK_SECOND)));
  }

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
