# SmartOrchard Project
This repository contains the files and the documentation for the didactical project for the Networked Embedded Systems course.
The code folder contains the file for the three kind of actors of the IoT Environment:
* [Actuator](/code/actuator.c)
* [Sensor](/code/sensor.c)
* [Sink](/code/sink.c)

The whole code has been compiled in the Contiki-NG operating system.

## Assignment 
For this project, it has been provided several use cases for real life scenarios in which the IoT devices are used in order to improve people's lives.
The use case that has been chosen for this project is the Smart Orchard, which is the following:
![Smart Orchard Use Case](/images/usecase.png)
Each group will be given of 3 IoT launchpads.

## Requirements
* Each use case must include 3 different roles (3 different firmware)
* At least 1 sensor must produce some information
* At least 1 sensor must consume the produced information
* The MAC adress must be discovered by broadcast messages (not hardcoded)
* Both broadcast and unicast communication must be used
* Use at least one timer between etimer and ctimer
* Use at least two of the interactions human(environment)/sensor and viceversa, that we have seen during lectures on each sensor:
    * Inputs from serial line 
    * Leds
    * Buttons
    * Batmon sensor

## Launchpad
The launchpads used in order to develop the project have the following characteristics:
![Launchpad](/images/launchpad.png)

## Presentation
It is also available the [presentation](/SmartOrchard_Presentation.pdf) of the project for more details of the implementation.

# Credits
E. Petrangeli, A. De Roberto, L. Fontanelli