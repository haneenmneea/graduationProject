/*
 * Intermittent UDP Client + ML-Predicted Checkpointing
 * =====================================================
 * Extends the original intermittent client with:
 *   1) On boot: try to restore counters from external NOR Flash
 *   2) Each cycle: run TinyML inference (emlearn) on energy features
 *   3) When the model predicts an imminent power drop -> save to NVM
 *   4) Emergency save as a fallback if ML missed the drop
 *   5) After watchdog_reboot(), continue from last saved seq_id
 *
 * Counters only (seq_id, total_tx, ...) are saved.
 * No retransmission buffer is used here, so server-side PDR stays valid:
 * (received / total_tx) <= 100%.
 */

#include "contiki.h"
#include "sys/etimer.h"
#include "dev/watchdog.h"
#include "dev/xmem.h"          /* External NOR flash driver */
#include "lib/random.h"
#include "net/routing/routing.h"
#include "net/netstack.h"
#include "net/ipv6/simple-udp.h"
#include "sys/log.h"
#include "models.h"            /* emlearn-generated ML models */

#include <string.h>

#define LOG_MODULE "App"
#define LOG_LEVEL  LOG_LEVEL_INFO

/*---------------------------------------------------------------------------*/
/* Configuration (same as original)                                          */
/*---------------------------------------------------------------------------*/
#define UDP_CLIENT_PORT      8765
#define UDP_SERVER_PORT      5678
#define SEND_INTERVAL        (60 * CLOCK_SECOND)
#define MIN_OFF_TIME         (30  * CLOCK_SECOND)
#define MAX_OFF_TIME         (120 * CLOCK_SECOND)
#define RPL_SETTLE_TIME      (60  * CLOCK_SECOND)
#define ENERGY_THRESHOLD     10
#define ENERGY_COST_IDLE      1
#define ENERGY_COST_SEND_MIN  3
#define ENERGY_COST_SEND_MAX  8
#define ENERGY_HARVEST_PROB  85
#define ENERGY_HARVEST_MAX    3
#define ENERGY_INIT_MIN      30
#define ENERGY_INIT_MAX      60

/* Checkpoint storage on external NOR Flash */
#define CHECKPOINT_MAGIC     0xABCD1234
#define CHECKPOINT_ADDR      0x00000000

/*---------------------------------------------------------------------------*/
/* Persistent Checkpoint Structure                                          */
/*---------------------------------------------------------------------------*/
typedef struct {
  uint32_t seq_id;
  uint32_t total_tx;
  uint32_t total_wakeups;
  uint32_t total_failures;
  uint8_t  last_energy;
  uint8_t  pad[3];
} ckpt_data_t;

/*---------------------------------------------------------------------------*/
/* Globals                                                                   */
/*---------------------------------------------------------------------------*/
static struct simple_udp_connection udp_conn;
static uint8_t energy_level;

static uint32_t seq_id          = 0;
static uint32_t total_tx        = 0;
static uint32_t total_wakeups   = 0;
static uint32_t total_failures  = 0;
static uint32_t total_notreach  = 0;
static uint8_t  cp_saved        = 0;
static uint8_t  recovered       = 0;
static clock_time_t session_start;

/* ML feature tracking */
static float e_start       = 0.0f;
static float avg_drop      = 0.0f;
static float prev_energy   = -1.0f;
static int   reading_count = 0;

/*---------------------------------------------------------------------------*/
/* Save state to external NOR Flash.                                        */
/* NOR flash requires erase-before-write. The magic number is written LAST  */
/* so a power failure mid-write leaves an invalid checkpoint (magic stays   */
/* 0xFFFFFFFF), and restore_checkpoint() rejects it on next boot.           */
/*---------------------------------------------------------------------------*/
static int
save_checkpoint(void)
{
  ckpt_data_t data;
  uint32_t magic = CHECKPOINT_MAGIC;

  data.seq_id         = seq_id;
  data.total_tx       = total_tx;
  data.total_wakeups  = total_wakeups;
  data.total_failures = total_failures;
  data.last_energy    = energy_level;
  memset(data.pad, 0, sizeof(data.pad));

  xmem_erase(XMEM_ERASE_UNIT_SIZE, CHECKPOINT_ADDR);
  xmem_pwrite(&data, sizeof(data),
              CHECKPOINT_ADDR + sizeof(uint32_t));
  xmem_pwrite(&magic, sizeof(magic), CHECKPOINT_ADDR);

  LOG_INFO("### CHECKPOINT_SAVED: seq=%lu energy=%u tx=%lu ###\n",
           (unsigned long)seq_id, energy_level, (unsigned long)total_tx);
  return 0;
}

/*---------------------------------------------------------------------------*/
static int
restore_checkpoint(void)
{
  uint32_t    magic;
  ckpt_data_t data;

  xmem_pread(&magic, sizeof(magic), CHECKPOINT_ADDR);

  if(magic != CHECKPOINT_MAGIC) {
    LOG_INFO("Checkpoint: no valid data (magic=0x%08lX)\n",
             (unsigned long)magic);
    return -1;
  }

  xmem_pread(&data, sizeof(data),
             CHECKPOINT_ADDR + sizeof(uint32_t));

  seq_id         = data.seq_id;
  total_tx       = data.total_tx;
  total_wakeups  = data.total_wakeups;
  total_failures = data.total_failures;

  LOG_INFO("### CHECKPOINT_RESTORED: seq=%lu tx=%lu wakeups=%lu ###\n",
           (unsigned long)seq_id,
           (unsigned long)total_tx,
           (unsigned long)total_wakeups);
  return 0;
}

