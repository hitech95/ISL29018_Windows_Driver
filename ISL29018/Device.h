//Copyright (C) Microsoft Corporation, All Rights Reserved
//
//Abstract:
//
//    This module contains the type definitions for the client
//    driver's device callback class.
//
//Environment:
//
//    Windows User-Mode Driver Framework (UMDF)

#pragma once

#include <windows.h>
#include <wdf.h>
#include <cmath>
#include <reshub.h>
#include <strsafe.h>

#include <SensorsDef.h>
#include <SensorsCx.h>
#include <SensorsUtils.h>
#include <SensorsDriversUtils.h>

#include "isl29018.h"
#include "SensorsTrace.h"



#define SENSORV2_POOL_TAG_ACCELEROMETER '2ccA'

enum class SensorConnectionType : ULONG
{
    Integrated = 0,
    Attached = 1,
    External = 2
};

// Sensor Common Properties
typedef enum
{
    SENSOR_PROPERTY_STATE = 0,
    SENSOR_PROPERTY_MIN_DATA_INTERVAL,
    SENSOR_PROPERTY_MAX_DATA_FIELD_SIZE,
    SENSOR_PROPERTY_TYPE,
    SENSOR_PROPERTY_ALS_RESPONSE_CURVE,
    SENSOR_PROPERTIES_COUNT
} SENSOR_PROPERTIES_INDEX;

// Sensor Enumeration Properties
typedef enum
{
    SENSOR_ENUMERATION_PROPERTY_TYPE = 0,
    SENSOR_ENUMERATION_PROPERTY_MANUFACTURER,
    SENSOR_ENUMERATION_PROPERTY_MODEL,
    SENSOR_ENUMERATION_PROPERTY_CONNECTION_TYPE,
    SENSOR_ENUMERATION_PROPERTY_PERSISTENT_UNIQUE_ID,
    SENSOR_ENUMERATION_PROPERTY_CATEGORY,
    SENSOR_ENUMERATION_PROPERTY_ISPRIMARY,
    SENSOR_ENUMERATION_PROPERTIES_COUNT
} SENSOR_ENUMERATION_PROPERTIES_INDEX;

// Data-field Properties
// Sensor data
typedef enum
{
    ALS_DATA_TIMESTAMP = 0,
    ALS_DATA_LUX,
    ALS_DATA_COUNT
} ALS_DATA_INDEX;

typedef enum
{
    SENSOR_DATA_FIELD_PROPERTY_RESOLUTION = 0,
    SENSOR_DATA_FIELD_PROPERTY_RANGE_MIN,
    SENSOR_DATA_FIELD_PROPERTY_RANGE_MAX,
    SENSOR_DATA_FIELD_PROPERTIES_COUNT
} SENSOR_DATA_FIELD_PROPERTIES_INDEX;

// Sensor thresholds
typedef enum
{
    ALS_THRESHOLD_LUX_PCT = 0,
    ALS_THRESHOLD_LUX_ABS,
    ALS_THRESHOLD_COUNT
} ALS_THRESHOLD_INDEX;

typedef struct _REGISTER_SETTING
{
    BYTE Register;
    BYTE Value;
} REGISTER_SETTING, *PREGISTER_SETTING;

// Array of settings that describe the initial device configuration.
const REGISTER_SETTING g_ConfigurationSettings[] =
{
    // See Intersil AN1534, reset the device
    { ISL29018_REG_ADDR_TEST, 0x00},

    // Standby mode
    { ISL29018_REG_ADD_COMMAND1, 0x00 },

    // 16bit resolution & 4k Lux fullscale range
    { ISL29018_REG_ADD_COMMAND2, 0x01 },
};



