#include "contiki.h" 
// libreria per poter inviare
#include "net/netstack.h"
// libreria che implementa il link-layer 
#include "net/nullnet/nullnet.h" 
#include <string.h> 
#include <stdio.h> 
#include "sys/log.h" 
#include "sys/clock.h" 

#include "os/dev/serial-line.h"
#include "arch/cpu/cc26x0-cc13x0/dev/cc26xx-uart.h"

// contiene tutte le funzioni e le var per manipolare l'indirizzo del link-layer
#include "os/net/linkaddr.h"
#include "structures.h"


// Log for our Application, I can't downgrade this log at runtime
#define LOG_MODULE "Sink" 
#define LOG_LEVEL LOG_LEVEL_DBG
/*
#ifndef PROJECT_CONF_H_ 
#define PROJECT_CONF_H_
#define LOG_CONF_LEVEL_MAC LOG_LEVEL_DBG 
#endif
*/

#define MAX_SENSOR_NODES 1
// ange dei vari valori dei sensori
#define TEMP_RANGE 1
#define HUMIDITY_RANGE 1
#define LIGHT_RANGE 1

// timer
#define TIMER_PERIOD 15 // (in secondi) DEVE essere maggiore dell'inactive period minore, è inutile controllare prima
#define INACTIVE_PERIOD_SN 15
#define INACTIVE_PERIOD_ACT 15

// broken sensors
#define broken_temp_sensor_up 20
#define broken_temp_sensor_down -20
#define broken_humidity_sensor_up 20
#define broken_humidity_sensor_down 0
#define broken_light_sensor_up 20
#define broken_light_sensor_down -20

// tresholds
#define temperature_treshold 10
#define humidity_treshold 10
#define light_treshold 10
#define battery_treshold 800

static unsigned int secret = 123456789;

// parameters for registered nides
static struct sensor_node sensor_nodes[MAX_SENSOR_NODES];
static unsigned int sn_registered = 0;
static struct actuator_node actuator;
static bool actuator_registered;

// parameters for saving the data coming from nodes
static struct mess_to_actuator previous_mess_actuator;
static struct mess_sensor_node data_rcv;
static struct mess_registration mess_reg;
static struct actuator_status mess_act;

PROCESS(sink_process, "sink_process"); 
AUTOSTART_PROCESSES(&sink_process); 

// Returns the sensor node's index. -1 if it doesn't exist
static int find_sensor_node(const linkaddr_t *node) {
	for (int i = 0; i < sn_registered; i++) {
		if(linkaddr_cmp(&sensor_nodes[i].addr,node)!=0)
			return i;
	}
	return -1;
}

// Update the actuator's timer
static void update_timer_actuator() {
	actuator.time = clock_seconds();
}

// Update a specific sensor node's timer
static void update_timer_sn(const linkaddr_t *node) {
	int i = find_sensor_node(node);
	if( i != -1)
		sensor_nodes[i].time = clock_seconds();	
}

// Show the messages coming from the actuator
static void log_mess_actuator() {
	switch(mess_act.status) {
		case windows_broken :
			LOG_WARN("TIMESTAMP: %lu. Broken windows. A technician is required\n",clock_seconds());
			break;
		case lights_broken:
			LOG_WARN("TIMESTAMP: %lu. Broken lights. A technician is required\n",clock_seconds());
			break;
		case irrigation_broken:
			LOG_WARN("TIMESTAMP: %lu. Broken irrigation. A technician is required\n",clock_seconds());
			break;
		case windows_ok:
			LOG_WARN("TIMESTAMP: %lu. Repaired Windows\n",clock_seconds());
			break;
		case lights_ok:
			LOG_WARN("TIMESTAMP: %lu. Repaired Lights\n",clock_seconds());
			break;
		case irrigation_ok:
			LOG_WARN("TIMESTAMP: %lu. Repaired Irrigation\n",clock_seconds());
			break;
	}
}
 
