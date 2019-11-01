#include "contiki.h"
#include "net/netstack.h"
#include "net/nullnet/nullnet.h"
#include "sys/log.h"
#include "os/dev/serial-line.h"
#include "arch/cpu/cc26x0-cc13x0/dev/cc26xx-uart.h"
#include "os/dev/button-hal.h"
#include "batmon-sensor.h"
#include "os/dev/leds.h"
#include <stdio.h>
#include <string.h>
#include "sys/ctimer.h"
#include "sys/etimer.h"
#include "random.h"

#include "structures.h"

#define LOG_MODULE "Sensor"
#define LOG_LEVEL LOG_LEVEL_DBG
//#define LOG_LEVEL LOG_LEVEL_INFO

//status list
#define STATUS_INACTIVE 0
#define STATUS_CONNECTING 1
#define STATUS_REGISTERED 2

#define SERIAL_STATUS_IDLE 0
#define SERIAL_STATUS_DEVICE 1
#define SERIAL_STATUS_SET 2

#define BLINKING_PERIOD 0.25
#define BEACON_PERIOD 1

struct mean{
	int samples;
	int value;
};

static int samplingPeriod = 2;
static int reportingPeriod = 9;
static int beaconMaxRetry = 5;

static struct mean valuesArray[4];
static int valueIndex = 0;

static unsigned int secret = 123456789;

static struct ctimer collectingTimer; //Collecting data
static struct ctimer reportingTimer; //Sending a msg. with data
static struct etimer beaconTimer; //Used in registration phase
static struct etimer ledTimer; //Led blinking timer
	
static linkaddr_t sinkAddress;
static volatile int status;
static int serialStatus;
static int serialDevice;//Which timer to update

PROCESS(main_process, "Main process");
PROCESS(ui_process, "UI process");
AUTOSTART_PROCESSES(&main_process, &ui_process);

//Compute the mean of a value with random samples
static void getValue(void *ptr){
	int newSample = random_rand() % 5;
	struct mean *pointer = (struct mean*)ptr;
	pointer->value = (pointer->value * pointer->samples + newSample) / (pointer->samples + 1);
	pointer->samples++;
}

//Add random value to battery value
static void getValueBat(void *ptr){
	int newSample = batmon_sensor.value(BATMON_SENSOR_TYPE_VOLT);
	struct mean *pointer = (struct mean*)ptr;
	pointer->value = (pointer->value * pointer->samples + newSample) / (pointer->samples + 1);
	pointer->samples++;
}

//Get the values for all sensors
static void getSamples(void *ptr){
	struct mean *valuesArray = (struct mean*)ptr;
	getValue(&valuesArray[0]);
	getValue(&valuesArray[1]);
	getValue(&valuesArray[2]);
	getValueBat(&valuesArray[3]);
	process_poll(&main_process);
}

//Reporting timer is expired, poll the process (do nothing here because local values are involved)
static void makeReport(void *ptr){
	process_poll(&main_process);
}

static void sendMessage(void *payload, int length, linkaddr_t *address){
	/*
	printf("len: %d\n", length);
	for(int i=0; i<length; i++){
		printf("%x\n", *(char *)(payload + i));
	}
	*/
	nullnet_buf = (uint8_t *)payload;
	nullnet_len = length;
	NETSTACK_NETWORK.output(address);
	LOG_DBG("Invio messaggio. len: %d\n", nullnet_len);
}

static void activateSensors(){//and set timers
	ctimer_set(&reportingTimer, CLOCK_SECOND * reportingPeriod, makeReport, NULL);
	ctimer_set(&collectingTimer, CLOCK_SECOND * samplingPeriod, getSamples, &valuesArray);
	SENSORS_ACTIVATE(batmon_sensor);
}

static void deactivateSensors(){//and stop timers
	ctimer_stop(&reportingTimer);
	ctimer_stop(&collectingTimer);
	SENSORS_DEACTIVATE(batmon_sensor);
}

//Build the structure for the transmission
static void buildMessage(void *ptr, void *outputBuffer){
	struct mean *valuesArray = (struct mean*)ptr;
	struct mess_sensor_node MSN = {secret, valuesArray[0].value, valuesArray[1].value, valuesArray[2].value, valuesArray[3].value};
	memcpy(outputBuffer, &MSN, sizeof(struct mess_sensor_node));
}

