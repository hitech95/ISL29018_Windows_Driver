//Copyright (C) Microsoft Corporation, All Rights Reserved.
//
//Abstract:
//
//    This module contains the implementation of driver callback functions
//    from clx to ISL29018 accelerometer.
//
//Environment:
//
//   Windows User-Mode Driver Framework (UMDF)

#include "Device.h"
#include "isl29018.h"

#include <timeapi.h>

#include "Client.tmh"


#define SENSORV2_POOL_TAG_AMBIENT_LIGHT           '2LmA'

#define Als_Initial_MinDataInterval_Ms            (90)          // 12Hz
#define Als_Initial_Lux_Threshold_Pct             (1.0f)        // Percent threshold: 100%
#define Als_Initial_Lux_Threshold_Abs             (0.0f)        // Absolute threshold: 0 lux

#define AlsDevice_Minimum_Lux                     (0.0f)
#define AlsDevice_Maximum_Lux                     (4000.0f)
#define AlsDevice_Precision                       (65536.0f)    // 65536 = 2^16, 16 bit data
#define AlsDevice_Range_Lux                       (AlsDevice_Maximum_Lux - AlsDevice_Minimum_Lux)
#define AlsDevice_Resolution_Lux                  (AlsDevice_Range_Lux / AlsDevice_Precision)

// Ambient Light Sensor Unique ID
// {2D2A4524-51E3-4E68-9B0F-5CAEDFB12C02}
DEFINE_GUID(GUID_AlsDevice_UniqueID,
    0x2d2a4524, 0x51e3, 0x4e68, 0x9b, 0xf, 0x5c, 0xae, 0xdf, 0xb1, 0x2c, 0x2);

static const UINT SYSTEM_TICK_COUNT_1MS = 1; // 1ms

