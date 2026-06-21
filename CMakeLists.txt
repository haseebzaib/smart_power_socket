# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(smart_power_socket)

target_sources(app PRIVATE
	src/main.cpp
	src/hardware/gpio_con.cpp
	src/hardware/uart_con.cpp
	src/sensors/hlw811x.cpp
	src/sensors/externalLibs/hlw811x/hlw811x.c
	src/cellular/at_engine.cpp
)

target_include_directories(app PRIVATE
	src
	src/hardware
	src/sensors
	src/sensors/externalLibs/hlw811x
	src/cellular/
)