static void inputCallback(const void *data, uint16_t len, const linkaddr_t *src, const linkaddr_t *dest){
	/*
	LOG_DBG("Messaggio ricevuto\n");
	LOG_DBG("Sono %d %d %d %d %d %d %d %d\n", linkaddr_node_addr.u8[0], linkaddr_node_addr.u8[1], linkaddr_node_addr.u8[2], linkaddr_node_addr.u8[3], linkaddr_node_addr.u8[4], linkaddr_node_addr.u8[5], linkaddr_node_addr.u8[6], linkaddr_node_addr.u8[7]);
	LOG_DBG("Src %d %d %d %d %d %d %d %d\n", src->u8[0], src->u8[1], src->u8[2], src->u8[3], src->u8[4], src->u8[5], src->u8[6], src->u8[7]);
	LOG_DBG("Dest %d %d %d %d %d %d %d %d\n", dest->u8[0], dest->u8[1], dest->u8[2], dest->u8[3], dest->u8[4], dest->u8[5], dest->u8[6], dest->u8[7]);
	*/
	if(linkaddr_cmp(&linkaddr_node_addr, dest) && (status == STATUS_CONNECTING) && (len == 4) && (*(unsigned int*)data == secret)){
		sinkAddress = *src;
		status = STATUS_REGISTERED;
		etimer_stop(&beaconTimer);
		activateSensors();
		process_poll(&ui_process);
	}
}