//------------------------------------------------------------------------------
// Function: Initialize
//
// This routine initializes the sensor to its default properties
//
// Arguments:
//       Device: IN: WDFDEVICE object
//       SensorInstance: IN: SENSOROBJECT for each sensor instance
//
// Return Value:
//      NTSTATUS code
//------------------------------------------------------------------------------
NTSTATUS
AlsDevice::Initialize(
    _In_ WDFDEVICE Device,
    _In_ SENSOROBJECT SensorInstance
)
{
    NTSTATUS Status = STATUS_SUCCESS;

    SENSOR_FunctionEnter();

    //
    // Store device and instance
    //
    m_Device = Device;
    m_SensorInstance = SensorInstance;
    m_Started = FALSE;

    //
    // Create Lock
    //
    Status = WdfWaitLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &m_I2CWaitLock);
    if (!NT_SUCCESS(Status))
    {
        TraceError("COMBO %!FUNC! ALS WdfWaitLockCreate failed %!STATUS!", Status);
        goto Exit;
    }

    //
    // Create timer object for polling sensor samples
    //
    {
        WDF_OBJECT_ATTRIBUTES TimerAttributes;
        WDF_TIMER_CONFIG TimerConfig;

        WDF_TIMER_CONFIG_INIT(&TimerConfig, AlsDevice::OnTimerExpire);
        WDF_OBJECT_ATTRIBUTES_INIT(&TimerAttributes);
        TimerAttributes.ParentObject = SensorInstance;
        TimerAttributes.ExecutionLevel = WdfExecutionLevelPassive;

        Status = WdfTimerCreate(&TimerConfig, &TimerAttributes, &m_Timer);
        if (!NT_SUCCESS(Status))
        {
            TraceError("COMBO %!FUNC! ALS WdfTimerCreate failed %!STATUS!", Status);
            goto Exit;
        }
    }

    //
    // Sensor Enumeration Properties
    //
    {
        WDF_OBJECT_ATTRIBUTES MemoryAttributes;
        WDFMEMORY MemoryHandle = NULL;
        ULONG Size = SENSOR_COLLECTION_LIST_SIZE(SENSOR_ENUMERATION_PROPERTIES_COUNT);

        MemoryHandle = NULL;
        WDF_OBJECT_ATTRIBUTES_INIT(&MemoryAttributes);
        MemoryAttributes.ParentObject = SensorInstance;
        Status = WdfMemoryCreate(&MemoryAttributes,
            PagedPool,
            SENSORV2_POOL_TAG_AMBIENT_LIGHT,
            Size,
            &MemoryHandle,
            (PVOID*)&m_pEnumerationProperties);
        if (!NT_SUCCESS(Status) || m_pEnumerationProperties == nullptr)
        {
            TraceError("COMBO %!FUNC! ALS WdfMemoryCreate failed %!STATUS!", Status);
            goto Exit;
        }

        SENSOR_COLLECTION_LIST_INIT(m_pEnumerationProperties, Size);
        m_pEnumerationProperties->Count = SENSOR_ENUMERATION_PROPERTIES_COUNT;

        m_pEnumerationProperties->List[SENSOR_ENUMERATION_PROPERTY_TYPE].Key = DEVPKEY_Sensor_Type;
        InitPropVariantFromCLSID(GUID_SensorType_AmbientLight,
            &(m_pEnumerationProperties->List[SENSOR_ENUMERATION_PROPERTY_TYPE].Value));

        m_pEnumerationProperties->List[SENSOR_ENUMERATION_PROPERTY_MANUFACTURER].Key = DEVPKEY_Sensor_Manufacturer;
        InitPropVariantFromString(SENSOR_ALS_MANUFACTURER,
            &(m_pEnumerationProperties->List[SENSOR_ENUMERATION_PROPERTY_MANUFACTURER].Value));

        m_pEnumerationProperties->List[SENSOR_ENUMERATION_PROPERTY_MODEL].Key = DEVPKEY_Sensor_Model;
        InitPropVariantFromString(SENSOR_ALS_MODEL,
            &(m_pEnumerationProperties->List[SENSOR_ENUMERATION_PROPERTY_MODEL].Value));

        m_pEnumerationProperties->List[SENSOR_ENUMERATION_PROPERTY_CONNECTION_TYPE].Key = DEVPKEY_Sensor_ConnectionType;
        // The DEVPKEY_Sensor_ConnectionType values match the SensorConnectionType enumeration
        InitPropVariantFromUInt32(static_cast<ULONG>(SensorConnectionType::Integrated),
            &(m_pEnumerationProperties->List[SENSOR_ENUMERATION_PROPERTY_CONNECTION_TYPE].Value));

        m_pEnumerationProperties->List[SENSOR_ENUMERATION_PROPERTY_PERSISTENT_UNIQUE_ID].Key = DEVPKEY_Sensor_PersistentUniqueId;
        InitPropVariantFromCLSID(GUID_AlsDevice_UniqueID,
            &(m_pEnumerationProperties->List[SENSOR_ENUMERATION_PROPERTY_PERSISTENT_UNIQUE_ID].Value));

        m_pEnumerationProperties->List[SENSOR_ENUMERATION_PROPERTY_CATEGORY].Key = DEVPKEY_Sensor_Category;
        InitPropVariantFromCLSID(GUID_SensorCategory_Light,
            &(m_pEnumerationProperties->List[SENSOR_ENUMERATION_PROPERTY_CATEGORY].Value));

        m_pEnumerationProperties->List[SENSOR_ENUMERATION_PROPERTY_ISPRIMARY].Key = DEVPKEY_Sensor_IsPrimary;
        InitPropVariantFromBoolean(TRUE,
            &(m_pEnumerationProperties->List[SENSOR_ENUMERATION_PROPERTY_ISPRIMARY].Value));
    }

    //
    // Supported Data-Fields
    //
    {
        WDF_OBJECT_ATTRIBUTES MemoryAttributes;
        WDFMEMORY MemoryHandle = NULL;
        ULONG Size = SENSOR_PROPERTY_LIST_SIZE(ALS_DATA_COUNT);

        MemoryHandle = NULL;
        WDF_OBJECT_ATTRIBUTES_INIT(&MemoryAttributes);
        MemoryAttributes.ParentObject = SensorInstance;
        Status = WdfMemoryCreate(&MemoryAttributes,
            PagedPool,
            SENSORV2_POOL_TAG_AMBIENT_LIGHT,
            Size,
            &MemoryHandle,
            (PVOID*)&m_pSupportedDataFields);
        if (!NT_SUCCESS(Status) || m_pSupportedDataFields == nullptr)
        {
            TraceError("COMBO %!FUNC! ALS WdfMemoryCreate failed %!STATUS!", Status);
            goto Exit;
        }

        SENSOR_PROPERTY_LIST_INIT(m_pSupportedDataFields, Size);
        m_pSupportedDataFields->Count = ALS_DATA_COUNT;

        m_pSupportedDataFields->List[ALS_DATA_TIMESTAMP] = PKEY_SensorData_Timestamp;
        m_pSupportedDataFields->List[ALS_DATA_LUX] = PKEY_SensorData_LightLevel_Lux;
    }

    //
    // Data
    //
    {
        WDF_OBJECT_ATTRIBUTES MemoryAttributes;
        WDFMEMORY MemoryHandle = NULL;
        ULONG Size = SENSOR_COLLECTION_LIST_SIZE(ALS_DATA_COUNT);
        FILETIME Time = { 0 };

        MemoryHandle = NULL;
        WDF_OBJECT_ATTRIBUTES_INIT(&MemoryAttributes);
        MemoryAttributes.ParentObject = SensorInstance;
        Status = WdfMemoryCreate(&MemoryAttributes,
            PagedPool,
            SENSORV2_POOL_TAG_AMBIENT_LIGHT,
            Size,
            &MemoryHandle,
            (PVOID*)&m_pSensorData);
        if (!NT_SUCCESS(Status) || m_pSensorData == nullptr)
        {
            TraceError("COMBO %!FUNC! ALS WdfMemoryCreate failed %!STATUS!", Status);
            goto Exit;
        }

        SENSOR_COLLECTION_LIST_INIT(m_pSensorData, Size);
        m_pSensorData->Count = ALS_DATA_COUNT;

        m_pSensorData->List[ALS_DATA_TIMESTAMP].Key = PKEY_SensorData_Timestamp;
        GetSystemTimePreciseAsFileTime(&Time);
        InitPropVariantFromFileTime(&Time, &(m_pSensorData->List[ALS_DATA_TIMESTAMP].Value));

        m_pSensorData->List[ALS_DATA_LUX].Key = PKEY_SensorData_LightLevel_Lux;
        InitPropVariantFromFloat(0.0f, &(m_pSensorData->List[ALS_DATA_LUX].Value));

        m_CachedData = 1.0f; // Lux
        m_LastSample = 0.0f; // Lux
    }

    //
    // Sensor Properties
    //
    {
        WDF_OBJECT_ATTRIBUTES MemoryAttributes;
        WDFMEMORY MemoryHandle = NULL;
        ULONG Size = SENSOR_COLLECTION_LIST_SIZE(SENSOR_PROPERTIES_COUNT);

        MemoryHandle = NULL;
        WDF_OBJECT_ATTRIBUTES_INIT(&MemoryAttributes);
        MemoryAttributes.ParentObject = SensorInstance;
        Status = WdfMemoryCreate(&MemoryAttributes,
            PagedPool,
            SENSORV2_POOL_TAG_AMBIENT_LIGHT,
            Size,
            &MemoryHandle,
            (PVOID*)&m_pSensorProperties);
        if (!NT_SUCCESS(Status) || m_pSensorProperties == nullptr)
        {
            TraceError("COMBO %!FUNC! ALS WdfMemoryCreate failed %!STATUS!", Status);
            goto Exit;
        }

        SENSOR_COLLECTION_LIST_INIT(m_pSensorProperties, Size);
        m_pSensorProperties->Count = SENSOR_PROPERTIES_COUNT;

        m_pSensorProperties->List[SENSOR_PROPERTY_STATE].Key = PKEY_Sensor_State;
        InitPropVariantFromUInt32(SensorState_Initializing,
            &(m_pSensorProperties->List[SENSOR_PROPERTY_STATE].Value));

        m_pSensorProperties->List[SENSOR_PROPERTY_MIN_DATA_INTERVAL].Key = PKEY_Sensor_MinimumDataInterval_Ms;
        InitPropVariantFromUInt32(Als_Initial_MinDataInterval_Ms,
            &(m_pSensorProperties->List[SENSOR_PROPERTY_MIN_DATA_INTERVAL].Value));
        m_Interval = Als_Initial_MinDataInterval_Ms;
        m_MinimumInterval = Als_Initial_MinDataInterval_Ms;

        m_pSensorProperties->List[SENSOR_PROPERTY_MAX_DATA_FIELD_SIZE].Key = PKEY_Sensor_MaximumDataFieldSize_Bytes;
        InitPropVariantFromUInt32(CollectionsListGetMarshalledSize(m_pSensorData),
            &(m_pSensorProperties->List[SENSOR_PROPERTY_MAX_DATA_FIELD_SIZE].Value));

        m_pSensorProperties->List[SENSOR_PROPERTY_TYPE].Key = PKEY_Sensor_Type;
        InitPropVariantFromCLSID(GUID_SensorType_AmbientLight,
            &(m_pSensorProperties->List[SENSOR_PROPERTY_TYPE].Value));

        ULONG responseCurve[10] = {}; // Array to contain the response curve data.

        // ****************************************************************************************
        // The response curve consists of an array of byte pairs.
        // The first byte contains the percentage brightness offset to be applied to the display.
        // The second byte contains the corresponding ambient light value (in LUX).
        // ****************************************************************************************
        // (0, 10)
        responseCurve[0] = 0; responseCurve[1] = 10;
        // (10, 40)
        responseCurve[2] = 10; responseCurve[3] = 40;
        // (40, 100)
        responseCurve[4] = 40; responseCurve[5] = 100;
        // (68, 400)
        responseCurve[6] = 68; responseCurve[7] = 400;
        // (90, 1000)
        responseCurve[8] = 90; responseCurve[9] = 1000;

        m_pSensorProperties->List[SENSOR_PROPERTY_ALS_RESPONSE_CURVE].Key = PKEY_LightSensor_ResponseCurve;
        InitPropVariantFromUInt32Vector(responseCurve,
            10,
            &(m_pSensorProperties->List[SENSOR_PROPERTY_ALS_RESPONSE_CURVE].Value));
    }

    //
    // Data filed properties
    //
    {
        WDF_OBJECT_ATTRIBUTES MemoryAttributes;
        WDFMEMORY MemoryHandle = NULL;
        ULONG Size = SENSOR_COLLECTION_LIST_SIZE(SENSOR_DATA_FIELD_PROPERTIES_COUNT);

        MemoryHandle = NULL;
        WDF_OBJECT_ATTRIBUTES_INIT(&MemoryAttributes);
        MemoryAttributes.ParentObject = SensorInstance;
        Status = WdfMemoryCreate(&MemoryAttributes,
            PagedPool,
            SENSORV2_POOL_TAG_AMBIENT_LIGHT,
            Size,
            &MemoryHandle,
            (PVOID*)&m_pDataFieldProperties);
        if (!NT_SUCCESS(Status) || m_pDataFieldProperties == nullptr)
        {
            TraceError("COMBO %!FUNC! ALS WdfMemoryCreate failed %!STATUS!", Status);
            goto Exit;
        }

        SENSOR_COLLECTION_LIST_INIT(m_pDataFieldProperties, Size);
        m_pDataFieldProperties->Count = SENSOR_DATA_FIELD_PROPERTIES_COUNT;

        m_pDataFieldProperties->List[SENSOR_DATA_FIELD_PROPERTY_RESOLUTION].Key = PKEY_SensorDataField_Resolution;
        InitPropVariantFromFloat(AlsDevice_Resolution_Lux,
            &(m_pDataFieldProperties->List[SENSOR_DATA_FIELD_PROPERTY_RESOLUTION].Value));

        m_pDataFieldProperties->List[SENSOR_DATA_FIELD_PROPERTY_RANGE_MIN].Key = PKEY_SensorDataField_RangeMinimum;
        InitPropVariantFromFloat(AlsDevice_Minimum_Lux,
            &(m_pDataFieldProperties->List[SENSOR_DATA_FIELD_PROPERTY_RANGE_MIN].Value));

        m_pDataFieldProperties->List[SENSOR_DATA_FIELD_PROPERTY_RANGE_MAX].Key = PKEY_SensorDataField_RangeMaximum;
        InitPropVariantFromFloat(AlsDevice_Maximum_Lux,
            &(m_pDataFieldProperties->List[SENSOR_DATA_FIELD_PROPERTY_RANGE_MAX].Value));
    }

    //
    // Set default threshold
    //
    {
        WDF_OBJECT_ATTRIBUTES MemoryAttributes;
        WDFMEMORY MemoryHandle = NULL;
        ULONG Size = SENSOR_COLLECTION_LIST_SIZE(ALS_THRESHOLD_COUNT);

        MemoryHandle = NULL;
        WDF_OBJECT_ATTRIBUTES_INIT(&MemoryAttributes);
        MemoryAttributes.ParentObject = SensorInstance;
        Status = WdfMemoryCreate(&MemoryAttributes,
            PagedPool,
            SENSORV2_POOL_TAG_AMBIENT_LIGHT,
            Size,
            &MemoryHandle,
            (PVOID*)&m_pThresholds);
        if (!NT_SUCCESS(Status) || m_pThresholds == nullptr)
        {
            TraceError("COMBO %!FUNC! ALS WdfMemoryCreate failed %!STATUS!", Status);
            goto Exit;
        }

        SENSOR_COLLECTION_LIST_INIT(m_pThresholds, Size);
        m_pThresholds->Count = ALS_THRESHOLD_COUNT;

        // Set lux threshold
        m_pThresholds->List[ALS_THRESHOLD_LUX_PCT].Key = PKEY_SensorData_LightLevel_Lux;
        InitPropVariantFromFloat(Als_Initial_Lux_Threshold_Pct,
            &(m_pThresholds->List[ALS_THRESHOLD_LUX_PCT].Value));
        m_CachedThresholds.LuxPct = Als_Initial_Lux_Threshold_Pct;

        m_pThresholds->List[ALS_THRESHOLD_LUX_ABS].Key = PKEY_SensorData_LightLevel_Lux_Threshold_AbsoluteDifference;
        InitPropVariantFromFloat(Als_Initial_Lux_Threshold_Abs,
            &(m_pThresholds->List[ALS_THRESHOLD_LUX_ABS].Value));
        m_CachedThresholds.LuxAbs = Als_Initial_Lux_Threshold_Abs;

        m_FirstSample = TRUE;
    }

