//Copyright (C) Microsoft Corporation, All Rights Reserved
//
//Abstract:
//
//      This module contains the constant definitions for the accelerometer's
//      register interface and default property values.

#pragma once

#include "WTypesbase.h"

#define ISL29018_CONV_TIME_MS		100

#define ISL29018_REG_ADD_COMMAND1	0x00
#define ISL29018_CMD1_ISR_SHIFT	2
#define ISL29018_CMD1_ISR_MASK	(0x1 << ISL29018_CMD1_ISR_SHIFT)

#define ISL29018_CMD1_OPMODE_SHIFT	5
#define ISL29018_CMD1_OPMODE_MASK	(7 << ISL29018_CMD1_OPMODE_SHIFT)
#define ISL29018_CMD1_OPMODE_POWER_DOWN	0
#define ISL29018_CMD1_OPMODE_ALS_ONCE	1
#define ISL29018_CMD1_OPMODE_IR_ONCE	2
#define ISL29018_CMD1_OPMODE_PROX_ONCE	3
#define ISL29018_CMD1_OPMODE_ALS_CONT	5
#define ISL29018_CMD1_OPMODE_IR_CONT	6
#define ISL29018_CMD1_OPMODE_PROX_CONT	7

#define ISL29018_REG_ADD_COMMAND2	    0x01
#define ISL29018_CMD2_RESOLUTION_SHIFT	2
#define ISL29018_CMD2_RESOLUTION_MASK	(0x3 << ISL29018_CMD2_RESOLUTION_SHIFT)

#define ISL29018_CMD2_RANGE_SHIFT	0
#define ISL29018_CMD2_RANGE_MASK	(0x3 << ISL29018_CMD2_RANGE_SHIFT)

#define ISL29018_CMD2_SCHEME_SHIFT	7
#define ISL29018_CMD2_SCHEME_MASK	(0x1 << ISL29018_CMD2_SCHEME_SHIFT)

#define ISL29018_REG_ADD_DATA_LSB	0x02
#define ISL29018_REG_ADD_DATA_MSB	0x03
#define ISL290185_DATA_SIZE_BYTES   2

#define ISL29018_REG_ADD_INT_LT_LSB	0x04
#define ISL29018_REG_ADD_INT_LT_MSB	0x05
#define ISL29018_REG_ADD_INT_HT_LSB	0x06
#define ISL29018_REG_ADD_INT_HT_MSB	0x07

#define ISL29018_REG_ADDR_TEST		0x08
#define ISL29018_TEST_SHIFT		0
#define ISL29018_TEST_MASK		(0xFF << ISL29018_TEST_SHIFT)

enum isl29018_int_time {
	ISL29018_INT_TIME_16,
	ISL29018_INT_TIME_12,
	ISL29018_INT_TIME_8,
	ISL29018_INT_TIME_4,
};

static const unsigned int isl29018_int_utimes[3][4] = {
	{90000, 5630, 351, 21},
	{90000, 5600, 352, 22},
	{105000, 6500, 410, 25},
};

static const struct isl29018_scale {
	unsigned int scale;
	unsigned int uscale;
} isl29018_scales[4][4] = {
	{ {0, 15258}, {0, 61035}, {0, 244140}, {0, 976562} },
	{ {0, 244140}, {0, 976562}, {3, 906250}, {15, 625000} },
	{ {3, 906250}, {15, 625000}, {62, 500000}, {250, 0} },
	{ {62, 500000}, {250, 0}, {1000, 0}, {4000, 0} }
};

const unsigned short SENSOR_ALS_NAME[] = L"Ambient Light Sensor";
const unsigned short SENSOR_ALS_DESCRIPTION[] = L"Ambient Light Sensor";
const unsigned short SENSOR_ALS_ID[] = L"ISL29018";
const unsigned short SENSOR_ALS_MANUFACTURER[] = L"Intersil";
const unsigned short SENSOR_ALS_MODEL[] = L"ISL29018";
const unsigned short SENSOR_ALS_SERIAL_NUMBER[] = L"0123456789=0123456789";