PROCESS_THREAD(main_process, ev, data){
	static uint8_t outputBuffer[20];
	
	struct mess_registration beaconMessage;
	beaconMessage.secret = secret;
	beaconMessage.t = s_node;
	
	static int beaconActualRetry;
	
	PROCESS_BEGIN();
	cc26xx_uart_set_input(serial_line_input_byte);
	serial_line_init();
	status = STATUS_INACTIVE;
	serialStatus = SERIAL_STATUS_IDLE;
	serialDevice = -1;

	nullnet_set_input_callback(inputCallback);
	
	while(1){
		PROCESS_YIELD();
		LOG_DBG("Status: %d\n", status);
		if (ev == PROCESS_EVENT_TIMER){
			//Beacon timer is expired
			if(etimer_expired(&beaconTimer)){
				LOG_DBG("Beacon timer\n");
				if(status == STATUS_CONNECTING){
					if(beaconActualRetry < beaconMaxRetry){
						sendMessage(&beaconMessage, sizeof(beaconMessage), NULL);
						beaconActualRetry++;
						etimer_reset(&beaconTimer);
					}
					else{
						status = STATUS_INACTIVE;
						etimer_stop(&beaconTimer);
						process_poll(&ui_process);
					}
				}
			}
		}
		else if (ev == PROCESS_EVENT_POLL){
			//Reporting timer is expired
			if(ctimer_expired(&reportingTimer)){
				if(status == STATUS_REGISTERED){//reportingTimerStatus
					LOG_DBG("Reporting timer\n");
					buildMessage(&valuesArray, &outputBuffer);
					// DEBUG
					LOG_DBG("Temperature: %d\n", (int)((struct mean)valuesArray[0]).value);
					LOG_DBG("Humidity: %d\n", (unsigned int)((struct mean)valuesArray[1]).value);
					LOG_DBG("Luminance: %d\n", (int)((struct mean)valuesArray[2]).value);
					LOG_DBG("Voltage: %d\n", (int)((struct mean)valuesArray[3]).value);
					sendMessage(&outputBuffer, (sizeof(struct mess_sensor_node)), &sinkAddress);
				}
				ctimer_restart(&reportingTimer);
			}
			//Collecting timer is expired
			if(ctimer_expired(&collectingTimer)){
				LOG_DBG("Collecting timer\n");
				ctimer_reset(&collectingTimer);
			}
		}
		//Inputs from serial line. Used to set collecting and reporting time at runtime
		else if (ev == serial_line_event_message){
			if(serialStatus == SERIAL_STATUS_IDLE){
				if(strcmp(data, "r") == 0){
					serialStatus = SERIAL_STATUS_DEVICE;
					serialDevice = 0;
				}
				else if(strcmp(data, "s") == 0){
					serialStatus = SERIAL_STATUS_DEVICE;
					serialDevice = 1;
				}
				else{
					serialDevice = -1;
				}
			}
			
			if(serialStatus == SERIAL_STATUS_DEVICE){
				int tmp = 0;
				if(strcmp(data, "1") == 0){
					serialStatus = SERIAL_STATUS_IDLE;
					tmp = 1;
				}
				else if(strcmp(data, "2") == 0){
					serialStatus = SERIAL_STATUS_IDLE;
					tmp = 2;
				}
				else if(strcmp(data, "3") == 0){
					serialStatus = SERIAL_STATUS_IDLE;
					tmp = 3;
				}
				else if(strcmp(data, "4") == 0){
					serialStatus = SERIAL_STATUS_IDLE;
					tmp = 4;
				}
				else if(strcmp(data, "5") == 0){
					serialStatus = SERIAL_STATUS_IDLE;
					tmp = 5;
				}
				else if(strcmp(data, "6") == 0){
					serialStatus = SERIAL_STATUS_IDLE;
					tmp = 6;
				}
				else if(strcmp(data, "7") == 0){
					serialStatus = SERIAL_STATUS_IDLE;
					tmp = 7;
				}
				else if(strcmp(data, "8") == 0){
					serialStatus = SERIAL_STATUS_IDLE;
					tmp = 8;
				}
				else if(strcmp(data, "9") == 0){
					serialStatus = SERIAL_STATUS_IDLE;
					tmp = 9;
				}
				else if(strcmp(data, "c") == 0){
					serialStatus = SERIAL_STATUS_IDLE;
					serialDevice = -1;
				}
				
				if(tmp > 0){
					if(serialDevice == 0){
						reportingPeriod = tmp;
						printf("New reporting period: %d\n", reportingPeriod);
					}
					else if(serialDevice == 1){
						samplingPeriod = tmp;
						printf("New sampling period: %d\n", samplingPeriod);
					}
				}
				if(status == STATUS_REGISTERED){
					deactivateSensors();
					activateSensors();
				}
			}
			
			if(serialStatus == SERIAL_STATUS_IDLE){
				printf("Settings:\n");
				printf("\tPress \'r\' to set new reporting period\n");
				printf("\tPress \'s\' to set new sampling period\n");
				printf("\tPress \'c\' to cancel\n");
			}
			else if(serialStatus == SERIAL_STATUS_DEVICE){
				if(serialDevice == 0){
					printf("Insert new reporting period (from 1 to 9) or \'c\' to cancel:\n");
				}
				else if(serialDevice == 1){
					printf("Insert new sampling period (from 1 to 9) or \'c\' to cancel:\n");
				}
			}
		}
		else if(ev == button_hal_press_event){//button
			button_hal_button_t *btn = (button_hal_button_t *)data;
			if(btn->unique_id == BOARD_BUTTON_HAL_INDEX_KEY_LEFT){
				//If disconnected, tries to connect to the sink
				if(status == STATUS_INACTIVE){ //Start sending beacons
					status = STATUS_CONNECTING;
					process_poll(&ui_process);
					beaconActualRetry = 0;
					sendMessage(&beaconMessage, sizeof(beaconMessage), NULL);
					etimer_set(&beaconTimer, CLOCK_SECOND * BEACON_PERIOD);
				}
				//If connected, updates its sensors values with random
				if(status == STATUS_REGISTERED){ //Alerates sensor's values (FOR TESTING PURPOSES)
					if(valueIndex == 3)
						valuesArray[valueIndex].value -= 10;
					else
						valuesArray[valueIndex].value += 10;
					valueIndex++;
					if(valueIndex > 3)
						valueIndex = 0;
				}
			}
			//Disconnect the node from the sink
			if(btn->unique_id == BOARD_BUTTON_HAL_INDEX_KEY_RIGHT){
				status = STATUS_INACTIVE;
				process_poll(&ui_process);
				deactivateSensors();
			}
		}
	}
	PROCESS_END();
}

//Node il disconnected 		-> red ON
//Node is looking for sink 	-> green BLINKING
//Node is active 			-> both OFF
PROCESS_THREAD(ui_process, ev, data){ //Handles leds
	PROCESS_BEGIN();
	etimer_set(&ledTimer, CLOCK_SECOND * BLINKING_PERIOD);
	leds_on(LEDS_RED);
	leds_off(LEDS_GREEN);
	while(1){
		PROCESS_YIELD();
		if (ev == PROCESS_EVENT_POLL){
			if(status == STATUS_INACTIVE){
				leds_on(LEDS_RED);
				leds_off(LEDS_GREEN);
			}
			else if(status == STATUS_CONNECTING){
				leds_off(LEDS_RED);
				leds_on(LEDS_GREEN);
				etimer_reset(&ledTimer);
			}
			else{
				leds_off(LEDS_RED);
				leds_off(LEDS_GREEN);
			}
		}
		if (ev == PROCESS_EVENT_TIMER){
			if(etimer_expired(&ledTimer)){
				if(status == STATUS_CONNECTING){
					leds_toggle(LEDS_GREEN);
					etimer_reset(&ledTimer);
				}
			}
		}
	}
	PROCESS_END();
}