Exit:
    SENSOR_FunctionExit(Status);
    return Status;
}

VOID 
AlsDevice::DeInit()
{
    // Delete lock
    if (NULL != m_I2CWaitLock)
    {
        WdfObjectDelete(m_I2CWaitLock);
        m_I2CWaitLock = NULL;
    }

    // Delete sensor instance
    if (NULL != m_SensorInstance)
    {
        WdfObjectDelete(m_SensorInstance);
    }
}

//------------------------------------------------------------------------------
// Function: GetData
//
// This routine is called by worker thread to read a single sample, compare threshold
// and push it back to CLX. It simulates hardware thresholding by only generating data
// when the change of data is greater than threshold.
//
// Arguments:
//       None
//
// Return Value:
//      NTSTATUS code
//------------------------------------------------------------------------------
NTSTATUS
AlsDevice::GetData(
)
{
    BOOLEAN DataReady = FALSE;
    FILETIME TimeStamp = { 0 };
    NTSTATUS Status = STATUS_SUCCESS;

    SENSOR_FunctionEnter();

    // Read the device data
    BYTE DataBuffer[ISL290185_DATA_SIZE_BYTES];
    WdfWaitLockAcquire(m_I2CWaitLock, NULL);
    Status = I2CSensorReadRegister(m_I2CIoTarget, ISL29018_REG_ADD_DATA_LSB, &DataBuffer[0], sizeof(DataBuffer));
    WdfWaitLockRelease(m_I2CWaitLock);
    if (!NT_SUCCESS(Status))
    {
        TraceError("ACC %!FUNC! I2CSensorReadRegister from 0x%02x failed! %!STATUS!", ISL29018_REG_ADD_DATA_LSB, Status);
    }
    else
    {
       // bool DataReady = false;

        // Perform data conversion
        SHORT luxRaw = static_cast<SHORT>((DataBuffer[1] << 8) | DataBuffer[0]);

        m_CachedData = static_cast<float>(luxRaw * AlsDevice_Resolution_Lux);
    }

    // new sample?
    if (m_FirstSample != FALSE)
    {
        Status = GetPerformanceTime(&m_StartTime);
        if (!NT_SUCCESS(Status))
        {
            m_StartTime = 0;
            TraceError("COMBO %!FUNC! ALS GetPerformanceTime %!STATUS!", Status);
        }

        m_SampleCount = 0;

        DataReady = TRUE;
    }
    else
    {
        // Compare the change of data to threshold, and only push the data back to
        // clx if the change exceeds threshold. This is usually done in HW.
        if (            
            // Lux thresholds needs to exceed absolute and percentage
            ((abs(m_CachedData - m_LastSample) >= (m_LastSample * m_CachedThresholds.LuxPct)) &&
                (abs(m_CachedData - m_LastSample) >= m_CachedThresholds.LuxAbs)))
        {
            DataReady = TRUE;
        }
    }

    if (DataReady != FALSE)
    {
        // update last sample
        m_LastSample = m_CachedData;

        // push to clx
        InitPropVariantFromFloat(m_LastSample, &(m_pSensorData->List[ALS_DATA_LUX].Value));

        GetSystemTimePreciseAsFileTime(&TimeStamp);
        InitPropVariantFromFileTime(&TimeStamp, &(m_pSensorData->List[ALS_DATA_TIMESTAMP].Value));

        SensorsCxSensorDataReady(m_SensorInstance, m_pSensorData);
        m_FirstSample = FALSE;
    }
    else
    {
        Status = STATUS_DATA_NOT_ACCEPTED;
        TraceInformation("COMBO %!FUNC! ALS Data did NOT meet the threshold");
    }

    SENSOR_FunctionExit(Status);
    return Status;
}