typedef class _AlsDevice
{
private:
    // Internal struct used to store thresholds
    typedef struct _AlsThresholdData
    {
        FLOAT LuxPct;
        FLOAT LuxAbs;
    } AlsThresholdData;

private:
    // WDF
    WDFDEVICE                   m_Device;
    WDFIOTARGET                 m_I2CIoTarget;
    WDFWAITLOCK                 m_I2CWaitLock;
    WDFINTERRUPT                m_Interrupt;
    WDFTIMER                    m_Timer;

    // Sensor Operation
    bool                        m_PoweredOn;
    bool                        m_Started;
    ULONG                       m_Interval;
    ULONG                       m_MinimumInterval;

    bool                        m_FirstSample;
    ULONG                       m_StartTime;
    ULONGLONG                   m_SampleCount;

    AlsThresholdData            m_CachedThresholds;
    FLOAT                       m_CachedData;
    FLOAT                       m_LastSample;

    SENSOROBJECT                m_SensorInstance;

    // Sensor Specific Properties
    PSENSOR_PROPERTY_LIST       m_pSupportedDataFields;
    PSENSOR_COLLECTION_LIST     m_pEnumerationProperties;
    PSENSOR_COLLECTION_LIST     m_pSensorProperties;
    PSENSOR_COLLECTION_LIST     m_pSensorData;
    PSENSOR_COLLECTION_LIST     m_pDataFieldProperties;
    PSENSOR_COLLECTION_LIST     m_pThresholds;

public:
    // WDF callbacks
    static EVT_WDF_DRIVER_DEVICE_ADD                OnDeviceAdd;
    static EVT_WDF_DEVICE_PREPARE_HARDWARE          OnPrepareHardware;
    static EVT_WDF_DEVICE_RELEASE_HARDWARE          OnReleaseHardware;
    static EVT_WDF_DEVICE_D0_ENTRY                  OnD0Entry;
    static EVT_WDF_DEVICE_D0_EXIT                   OnD0Exit;

    // CLX callbacks
    static EVT_SENSOR_DRIVER_START_SENSOR               OnStart;
    static EVT_SENSOR_DRIVER_STOP_SENSOR                OnStop;
    static EVT_SENSOR_DRIVER_GET_SUPPORTED_DATA_FIELDS  OnGetSupportedDataFields;
    static EVT_SENSOR_DRIVER_GET_PROPERTIES             OnGetProperties;
    static EVT_SENSOR_DRIVER_GET_DATA_FIELD_PROPERTIES  OnGetDataFieldProperties;
    static EVT_SENSOR_DRIVER_GET_DATA_INTERVAL          OnGetDataInterval;
    static EVT_SENSOR_DRIVER_SET_DATA_INTERVAL          OnSetDataInterval;
    static EVT_SENSOR_DRIVER_GET_DATA_THRESHOLDS        OnGetDataThresholds;
    static EVT_SENSOR_DRIVER_SET_DATA_THRESHOLDS        OnSetDataThresholds;
    static EVT_SENSOR_DRIVER_DEVICE_IO_CONTROL          OnIoControl;

    // Interrupt callbacks
    static EVT_WDF_INTERRUPT_ISR       OnInterruptIsr;
    static EVT_WDF_INTERRUPT_WORKITEM  OnInterruptWorkItem;
    static VOID                        OnTimerExpire(_In_ WDFTIMER Timer);

private:
    NTSTATUS                    GetData();
    NTSTATUS                    UpdateCachedThreshold();

    // Helper function for OnPrepareHardware to initialize sensor to default properties
    NTSTATUS                    Initialize(_In_ WDFDEVICE Device, _In_ SENSOROBJECT SensorInstance);
    VOID                        DeInit();

    // Helper function for OnPrepareHardware to get resources from ACPI and configure the I/O target
    NTSTATUS                    ConfigureIoTarget(_In_ WDFCMRESLIST ResourceList,
                                                  _In_ WDFCMRESLIST ResourceListTranslated);

    // Helper function for OnD0Entry which sets up device to default configuration
    NTSTATUS                    PowerOn();
    NTSTATUS                    PowerOff();
    
    NTSTATUS                    IsrOn();
    NTSTATUS                    IsrOff();
    

} AlsDevice, *PAlsDevice;

// Set up accessor function to retrieve device context
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(AlsDevice, GetAlsDeviceContextFromSensorInstance);