/*
	Show the error messages:
	sensor_node = 1 -> No activity detected from a specifi sensor node
	sensor_node = 0 -> No activity detected from the actuator
*/
static void log_inactive_node(bool sensor_node, const linkaddr_t *node) {
	if(sensor_node) {	// sensor_node
		LOG_WARN("TIMESTAMP: %lu. No activity detected by the sensor node: %d%d", clock_seconds(),node->u8[6],node->u8[7]);
		LOG_WARN_(" for more than %d seconds\n",INACTIVE_PERIOD_SN);
	}
	else {	// actuator
		LOG_WARN("TIMESTAMP: %lu. No activity detected by the", clock_seconds());
		LOG_WARN_(" actuator for more than %d seconds\n",INACTIVE_PERIOD_ACT);
	}
}

/*
	Is called when a message arrives from a sensor node.
	Shows also if a specific sensor measure an anomalous value.
	error: 
		0 -> no error, 
		1 -> temp sensor, 
		2 -> humidity sensor, 
		3 -> light sensor, 
		4 -> change the battery
*/
static void log_mess_sensors(const linkaddr_t *node, unsigned int error) {
	if(error == 0) {
		LOG_INFO("TIMESTAMP: %lu. Received data: temperature: \"%d\" humidity: \"%u\"",clock_seconds(),data_rcv.temperature,data_rcv.humidity);
		LOG_INFO_(" light: \"%d\" and battery: \"%d\" from the sensor node \"%d%d\" \n",data_rcv.light,data_rcv.mVolt, node->u8[6],node->u8[7]);
	}
	else {
		switch(error) {
			case 1 :
				LOG_WARN("TIMESTAMP: %lu. Temperature sensor may be broken. A technician is required",clock_seconds());
				LOG_WARN_(" for sensor node \"%d%d\" \n",node->u8[6],node->u8[7]);
				break;	
			case 2:
				LOG_WARN("TIMESTAMP: %lu. Humidity sensor may be broken. A technician is required",clock_seconds());
				LOG_WARN_(" for sensor node \"%d%d\" \n",node->u8[6],node->u8[7]);
				break;
			case 3:
				LOG_WARN("TIMESTAMP: %lu. Light sensor may be broken. A technician is required",clock_seconds());
				LOG_WARN_(" for sensor node \"%d%d\" \n",node->u8[6],node->u8[7]);
				break;
			case 4:
				LOG_WARN("TIMESTAMP: %lu. Change the battery. A technician is required",clock_seconds());
				LOG_WARN_(" for sensor node \"%d%d\" \n",node->u8[6],node->u8[7]);
				break;
		}
	}
}

// Shows the command sent to the actuator
static void log_command() {
	if(previous_mess_actuator.open_window)
		LOG_INFO("TIMESTAMP: %lu. Request to the actuator sent: Open windows\n", clock_seconds());
	else 
		LOG_INFO("TIMESTAMP: %lu. Request to the actuator sent: Close windows\n", clock_seconds());
	if(previous_mess_actuator.open_irrigation)
		LOG_INFO("TIMESTAMP: %lu. Request to the actuator sent: Open irrigation\n", clock_seconds());
	else
		LOG_INFO("TIMESTAMP: %lu. Request to the actuator sent: Close irrigation\n", clock_seconds());
	if(previous_mess_actuator.darken)
		LOG_INFO("TIMESTAMP: %lu. Request to the actuator sent: Turn on the lights\n", clock_seconds());
	else 
		LOG_INFO("TIMESTAMP: %lu. Request to the actuator sent: Turn off the lights\n", clock_seconds());
}