// Called by Sensor CLX to begin continously sampling the sensor.
NTSTATUS AlsDevice::OnStart(
    _In_ SENSOROBJECT SensorInstance)    // Sensor device object
{
    NTSTATUS Status = STATUS_SUCCESS;
    REGISTER_SETTING setting;

    SENSOR_FunctionEnter();

    // Get the device context
    PAlsDevice pDevice = GetAlsDeviceContextFromSensorInstance(SensorInstance);
    if (nullptr == pDevice)
    {
        Status = STATUS_INVALID_PARAMETER;
        TraceError("ACC %!FUNC! Sensor(0x%p) parameter is invalid %!STATUS!", SensorInstance, Status);
    }
    else if (!pDevice->m_PoweredOn)
    {
        Status = STATUS_INVALID_PARAMETER;
        TraceError("ACC %!FUNC! Sensor is not powered on! %!STATUS!", Status);
    }
    else
    {
        WdfWaitLockAcquire(pDevice->m_I2CWaitLock, NULL);

        // Set accelerometer to measurement mode
        setting = { ISL29018_REG_ADD_COMMAND1, ISL29018_CMD1_OPMODE_ALS_CONT << ISL29018_CMD1_OPMODE_SHIFT };
        Status = I2CSensorWriteRegister(pDevice->m_I2CIoTarget, setting.Register, &setting.Value, sizeof(setting.Value));
        if (!NT_SUCCESS(Status))
        {
            WdfWaitLockRelease(pDevice->m_I2CWaitLock);
            TraceError("ACC %!FUNC! I2CSensorWriteRegister to 0x%02x failed! %!STATUS!", setting.Register, Status);
        }

        // Enable interrupts
        else // if (NT_SUCCESS(Status))
        {
            /*
            // Enable Interrupts
            Status = IsrOn();
            if (!NT_SUCCESS(Status))
            {
                TraceError("ACC %!FUNC! Failed to enable interrupts. %!STATUS!", Status);
            }
            */
        }

        if (NT_SUCCESS(Status))
        {
            pDevice->m_FirstSample = true;
            pDevice->m_Started = true;

            InitPropVariantFromUInt32(SensorState_Active, &(pDevice->m_pSensorProperties->List[SENSOR_PROPERTY_STATE].Value));

            // Start polling
            WdfTimerStart(pDevice->m_Timer, WDF_REL_TIMEOUT_IN_MS(pDevice->m_MinimumInterval));
        }
    }

    SENSOR_FunctionExit(Status);
    return Status;
}

