#include "net/netstack.h"
#include "net/nullnet/nullnet.h"
#include "sys/log.h"
#include "contiki.h"
#include "os/dev/button-hal.h"
#include <random.h>
#include "os/dev/leds.h"
#include "sys/clock.h"
#include "sys/ctimer.h"
#include "structures.h"

#define LOG_MODULE "Actuator"
#define LOG_LEVEL LOG_LEVEL_INFO

#define IRRIGATION 0	//identifier of irrigation actuator 
#define WINDOWS 1	//identifier of windows actuator
#define LIGHTS 2	//identifier of lights actuator
#define NUM_ACTUATOR 3 //number of actuators programmed in the software
#define DELAY_ALIVE_MESSAGE 10 //delay of rate of the ACK message to the sink
#define SECRET 123456789 //security key value

struct actuators { //defines the status (on = 1 /off = 0) and if it is functioning
	bool status; 
	bool broken;
	bool old;	//this variable allows to have memory of the last status before the break or while it's broken
};
static struct actuators elem[NUM_ACTUATOR];
static struct ctimer timer;

static bool connected = false; //variable to state if the actuator is connected to the sink node
static linkaddr_t sink_addr; //here we will save the sink address after the first communication
static struct actuator_status info; //this will indicate to the sink the kind of error or the repaired actuator

PROCESS(actuator_process, "Actuator start");
AUTOSTART_PROCESSES(&actuator_process);

static void register_node(){ //function called for sending the broadcast message to the sink for the first registration
	struct mess_registration registration;
	registration.secret = (unsigned int)SECRET; // we give the security to the sink that we are an allowed node
	registration.t = act;
	nullnet_buf = (uint8_t *)&registration;
	nullnet_len = sizeof(registration);
	NETSTACK_NETWORK.output(NULL); //broadcast message
	LOG_INFO("TIMESTAMP: %lu, Sending BROADCAST message to retrieve the Sink address\n", clock_seconds());
}

static void break_actuator(){ //function called when the right button of the actuator node is pressed, and an actuator breaks
	bool broken = false;
	while(!broken){
		int random = random_rand() % (NUM_ACTUATOR);
		if(!elem[random].broken){
			elem[random].broken = true;
			if(random == IRRIGATION){
				info.status = irrigation_broken;
				LOG_WARN("TIMESTAMP: %lu, Actuator IRRIGATION broken\n", clock_seconds());
			}
			if(random == WINDOWS){
				info.status = windows_broken;
				LOG_WARN("TIMESTAMP: %lu, Actuator WINDOWS broken\n", clock_seconds());
			}
			if(random == LIGHTS){
				info.status = lights_broken;
				LOG_WARN("TIMESTAMP: %lu, Actuator LIGHTS broken\n", clock_seconds());
			}
			broken = true; //i found an actuator to break
			if((random == LIGHTS) && elem[LIGHTS].status){
				elem[LIGHTS].status = false;
				elem[LIGHTS].old = true;
				if(elem[IRRIGATION].status && !elem[WINDOWS].status){
					leds_off(LEDS_RED);
				}

				else if (elem[WINDOWS].status && !elem[IRRIGATION].status){
					leds_off(LEDS_GREEN);
				}

				else if (!elem[WINDOWS].status && !elem[IRRIGATION].status){
					leds_off(LEDS_ALL);
				}
			}
			else if(elem[random].status){
				elem[random].status = false;
				elem[random].old = true;
				if(random == IRRIGATION && !elem[LIGHTS].status){
					leds_off(LEDS_GREEN);
				}
				if(random == WINDOWS && !elem[LIGHTS].status){
					leds_off(LEDS_RED);
				}
			}
			//send the message to the sink to notify the break
			nullnet_buf = (uint8_t *)&info;
			nullnet_len = sizeof(info);
			NETSTACK_NETWORK.output(&sink_addr);
		}	
	}
}