/*---------------------------------------------------------------------------*/
/* Two-stage TinyML inference:                                              */
/*   Stage 1: node_classifier — is this an intermittent node?               */
/*   Stage 2: energy_nn       — probability of imminent power loss          */
/* If Stage 2 prob > 0.5 -> save checkpoint.                                */
/*---------------------------------------------------------------------------*/
static void
check_checkpoint(void)
{
  float time_s = (float)(clock_time() - session_start) / CLOCK_SECOND;
  float hours  = (time_s < 36.0f) ? 0.01f : time_s / 3600.0f;

  float fails_h = (float)total_failures / hours;
  float wakes_h = (float)total_wakeups  / hours;
  float tx_h    = (float)total_tx       / hours;
  float reach_h = (float)total_notreach / hours;

  float clf_feat[4] = {fails_h, wakes_h, tx_h, reach_h};
  float is_inter    = predict_node_type(clf_feat);

  if(is_inter < 0.5f) return;   /* stable node, skip stage 2 */

  float energy = (float)energy_level;
  if(e_start == 0.0f) e_start = energy;
  float e_lost = e_start - energy;
  if(prev_energy >= 0.0f)
    avg_drop = (avg_drop + (prev_energy - energy)) / 2.0f;
  prev_energy = energy;
  reading_count++;

  float eng_feat[6] = {energy, avg_drop, e_start,
                       e_lost, time_s, (float)reading_count};
  float prob = predict_checkpoint(eng_feat);

  LOG_INFO("ML: inter=%.2f prob=%.2f energy=%u\n",
           is_inter, prob, energy_level);

  if(prob > 0.5f && !cp_saved) {
    save_checkpoint();
    cp_saved = 1;
  }
}

/*---------------------------------------------------------------------------*/
/* Energy simulation (same as original)                                      */
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

static void
update_energy(uint8_t cost)
{
  energy_level = (energy_level >= cost) ? energy_level - cost : 0;
  try_harvest();
  LOG_INFO("ENERGY_LEVEL: %u\n", energy_level);

  check_checkpoint();   /* run ML each cycle */

  if(energy_level < ENERGY_THRESHOLD) {
    total_failures++;
    LOG_INFO("### POWER FAILURE: energy=%u ###\n", energy_level);

    /* Emergency save if ML missed it */
    if(!cp_saved) {
      LOG_INFO("Emergency checkpoint before reboot\n");
      save_checkpoint();
      cp_saved = 1;
    }
    watchdog_reboot();
  }
}

/*---------------------------------------------------------------------------*/
PROCESS(boot_process,       "Boot");
PROCESS(udp_client_process, "Intermittent UDP client + ML checkpoint");
AUTOSTART_PROCESSES(&boot_process, &udp_client_process);

/*---------------------------------------------------------------------------*/
PROCESS_THREAD(boot_process, ev, data)
{
  static struct etimer t;
  clock_time_t off_time;

  PROCESS_BEGIN();

  /* === Initialize external NOR flash === */
  xmem_init();
  LOG_INFO("External flash initialized (N25Q128A13E1240F)\n");

  /* === Try to restore checkpoint === */
  if(restore_checkpoint() == 0) {
    recovered = 1;
    LOG_INFO("RECOVERY: continuing from seq=%lu\n",
             (unsigned long)seq_id);
  } else {
    recovered = 0;
    seq_id = 0;
    total_tx = 0;
    total_failures = 0;
    total_wakeups = 0;
  }

  total_wakeups++;
  e_start = 0.0f;
  avg_drop = 0.0f;
  prev_energy = -1.0f;
  reading_count = 0;
  cp_saved = 0;
  session_start = clock_time();

  energy_level = ENERGY_INIT_MIN +
                 (random_rand() % (ENERGY_INIT_MAX - ENERGY_INIT_MIN + 1));

  LOG_INFO("Node waking up — energy=%u%% %s\n",
           energy_level,
           recovered ? "[RECOVERED]" : "[FRESH START]");

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
PROCESS_THREAD(udp_client_process, ev, data)
{
  static struct etimer send_timer;
  static char          payload[64];
  uip_ipaddr_t         dest_ipaddr;

  PROCESS_BEGIN();
  PROCESS_WAIT_UNTIL(!process_is_running(&boot_process));

  simple_udp_register(&udp_conn, UDP_CLIENT_PORT, NULL,
                      UDP_SERVER_PORT, NULL);

  LOG_INFO("UDP client started — energy=%u%% seq_start=%lu\n",
           energy_level, (unsigned long)seq_id);

  etimer_set(&send_timer, random_rand() % SEND_INTERVAL);

  while(1) {
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&send_timer));

    update_energy(ENERGY_COST_IDLE);

    if(NETSTACK_ROUTING.node_is_reachable() &&
       NETSTACK_ROUTING.get_root_ipaddr(&dest_ipaddr)) {

      uint8_t send_cost = ENERGY_COST_SEND_MIN +
                          (random_rand() % (ENERGY_COST_SEND_MAX -
                                            ENERGY_COST_SEND_MIN + 1));

      snprintf(payload, sizeof(payload), "hello %lu",
               (unsigned long)seq_id);

      LOG_INFO("Sending request %lu to ", (unsigned long)seq_id);
      LOG_INFO_6ADDR(&dest_ipaddr);
      LOG_INFO_(" from intermittent node | energy=%u%%\n", energy_level);

      seq_id++;
      total_tx++;
      simple_udp_sendto(&udp_conn, payload, strlen(payload), &dest_ipaddr);
      update_energy(send_cost);

    } else {
      LOG_INFO("Not reachable yet — energy=%u%%\n", energy_level);
      total_notreach++;
    }

    etimer_set(&send_timer,
               SEND_INTERVAL - CLOCK_SECOND +
               (random_rand() % (2 * CLOCK_SECOND)));
  }

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