// Called by Sensor CLX to stop continously sampling the sensor.
NTSTATUS AlsDevice::OnStop(
    _In_ SENSOROBJECT SensorInstance)   // Sensor device object
{
    NTSTATUS Status = STATUS_SUCCESS;
    REGISTER_SETTING setting;

    SENSOR_FunctionEnter();

    // Get the device context
    PAlsDevice pDevice = GetAlsDeviceContextFromSensorInstance(SensorInstance);
    if (nullptr == pDevice)
    {
        Status = STATUS_INVALID_PARAMETER;
        TraceError("ACC %!FUNC! Sensor(0x%p) parameter is invalid %!STATUS!", SensorInstance, Status);
    }
    else
    {
        pDevice->m_Started = false;

        /*
        // Disable Interrupts
        Status = IsrOff();
        if (!NT_SUCCESS(Status))
        {
            TraceError("ACC %!FUNC! Failed to disable interrupts. %!STATUS!", Status);
        }
        // Clear any stale interrupts
        else
        {
            RegisterSetting = { ISL29018_REG_ADD_COMMAND1, 0 };
            Status = I2CSensorWriteRegister(pDevice->m_I2CIoTarget, RegisterSetting.Register, &RegisterSetting.Value, sizeof(RegisterSetting.Value));
            if (!NT_SUCCESS(Status))
            {
                TraceError("ACC %!FUNC! I2CSensorReadRegister from 0x%02x failed! %!STATUS!", RegisterSetting.Register, Status);
            }
        }
        */

        // Stop polling

        WdfTimerStop(pDevice->m_Timer, TRUE);

        // Set sensor to standby
        setting = { ISL29018_REG_ADD_COMMAND1, ISL29018_CMD1_OPMODE_POWER_DOWN << ISL29018_CMD1_OPMODE_SHIFT};
        Status = I2CSensorWriteRegister(pDevice->m_I2CIoTarget, setting.Register, &setting.Value, sizeof(setting.Value));
        WdfWaitLockRelease(pDevice->m_I2CWaitLock);
        if (!NT_SUCCESS(Status))
        {
            TraceError("ACC %!FUNC! I2CSensorWriteRegister to 0x%02x failed! %!STATUS!", setting.Register, Status);
        }
        else
        {
            InitPropVariantFromUInt32(SensorState_Idle, &(pDevice->m_pSensorProperties->List[SENSOR_PROPERTY_STATE].Value));

            //
            // Restoring system time resolution
            //
            if (TIMERR_NOERROR != timeEndPeriod(SYSTEM_TICK_COUNT_1MS))
            {
                // not a failure, just log message
                Status = STATUS_UNSUCCESSFUL;
                TraceWarning("COMBO %!FUNC! timeEndPeriod failed to restore timer resolution! %!STATUS!", Status);
            }
        }
    }

    SENSOR_FunctionExit(Status);
    return Status;
}

// Called by Sensor CLX to get supported data fields. The typical usage is to call
// this function once with buffer pointer as NULL to acquire the required size 
// for the buffer, allocate buffer, then call the function again to retrieve 
// sensor information.
NTSTATUS AlsDevice::OnGetSupportedDataFields(
    _In_ SENSOROBJECT SensorInstance,          // Sensor device object
    _Inout_opt_ PSENSOR_PROPERTY_LIST pFields, // Pointer to a list of supported properties
    _Out_ PULONG pSize)                        // Number of bytes for the list of supported properties
{
    NTSTATUS Status = STATUS_SUCCESS;

    SENSOR_FunctionEnter();

    if (nullptr == pSize)
    {
        Status = STATUS_INVALID_PARAMETER;
        TraceError("ACC %!FUNC! pSize: Invalid parameter! %!STATUS!", Status);
    }
    else
    {
        *pSize = 0;

        // Get the device context
        PAlsDevice pDevice = GetAlsDeviceContextFromSensorInstance(SensorInstance);
        if (nullptr == pDevice)
        {
            Status = STATUS_INVALID_PARAMETER;
            TraceError("ACC %!FUNC! Invalid parameters! %!STATUS!", Status);
        }
        else if (nullptr == pFields)
        {
            // Just return size
            *pSize = pDevice->m_pSupportedDataFields->AllocatedSizeInBytes;
        }
        else
        {
            if (pFields->AllocatedSizeInBytes < pDevice->m_pSupportedDataFields->AllocatedSizeInBytes)
            {
                Status = STATUS_INSUFFICIENT_RESOURCES;
                TraceError("ACC %!FUNC! Buffer is too small. Failed %!STATUS!", Status);
            }
            else
            {
                // Fill out data
                Status = PropertiesListCopy(pFields, pDevice->m_pSupportedDataFields);
                if (!NT_SUCCESS(Status))
                {
                    TraceError("ACC %!FUNC! PropertiesListCopy failed %!STATUS!", Status);
                }
                else
                {
                    *pSize = pDevice->m_pSupportedDataFields->AllocatedSizeInBytes;
                }
            }
        }
    }

    SENSOR_FunctionExit(Status);
    return Status;
}

//------------------------------------------------------------------------------
// Function: OnGetProperties
//
// Called by Sensor CLX to get sensor properties. The typical usage is to call
// this function once with buffer pointer as NULL to acquire the required size
// for the buffer, allocate buffer, then call the function again to retrieve
// sensor information.
//
// Arguments:
//      SensorInstance: IN: sensor device object
//      pProperties: INOUT_OPT: pointer to a list of sensor properties
//      pSize: OUT: number of bytes for the list of sensor properties
//
// Return Value:
//      NTSTATUS code
//------------------------------------------------------------------------------
NTSTATUS
AlsDevice::OnGetProperties(
    _In_ SENSOROBJECT SensorInstance,
    _Inout_opt_ PSENSOR_COLLECTION_LIST pProperties,
    _Out_ PULONG pSize
)
{
    PAlsDevice pDevice = GetAlsDeviceContextFromSensorInstance(SensorInstance);
    NTSTATUS Status = STATUS_SUCCESS;

    SENSOR_FunctionEnter();

    if (nullptr == pDevice || nullptr == pSize)
    {
        Status = STATUS_INVALID_PARAMETER;
        TraceError("COMBO %!FUNC! Invalid parameters! %!STATUS!", Status);
        goto Exit;
    }

    if (nullptr == pProperties)
    {
        // Just return size
        *pSize = CollectionsListGetMarshalledSize(pDevice->m_pSensorProperties);
    }
    else
    {
        if (pProperties->AllocatedSizeInBytes <
            CollectionsListGetMarshalledSize(pDevice->m_pSensorProperties))
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            TraceError("COMBO %!FUNC! Buffer is too small. Failed %!STATUS!", Status);
            goto Exit;
        }

        // Fill out all data
        Status = CollectionsListCopyAndMarshall(pProperties, pDevice->m_pSensorProperties);
        if (!NT_SUCCESS(Status))
        {
            TraceError("COMBO %!FUNC! CollectionsListCopyAndMarshall failed %!STATUS!", Status);
            goto Exit;
        }

        *pSize = CollectionsListGetMarshalledSize(pDevice->m_pSensorProperties);
    }

Exit:
    if (!NT_SUCCESS(Status))
    {
        *pSize = 0;
    }
    SENSOR_FunctionExit(Status);
    return Status;
}