static void repair_actuator(){ //function that is called when the left button of the actuator node is pressed, an actuator repairs.
	if(elem[IRRIGATION].broken || elem[WINDOWS].broken || elem[LIGHTS].broken){ //check if there is something broken
		if(elem[IRRIGATION].broken){
			elem[IRRIGATION].broken = false;
			info.status = irrigation_ok;
			LOG_INFO("TIMESTAMP: %lu, Actuator IRRIGATION repaired\n", clock_seconds());
			if(elem[IRRIGATION].old){ //while the actuator has been broken, the sink requested the activation
				elem[IRRIGATION].status = true;
				elem[IRRIGATION].old = false;
				leds_on(LEDS_GREEN);
				LOG_INFO("TIMESTAMP: %lu, Re-Activated actuator after break: Irrigation\n", clock_seconds());
			}

		}
		else if(elem[WINDOWS].broken){
			elem[WINDOWS].broken = false;
			info.status = windows_ok;
			LOG_INFO("TIMESTAMP: %lu, Actuator WINDOWS repaired\n", clock_seconds());
			if(elem[WINDOWS].old){	//while the actuator has been broken, the sink requested the activation
				elem[WINDOWS].status = true;
				elem[WINDOWS].old = false;
				leds_on(LEDS_RED);
				LOG_INFO("TIMESTAMP: %lu, Re-Activated actuator after break: Windows\n", clock_seconds());
			}
		}
		else if(elem[LIGHTS].broken){
			elem[LIGHTS].broken = false;
			info.status = lights_ok;
			LOG_INFO("TIMESTAMP: %lu, Actuator LIGHTS repaired\n", clock_seconds());
			if(elem[LIGHTS].old){	//while the actuator has been broken, the sink requested the activation
				elem[LIGHTS].status = true;
				elem[LIGHTS].old = false;
				leds_on(LEDS_ALL);
				LOG_INFO("TIMESTAMP: %lu, Re-Activated actuator after break: Lights\n", clock_seconds());
			}
		}
		//send the message to the sink to notify the repair
		nullnet_buf = (uint8_t *)&info;
		nullnet_len = sizeof(info);
		NETSTACK_NETWORK.output(&sink_addr);
	}
}

static void command(struct mess_to_actuator *message){ //function that acts the command given by the sink
	if(message->open_window && !elem[WINDOWS].status){ //red led
		//check if it's not broken
		if(!elem[WINDOWS].broken){
			leds_on(LEDS_RED);
			elem[WINDOWS].status = true;
			LOG_INFO("TIMESTAMP: %lu, Activated actuator: Windows\n", clock_seconds());
		}
		else{
			LOG_WARN("TIMESTAMP: %lu, Sink requested the opening of the Windows, but they are broken \n", clock_seconds());
			elem[WINDOWS].old = true;
		}
	}	
	if(!message->open_window && elem[WINDOWS].status){ 
		//control if i have to keep on the leds, because the lights are on
		if(!elem[LIGHTS].status){ 
			leds_off(LEDS_RED);
		}
		elem[WINDOWS].status = false;
		elem[WINDOWS].old = false;
		LOG_INFO("TIMESTAMP: %lu, Disactivated actuator: Windows\n", clock_seconds());
	}	
	if(message->open_irrigation && !elem[IRRIGATION].status){ //green led
		if(!elem[IRRIGATION].broken){
			leds_on(LEDS_GREEN);
			elem[IRRIGATION].status = true;
			LOG_INFO("TIMESTAMP: %lu, Activated actuator: Irrigation\n", clock_seconds());
		}
		else{
			LOG_WARN("TIMESTAMP: %lu, Sink requested the activation of the Irrigation, but it's broken\n", clock_seconds());
			elem[IRRIGATION].old = true;
		}
	}
	if(!message->open_irrigation && elem[IRRIGATION].status){ 
		if(!elem[LIGHTS].status){
			leds_off(LEDS_GREEN);
		}
		elem[IRRIGATION].status = false;
		elem[IRRIGATION].old = false;
		LOG_INFO("TIMESTAMP: %lu, Disactivated actuator: Irrigation\n", clock_seconds());
	}	
	if(message->darken && !elem[LIGHTS].status) {
		if(!elem[LIGHTS].broken){
			leds_on(LEDS_ALL);
			elem[LIGHTS].status = true;
			LOG_INFO("TIMESTAMP: %lu, Activated actuator: Lights\n", clock_seconds());
		}
		else{
			LOG_WARN("TIMESTAMP: %lu, Sink requested the activation of the lights, but they are broken\n", clock_seconds());
			elem[LIGHTS].old = true;
		}
	}
	if(!message->darken && elem[LIGHTS].status){
		if((!elem[IRRIGATION].status) && (!elem[WINDOWS].status)){
			leds_off(LEDS_ALL);
		}
		else if (elem[IRRIGATION].status){
			leds_off(LEDS_RED);
		}
		else if (elem[WINDOWS].status){
			leds_off(LEDS_GREEN);
		}
		elem[LIGHTS].status = false;
		elem[LIGHTS].old = false;
		LOG_INFO("TIMESTAMP: %lu, Disactivated actuator: Lights\n", clock_seconds());
	}		
}