// Adds the sensor node to the array
static void add_sensor_node(const linkaddr_t *node) {
	if(sn_registered == MAX_SENSOR_NODES) {
		LOG_DBG("Impossible register new Sensor node %d%d because too many nodes are registered\n",node->u8[6], node->u8[7]);
		return;
	}
	// Checks if the node already exists
	int i = find_sensor_node(node);
	if(i != -1) {
		LOG_DBG("Sensor Node %d%d already exists\n", node->u8[6], node->u8[7]);
		return;
	}
	// Adds the sensor node to the array
	sensor_nodes[sn_registered].addr = *node;
	sensor_nodes[sn_registered].time = clock_seconds();
	sn_registered ++;
	LOG_DBG("Sensor node %d%d successfully added. ", node->u8[6],node->u8[7]);
	LOG_DBG_("There are been registered %d sensor nodes\n", sn_registered);
	return;
}

// Sends the action to be perform to the actuator
static void send_to_actuator() {
	nullnet_buf = (uint8_t*)&previous_mess_actuator;
	nullnet_len = sizeof(struct mess_to_actuator);
	NETSTACK_NETWORK.output(&actuator.addr);
}

// Sends the reply message to the registration
static void send_registration_resp(const linkaddr_t *src) {
	nullnet_buf = (uint8_t*)&secret;
	nullnet_len = sizeof(unsigned int);
	NETSTACK_NETWORK.output(src);
}

// Checks if it is necessary to send an action to the actuator
static void verify_tresholds(const linkaddr_t* node) {
	struct mess_to_actuator mess;
	memcpy(&mess,&previous_mess_actuator,sizeof(struct mess_to_actuator));
	// Checks the temperature
	if(data_rcv.temperature > broken_temp_sensor_up || data_rcv.temperature < broken_temp_sensor_down) {
		// Temperature sensor may be broken -> usless sends actions to the actuator
		log_mess_sensors(node, 1);
	} else {
		if(data_rcv.temperature > (temperature_treshold + TEMP_RANGE)) {
			mess.open_window = 1;
		}
		if(data_rcv.temperature < (temperature_treshold - TEMP_RANGE)) {
			mess.open_window = 0;
		}
	}

	// Checks the humidity
	if(data_rcv.humidity > broken_humidity_sensor_up || data_rcv.humidity < broken_humidity_sensor_down) {
		// Humidity sensor may be broken -> usless sends actions to the actuator
		log_mess_sensors(node, 2);
	} else {
		if(data_rcv.humidity > (humidity_treshold + TEMP_RANGE)) {
			mess.open_irrigation = 0;
		}
		if(data_rcv.humidity < (humidity_treshold - TEMP_RANGE)) {
			mess.open_irrigation = 1;
		}
	}

	// Checks the light
	if(data_rcv.light > broken_light_sensor_up || data_rcv.light < broken_light_sensor_down) {
		// Light sensor may be broken -> usless sends actions to the actuator
		log_mess_sensors(node, 3);
	} else {
		if(data_rcv.light > (light_treshold + LIGHT_RANGE)) {
			mess.darken = 0;
		}
		if(data_rcv.light < (light_treshold - LIGHT_RANGE)) {
			mess.darken = 1;
		}
	}

	// Checks the level of the battery
	if(data_rcv.mVolt < battery_treshold) {
		log_mess_sensors(node, 4);
	}

	// Checks if anything has changed with respect to the last message sent to the actuator
	if(mess.open_window != previous_mess_actuator.open_window || mess.open_irrigation != previous_mess_actuator.open_irrigation || mess.darken != previous_mess_actuator.darken) { 
		memcpy(&previous_mess_actuator,&mess,sizeof(struct mess_to_actuator));
		log_command();
		send_to_actuator();
	}
}