//------------------------------------------------------------------------------
// Function: OnGetDataFieldProperties
//
// Called by Sensor CLX to get data field properties. The typical usage is to call
// this function once with buffer pointer as NULL to acquire the required size
// for the buffer, allocate buffer, then call the function again to retrieve
// sensor information.
//
// Arguments:
//      SensorInstance: IN: sensor device object
//      DataField: IN: pointer to the propertykey of requested property
//      pProperties: INOUT_OPT: pointer to a list of sensor properties
//      pSize: OUT: number of bytes for the list of sensor properties
//
// Return Value:
//      NTSTATUS code
//------------------------------------------------------------------------------
NTSTATUS
AlsDevice::OnGetDataFieldProperties(
    _In_ SENSOROBJECT SensorInstance,
    _In_ const PROPERTYKEY* DataField,
    _Inout_opt_ PSENSOR_COLLECTION_LIST pProperties,
    _Out_ PULONG pSize
)
{
    PAlsDevice pDevice = GetAlsDeviceContextFromSensorInstance(SensorInstance);
    NTSTATUS Status = STATUS_SUCCESS;

    SENSOR_FunctionEnter();

    if (nullptr == pDevice || nullptr == pSize || nullptr == DataField)
    {
        Status = STATUS_INVALID_PARAMETER;
        TraceError("COMBO %!FUNC! Invalid parameters! %!STATUS!", Status);
        goto Exit;
    }

    if (IsKeyPresentInPropertyList(pDevice->m_pSupportedDataFields, DataField) != FALSE)
    {
        if (nullptr == pProperties)
        {
            // Just return size
            *pSize = CollectionsListGetMarshalledSize(pDevice->m_pDataFieldProperties);
        }
        else
        {
            if (pProperties->AllocatedSizeInBytes <
                CollectionsListGetMarshalledSize(pDevice->m_pDataFieldProperties))
            {
                Status = STATUS_INSUFFICIENT_RESOURCES;
                TraceError("COMBO %!FUNC! Buffer is too small. Failed %!STATUS!", Status);
                goto Exit;
            }

            // Fill out all data
            Status = CollectionsListCopyAndMarshall(pProperties, pDevice->m_pDataFieldProperties);
            if (!NT_SUCCESS(Status))
            {
                TraceError("COMBO %!FUNC! CollectionsListCopyAndMarshall failed %!STATUS!", Status);
                goto Exit;
            }

            *pSize = CollectionsListGetMarshalledSize(pDevice->m_pDataFieldProperties);
        }
    }
    else
    {
        Status = STATUS_NOT_SUPPORTED;
        TraceError("COMBO %!FUNC! Sensor does NOT have properties for this data field. Failed %!STATUS!", Status);
        goto Exit;
    }

Exit:
    if (!NT_SUCCESS(Status))
    {
        *pSize = 0;
    }
    SENSOR_FunctionExit(Status);
    return Status;
}

// Called by Sensor CLX to handle IOCTLs that clx does not support
NTSTATUS AlsDevice::OnIoControl(
    _In_ SENSOROBJECT /*SensorInstance*/, // WDF queue object
    _In_ WDFREQUEST /*Request*/,          // WDF request object
    _In_ size_t /*OutputBufferLength*/,   // number of bytes to retrieve from output buffer
    _In_ size_t /*InputBufferLength*/,    // number of bytes to retrieve from input buffer
    _In_ ULONG /*IoControlCode*/)         // IOCTL control code
{
    NTSTATUS Status = STATUS_NOT_SUPPORTED;

    SENSOR_FunctionEnter();

    SENSOR_FunctionExit(Status);
    return Status;
}

// Called by Sensor CLX to get sampling rate of the sensor.
NTSTATUS AlsDevice::OnGetDataInterval(
    _In_ SENSOROBJECT SensorInstance,   // Sensor device object
    _Out_ PULONG pDataRateMs)           // Sampling rate in milliseconds
{
    NTSTATUS Status = STATUS_SUCCESS;

    SENSOR_FunctionEnter();

    PAlsDevice pDevice = GetAlsDeviceContextFromSensorInstance(SensorInstance);
    if (nullptr == pDevice)
    {
        Status = STATUS_INVALID_PARAMETER;
        TraceError("ACC %!FUNC! Invalid parameters! %!STATUS!", Status);
    }
    else if (nullptr == pDataRateMs)
    {
        Status = STATUS_INVALID_PARAMETER;
        TraceError("ACC %!FUNC! Invalid parameters! %!STATUS!", Status);
    }
    else
    {
        *pDataRateMs = pDevice->m_Interval;
        TraceInformation("%!FUNC! giving data rate %lu", *pDataRateMs);
    }

    SENSOR_FunctionExit(Status);
    return Status;
}

//------------------------------------------------------------------------------
// Function: OnSetDataInterval
//
// Called by Sensor CLX to set sampling rate of the sensor.
//
// Arguments:
//      SensorInstance: IN: sensor device object
//      DataRateMs: IN: sampling rate in ms
//
// Return Value:
//      NTSTATUS code
//------------------------------------------------------------------------------
NTSTATUS AlsDevice::OnSetDataInterval(
    _In_ SENSOROBJECT SensorInstance, // Sensor device object
    _In_ ULONG DataRateMs)            // Sampling rate in milliseconds
{
    NTSTATUS Status = STATUS_SUCCESS;

    SENSOR_FunctionEnter();

    // Get the device context
    PAlsDevice pDevice = GetAlsDeviceContextFromSensorInstance(SensorInstance);

    if (pDevice == nullptr || DataRateMs == 0)
    {
        Status = STATUS_INVALID_PARAMETER;
        TraceError("COMBO %!FUNC! Invalid parameter!");
    }


    if (NT_SUCCESS(Status))
    {
        pDevice->m_Interval = DataRateMs;

        // reschedule sample to return as soon as possible if it's started
        if (FALSE != pDevice->m_Started)
        {
            pDevice->m_Started = FALSE;
            WdfTimerStop(pDevice->m_Timer, TRUE);

            pDevice->m_Started = TRUE;
            pDevice->m_FirstSample = TRUE;
            WdfTimerStart(pDevice->m_Timer, WDF_REL_TIMEOUT_IN_MS(pDevice->m_MinimumInterval));
        }
    }

    SENSOR_FunctionExit(Status);
    return Status;
}