static void alive(){ //function that sends the ACK to the Sink.
	unsigned int secret = (unsigned int)SECRET;
	nullnet_buf = (uint8_t*)&secret; //NULL message to say to the Sink that i'm alive
	nullnet_len = sizeof(secret);
	NETSTACK_NETWORK.output(&sink_addr);
	LOG_INFO("TIMESTAMP: %lu, Sent ACK message to the sink to tell that i'm not broken\n", clock_seconds());
	ctimer_set(&timer, CLOCK_SECOND * DELAY_ALIVE_MESSAGE, alive, NULL); //re-set the timer in order to trigger the next ACK
}

static void input_callback(const void *data, uint16_t len, const linkaddr_t *src, const linkaddr_t *dest){ 
	if(linkaddr_cmp(&linkaddr_null, dest) == 0){ //discarding broadcast message
		LOG_INFO("TIMESTAMP: %lu, Received message, from ", clock_seconds());
		LOG_INFO_LLADDR(src);
		LOG_INFO_("\n");			
		if(connected && linkaddr_cmp(src,&sink_addr)){ //we've already done the first connection with the sink, now it's giving us a command
			struct mess_to_actuator message = *(struct mess_to_actuator*)data;
			if(sizeof(struct mess_to_actuator) == len && message.secret == (unsigned int)SECRET){
				command(&message);
			}
			else{
				LOG_WARN("TIMESTAMP: %lu: Received message with not consistent data\n", clock_seconds());
			}
		}//closing the command from the sink		
		else if (!connected && len == sizeof((unsigned int)SECRET)){ //first approach between sink and actuator, the sink is answering
			unsigned int secret_sink = *(unsigned int*)data;
			if(secret_sink == (unsigned int)SECRET){
				sink_addr = *src;
				LOG_INFO("TIMESTAMP: %lu, Received message from the SINK, connected to ", clock_seconds());
				LOG_INFO_LLADDR(&sink_addr);
				LOG_INFO_("\n");
				ctimer_set(&timer, CLOCK_SECOND * DELAY_ALIVE_MESSAGE, alive, NULL); //start the ctimer for ACK message (i'm alive) to the sink
				connected = true;
			}
			else{
				LOG_WARN("TIMESTAMP: %lu: A not allowed node has tried to register\n", clock_seconds());
			}
		}//closing the first response
	}//closing not broadcast message
}//closing function

PROCESS_THREAD(actuator_process, ev, data){
	PROCESS_BEGIN();
    	LOG_INFO("TIMESTAMP: %lu, Actuator node is ON. Press the RIGHT button to start the connection with the sink\n", clock_seconds());
		info.secret = (unsigned int)SECRET;
		while(1){
			PROCESS_YIELD();
			//after this call we are sure that an event has occurred	
			if(ev == button_hal_press_event){
				button_hal_button_t *btn = (button_hal_button_t *)data;
				if(btn->unique_id == BOARD_BUTTON_HAL_INDEX_KEY_RIGHT){ //right button, an attuator breaks. Or if the sink is not connected, start the conversation
					if(!connected){
						for(int i = 0; i < NUM_ACTUATOR; i++){
							elem[i].old = false; //initialize the variable to be sure that it will be false at the beginning
						}
						nullnet_set_input_callback(input_callback);
						register_node();
					}
					else if (!(elem[IRRIGATION].broken && elem[WINDOWS].broken && elem[LIGHTS].broken)){ //if they are already dead (everyone) do nothing
						break_actuator();
					}//closing right button if
				}
				if(btn->unique_id == BOARD_BUTTON_HAL_INDEX_KEY_LEFT) { //left button, actuator repaired
					repair_actuator();
				}//closing left button if
			}//closing button event
		}//closing while event
	PROCESS_END();
}	