// Is called whenever a message arrives 
static void input_callback(const void *data, uint16_t len, const linkaddr_t *src, const linkaddr_t *dest){	
	if(*(unsigned int*)data != secret) {
		LOG_DBG("Message comes from an intruder\n");
		return;
	}

	if(linkaddr_cmp(&linkaddr_null,dest) != 0) {	// messaggio broadcast
		if(len != sizeof(*(struct mess_registration*)data)) {
			LOG_DBG("The message received is not intact, error\n");
			return;
		}
		memcpy(&mess_reg,data,sizeof(struct mess_registration));

		// The message comes from a sensor node
		if(mess_reg.t == s_node) {	
			linkaddr_t tmp = *src;
			add_sensor_node(&tmp);
			send_registration_resp(&tmp);
		}

		// The message comes from the actuator
		if(actuator_registered == false && mess_reg.t == act) {
			actuator_registered = true;
			LOG_DBG("Actuators registered %d\n", actuator_registered);
			actuator.addr = *src;
			actuator.time = clock_seconds();
			send_registration_resp(&actuator.addr);
		}
		return;
	} else {	// Unicast message
		if((actuator_registered && linkaddr_cmp(&actuator.addr,src) == 0) && (find_sensor_node(src) == -1)) {
			LOG_DBG("Incoming message from non registered node\n");
			return;
		}

		// The message comes from an actuator -> is a message with info or "I'm Alive"
		if(actuator_registered && linkaddr_cmp(&actuator.addr,src)!=0) {
			update_timer_actuator();

			// Mess "I'm Alive"
			if(len == sizeof(unsigned int)) {
				LOG_DBG("Ack dall'actuator\n");
				return;
			}
			// Mess with info
			if(len != sizeof(*(struct actuator_status*)data)) {
				LOG_DBG("The message received is not intact, error\n");
				return;
			} else {
				memcpy(&mess_act,data,len);
				log_mess_actuator();
			}
			return;
		}

		// Sensor node has sent data
		if(data != NULL && linkaddr_cmp(&actuator.addr,src) == 0) {	
			update_timer_sn(src);
			if(len != sizeof(*(struct mess_sensor_node*)data)) {
				LOG_DBG("The message received is not intact, error\n");
				return;
			}
			if(actuator_registered == false) {
				LOG_DBG("The actuator has not yet registered, no need to check the tresholds, %i\n",actuator_registered);
				return;
			}
			memcpy(&data_rcv,(struct mess_sensor_node*)data,sizeof(struct mess_sensor_node));
			log_mess_sensors(src,0);
			verify_tresholds(src);
		}
	}
}

// Checks if there are sensor nodes or actuator that are no longer active
void check_nodes_off(){
	// Searchs inactive sensor nodes in the array and if there is a deletion keep the compact array
	for(int i = sn_registered - 1; i >= 0; i--) {
		if(sensor_nodes[i].time < (clock_seconds()-INACTIVE_PERIOD_SN)) {
			sn_registered --;
			if(i != sn_registered -1) {	// It is not deleting the last item
				log_inactive_node(1, &sensor_nodes[i].addr);
				sensor_nodes[i] = sensor_nodes[sn_registered];
			}
			else // it is deleting the last item
				log_inactive_node(1, &sensor_nodes[i].addr);
		}
	}

	// Checks inactive actuator
	if(actuator_registered && actuator.time < (clock_seconds()-INACTIVE_PERIOD_ACT)) {
		actuator_registered = false;
		log_inactive_node(0, NULL);
	}
	process_poll(&sink_process);
}

PROCESS_THREAD(sink_process, ev, data){
	static struct ctimer timer_check;

	PROCESS_BEGIN();

	// Initialize the parameters
	previous_mess_actuator.secret = secret;
	previous_mess_actuator.open_window = false;
	previous_mess_actuator.open_irrigation = false;
	previous_mess_actuator.darken = false;
	actuator_registered = false;

	nullnet_set_input_callback(input_callback);
	ctimer_set(&timer_check, TIMER_PERIOD * CLOCK_SECOND, check_nodes_off, NULL);

	while(1) {
		PROCESS_YIELD();
		if(ev == PROCESS_EVENT_POLL) {
			ctimer_restart(&timer_check);
		}
	}	
	PROCESS_END();
}