//------------------------------------------------------------------------------
// Function: OnGetDataThresholds
//
// Called by Sensor CLX to get data thresholds. The typical usage is to call
// this function once with buffer pointer as NULL to acquire the required size
// for the buffer, allocate buffer, then call the function again to retrieve
// sensor information.
//
// Arguments:
//      SensorInstance: IN: sensor device object
//      pThresholds: INOUT_OPT: pointer to a list of sensor thresholds
//      pSize: OUT: number of bytes for the list of sensor thresholds
//
// Return Value:
//      NTSTATUS code
//------------------------------------------------------------------------------
NTSTATUS AlsDevice::OnGetDataThresholds(
    _In_ SENSOROBJECT SensorInstance,                   // Sensor Device Object
    _Inout_opt_ PSENSOR_COLLECTION_LIST pThresholds,    // Pointer to a list of sensor thresholds
    _Out_ PULONG pSize)                                 // Number of bytes for the list of sensor thresholds
{

    NTSTATUS Status = STATUS_SUCCESS;

    SENSOR_FunctionEnter();

    if (nullptr == pSize)
    {
        Status = STATUS_INVALID_PARAMETER;
        TraceError("ACC %!FUNC! pSize: Invalid parameter! %!STATUS!", Status);
    }
    else
    {
        *pSize = 0;

        PAlsDevice pDevice = GetAlsDeviceContextFromSensorInstance(SensorInstance);
        if (nullptr == pDevice)
        {
            Status = STATUS_INVALID_PARAMETER;
            TraceError("ACC %!FUNC! Invalid parameters! %!STATUS!", Status);
        }
        else if (nullptr == pThresholds)
        {
            // Just return size
            *pSize = CollectionsListGetMarshalledSize(pDevice->m_pThresholds);
        }
        else
        {
            if (pThresholds->AllocatedSizeInBytes < CollectionsListGetMarshalledSize(pDevice->m_pThresholds))
            {
                Status = STATUS_INSUFFICIENT_RESOURCES;
                TraceError("ACC %!FUNC! Buffer is too small. Failed %!STATUS!", Status);
            }
            else
            {
                // Fill out all data
                Status = CollectionsListCopyAndMarshall(pThresholds, pDevice->m_pThresholds);
                if (!NT_SUCCESS(Status))
                {
                    TraceError("ACC %!FUNC! CollectionsListCopyAndMarshall failed %!STATUS!", Status);
                }
                else
                {
                    *pSize = CollectionsListGetMarshalledSize(pDevice->m_pThresholds);
                }
            }
        }
    }

    SENSOR_FunctionExit(Status);
    return Status;
}

// Called by Sensor CLX to set data thresholds.
NTSTATUS AlsDevice::OnSetDataThresholds(
    _In_ SENSOROBJECT SensorInstance,           // Sensor Device Object
    _In_ PSENSOR_COLLECTION_LIST pThresholds)   // Pointer to a list of sensor thresholds
{
    NTSTATUS Status = STATUS_SUCCESS;

    SENSOR_FunctionEnter();

    PAlsDevice pDevice = GetAlsDeviceContextFromSensorInstance(SensorInstance);
    if (nullptr == pDevice)
    {
        Status = STATUS_INVALID_PARAMETER;
        TraceError("ACC %!FUNC! Sensor(0x%p) parameter is invalid %!STATUS!", SensorInstance, Status);
    }

    else // if (NT_SUCCESS(Status))
    {
        for (ULONG i = 0; i < pThresholds->Count; i++)
        {
            Status = PropKeyFindKeySetPropVariant(pDevice->m_pThresholds, &(pThresholds->List[i].Key), true, &(pThresholds->List[i].Value));
            if (!NT_SUCCESS(Status))
            {
                Status = STATUS_INVALID_PARAMETER;
                TraceError("ACC %!FUNC! Sensor does NOT have threshold for this data field. Failed %!STATUS!", Status);
                break;
            }
        }
    }

    // Update cached threshholds
    if (NT_SUCCESS(Status))
    {
        Status = pDevice->UpdateCachedThreshold();
        if (!NT_SUCCESS(Status))
        {
            TraceError("COMBO %!FUNC! UpdateCachedThreshold failed! %!STATUS!", Status);

            SENSOR_FunctionExit(Status);
            return Status;
        }
    }

    SENSOR_FunctionExit(Status);
    return Status;
}

//------------------------------------------------------------------------------
// Function: UpdateCachedThreshold
//
// This routine updates the cached threshold
//
// Arguments:
//       None
//
// Return Value:
//      NTSTATUS code
//------------------------------------------------------------------------------
NTSTATUS
AlsDevice::UpdateCachedThreshold(
)
{
    NTSTATUS status;

    SENSOR_FunctionEnter();

    status = PropKeyFindKeyGetFloat(m_pThresholds,
        &PKEY_SensorData_LightLevel_Lux, &m_CachedThresholds.LuxPct);
    if (!NT_SUCCESS(status))
    {
        TraceError("COMBO %!FUNC! Failed to get lux pct data from cached threshold %!STATUS!", status);

        SENSOR_FunctionExit(status);
        return status;
    }

    status = PropKeyFindKeyGetFloat(m_pThresholds,
        &PKEY_SensorData_LightLevel_Lux_Threshold_AbsoluteDifference, &m_CachedThresholds.LuxAbs);
    if (!NT_SUCCESS(status))
    {
        TraceError("COMBO %!FUNC! Failed to get lux abs data from cached threshold %!STATUS!", status);

        SENSOR_FunctionExit(status);
        return status;
    }

    SENSOR_FunctionExit(status);
    return status;
}

