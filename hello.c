#include "contiki.h"
#include <stdio.h>
#include "sys/etimer.h"
#include "dev/light-sensor.h"
#include "dev/pressure-sensor.h"

PROCESS(sensor_data_collection, "Sensor Data Collection");
AUTOSTART_PROCESSES(&sensor_data_collection);

PROCESS_THREAD(sensor_data_collection, ev, data)
{
  static struct etimer timer;
  static uint32_t timestamp_s = 0;
  static uint32_t timestamp_us = 0;

  PROCESS_BEGIN();

  SENSORS_ACTIVATE(light_sensor);
  SENSORS_ACTIVATE(pressure_sensor);


  printf("timestamp_s,timestamp_us,light,pressure\n");

  while(1) {
    etimer_set(&timer, CLOCK_SECOND);
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&timer));

    timestamp_s++;
    timestamp_us = clock_time();

    int light = light_sensor.value(0);
    int pressure = pressure_sensor.value(0);




    printf("%lu,%lu,%d,%d,\n",
           timestamp_s,
           timestamp_us,
           light,
           pressure);
  }

  PROCESS_END();
}