// Services a hardware interrupt.
BOOLEAN AlsDevice::OnInterruptIsr(
    _In_ WDFINTERRUPT Interrupt,        // Handle to a framework interrupt object
    _In_ ULONG /*MessageID*/)           // If the device is using message-signaled interrupts (MSIs),
                                        // this parameter is the message number that identifies the
                                        // device's hardware interrupt message. Otherwise, this value is 0.
{
    BOOLEAN InterruptRecognized = FALSE;
    PAlsDevice pDevice = nullptr;

    SENSOR_FunctionEnter();

    // Get the sensor instance
    ULONG SensorInstanceCount = 1;
    SENSOROBJECT SensorInstance = NULL;
    NTSTATUS Status = SensorsCxDeviceGetSensorList(WdfInterruptGetDevice(Interrupt), &SensorInstance, &SensorInstanceCount);
    if (!NT_SUCCESS(Status) || 0 == SensorInstanceCount || NULL == SensorInstance)
    {
        Status = STATUS_INVALID_PARAMETER;
        TraceError("ACC %!FUNC! SensorsCxDeviceGetSensorList failed %!STATUS!", Status);
    }

    // Get the device context
    else // if (NT_SUCCESS(Status))
    {
        pDevice = GetAlsDeviceContextFromSensorInstance(SensorInstance);
        if (nullptr == pDevice)
        {
            Status = STATUS_INVALID_PARAMETER;
            TraceError("ACC %!FUNC! GetAlsDeviceContextFromSensorInstance failed %!STATUS!", Status);
        }
    }

    // Read the interrupt source
    if (NT_SUCCESS(Status))
    {
        BYTE IntSrcBuffer = 0;
        WdfWaitLockAcquire(pDevice->m_I2CWaitLock, NULL);
        Status = I2CSensorReadRegister(pDevice->m_I2CIoTarget, ISL29018_REG_ADD_COMMAND1, &IntSrcBuffer, sizeof(IntSrcBuffer));
        WdfWaitLockRelease(pDevice->m_I2CWaitLock);

        if (!NT_SUCCESS(Status))
        {
            TraceError("ACC %!FUNC! I2CSensorReadRegister from 0x%02x failed! %!STATUS!", ISL29018_REG_ADD_COMMAND1, Status);
        }
        else if ((IntSrcBuffer & ISL29018_CMD1_ISR_MASK) == 0)
        {
            TraceError("%!FUNC! Interrupt source not recognized");
        }
        else
        {
            InterruptRecognized = TRUE;
            BOOLEAN WorkItemQueued = WdfInterruptQueueWorkItemForIsr(Interrupt);
            TraceVerbose("%!FUNC! Work item %s queued for interrupt", WorkItemQueued ? "" : " already");
        }
    }

    SENSOR_FunctionExit(Status);
    return InterruptRecognized;
}

// Processes interrupt information that the driver's EvtInterruptIsr callback function has stored.
VOID AlsDevice::OnInterruptWorkItem(
    _In_ WDFINTERRUPT Interrupt,            // Handle to a framework object
    _In_ WDFOBJECT /*AssociatedObject*/)    // A handle to the framework device object that 
                                            // the driver passed to WdfInterruptCreate.
{
    PAlsDevice pDevice = nullptr;

    SENSOR_FunctionEnter();

    // Get the sensor instance
    ULONG SensorInstanceCount = 1;
    SENSOROBJECT SensorInstance = NULL;
    NTSTATUS Status = SensorsCxDeviceGetSensorList(WdfInterruptGetDevice(Interrupt), &SensorInstance, &SensorInstanceCount);
    if (!NT_SUCCESS(Status) || 0 == SensorInstanceCount || NULL == SensorInstance)
    {
        Status = STATUS_INVALID_PARAMETER;
        TraceError("ACC %!FUNC! SensorsCxDeviceGetSensorList failed %!STATUS!", Status);
    }

    // Get the device context
    else //if (NT_SUCCESS(Status))
    {
        pDevice = GetAlsDeviceContextFromSensorInstance(SensorInstance);
        if (nullptr == pDevice)
        {
            Status = STATUS_INVALID_PARAMETER;
            TraceError("ACC %!FUNC! GetAlsDeviceContextFromSensorInstance failed %!STATUS!", Status);
        }
    }

    // Read the device data
    if (NT_SUCCESS(Status))
    {
        WdfInterruptAcquireLock(Interrupt);
        Status = pDevice->GetData();
        WdfInterruptReleaseLock(Interrupt);
        if (!NT_SUCCESS(Status) && STATUS_DATA_NOT_ACCEPTED != Status)
        {
            TraceError("ACC %!FUNC! GetData failed %!STATUS!", Status);
        }
    }

    SENSOR_FunctionExit(Status);
}

//------------------------------------------------------------------------------
// Function: OnTimerExpire
//
// This callback is called when interval wait time has expired and driver is ready
// to collect new sample. The callback reads current value, compare value to threshold,
// pushes it up to CLX framework, and schedule next wake up time.
//
// Arguments:
//      Timer: IN: WDF timer object
//
// Return Value:
//      None
//------------------------------------------------------------------------------
VOID AlsDevice::OnTimerExpire(
    _In_ WDFTIMER Timer
)
{
    PAlsDevice pDevice = nullptr;
    NTSTATUS Status = STATUS_SUCCESS;

    SENSOR_FunctionEnter();

    pDevice = GetAlsDeviceContextFromSensorInstance(WdfTimerGetParentObject(Timer));
    if (pDevice == nullptr)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        TraceError("COMBO %!FUNC! GetContextFromSensorInstance failed %!STATUS!", Status);
        goto Exit;
    }

    // Get data and push to clx
    WdfWaitLockAcquire(pDevice->m_I2CWaitLock, NULL);
    Status = pDevice->GetData();
    if (!NT_SUCCESS(Status) && Status != STATUS_DATA_NOT_ACCEPTED)
    {
        TraceError("COMBO %!FUNC! GetData Failed %!STATUS!", Status);
    }
    WdfWaitLockRelease(pDevice->m_I2CWaitLock);

    // Schedule next wake up time
    if (pDevice->m_MinimumInterval <= pDevice->m_Interval &&
        FALSE != pDevice->m_PoweredOn &&
        FALSE != pDevice->m_Started)
    {
        LONGLONG WaitTime = 0;  // in unit of 100ns

        if (pDevice->m_StartTime == 0)
        {
            // in case we fail to get sensor start time, use static wait time
            WaitTime = WDF_REL_TIMEOUT_IN_MS(pDevice->m_Interval);
        }
        else
        {
            ULONG CurrentTimeMs = 0;

            // dynamically calculate wait time to avoid jitter
            Status = GetPerformanceTime(&CurrentTimeMs);
            if (!NT_SUCCESS(Status))
            {
                TraceError("COMBO %!FUNC! GetPerformanceTime %!STATUS!", Status);
                WaitTime = WDF_REL_TIMEOUT_IN_MS(pDevice->m_Interval);
            }
            else
            {
                pDevice->m_SampleCount++;
                if (CurrentTimeMs > (pDevice->m_StartTime + (pDevice->m_Interval * (pDevice->m_SampleCount + 1))))
                {
                    // If we skipped two or more beats, reschedule the timer with a zero due time to catch up on missing samples
                    WaitTime = 0;
                }
                else
                {
                    // Else, just compute the remaining time
                    WaitTime = (pDevice->m_StartTime +
                        (pDevice->m_Interval * (pDevice->m_SampleCount + 1))) - CurrentTimeMs;
                }

                WaitTime = WDF_REL_TIMEOUT_IN_MS(WaitTime);
            }
        }

        WdfTimerStart(pDevice->m_Timer, WaitTime);
    }

Exit:

    SENSOR_FunctionExit(Status);
}
