//Copyright (C) Microsoft Corporation, All Rights Reserved.
//
//Abstract:
//
//    This module contains the implementation of WDF callback functions 
//    for the ISL29018 ambient light sensor driver.
//
//Environment:
//
//   Windows User-Mode Driver Framework (UMDF)

#include "Device.h"

#include "Device.tmh"


// This routine is the AddDevice entry point for the custom sensor client
// driver. This routine is called by the framework in response to AddDevice
// call from the PnP manager. It will create and initialize the device object
// to represent a new instance of the sensor client.
NTSTATUS AlsDevice::OnDeviceAdd(
    _In_    WDFDRIVER /* Driver */,         // Supplies a handle to the driver object created in DriverEntry
    _Inout_ PWDFDEVICE_INIT pDeviceInit) // Supplies a pointer to a framework-allocated WDFDEVICE_INIT structure
{    
    WDFDEVICE Device = nullptr;
    NTSTATUS status;
    WDF_OBJECT_ATTRIBUTES attributes;     
    SENSOR_CONTROLLER_CONFIG config;
    WDF_PNPPOWER_EVENT_CALLBACKS pnpPowerCallbacks;
    WDF_DEVICE_STATE deviceState;

    SENSOR_FunctionEnter();

    WdfDeviceInitSetPowerPolicyOwnership(pDeviceInit, true);
    
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);

    // Initialize FDO (functional device object) attributes and set up file object with sensor extension
    status = SensorsCxDeviceInitConfig(pDeviceInit, &attributes, 0);
    if (!NT_SUCCESS(status))
    {
        TraceError("ACC %!FUNC! SensorsCxDeviceInitConfig failed %!STATUS!", status);

        SENSOR_FunctionExit(status);
        return status;
    }    

    // Register the PnP callbacks with the framework.
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);
    pnpPowerCallbacks.EvtDevicePrepareHardware = AlsDevice::OnPrepareHardware;
    pnpPowerCallbacks.EvtDeviceReleaseHardware = AlsDevice::OnReleaseHardware;
    pnpPowerCallbacks.EvtDeviceD0Entry = AlsDevice::OnD0Entry;
    pnpPowerCallbacks.EvtDeviceD0Exit = AlsDevice::OnD0Exit;
    
    WdfDeviceInitSetPnpPowerEventCallbacks(pDeviceInit, &pnpPowerCallbacks);
    
    // Call the framework to create the device
    status = WdfDeviceCreate(&pDeviceInit, &attributes, &Device);
    if (!NT_SUCCESS(status))
    {
        TraceError("ACC %!FUNC! WdfDeviceCreate failed %!STATUS!", status);

        SENSOR_FunctionExit(status);
        return status;
    }   
     
    // Register CLX callback function pointers
        
    SENSOR_CONTROLLER_CONFIG_INIT(&config);
    config.DriverIsPowerPolicyOwner = WdfUseDefault;
    
    config.EvtSensorStart = AlsDevice::OnStart;
    config.EvtSensorStop = AlsDevice::OnStop;
    config.EvtSensorGetSupportedDataFields = AlsDevice::OnGetSupportedDataFields;
    config.EvtSensorGetDataInterval = AlsDevice::OnGetDataInterval;
    config.EvtSensorSetDataInterval = AlsDevice::OnSetDataInterval;
    config.EvtSensorGetDataFieldProperties = AlsDevice::OnGetDataFieldProperties;
    config.EvtSensorGetDataThresholds = AlsDevice::OnGetDataThresholds;
    config.EvtSensorSetDataThresholds = AlsDevice::OnSetDataThresholds;
    config.EvtSensorGetProperties = AlsDevice::OnGetProperties;
    config.EvtSensorDeviceIoControl = AlsDevice::OnIoControl;
    
    // Initialize the sensor device with the Sensor CLX
    // This lets the CLX call the above callbacks when
    // necessary and allows applications to retrieve and
    // set device data.
    status = SensorsCxDeviceInitialize(Device, &config);
    if (!NT_SUCCESS(status))
    {
        TraceError("ACC %!FUNC! SensorsCxDeviceInitialize failed %!STATUS!", status);

        SENSOR_FunctionExit(status);
        return status;
    }
    

    // Ensure device is disable-able
    // By default, devices enumerated by ACPI are not disable-able
    // Our accelerometer is enumerated by the ACPI so we must
    // explicitly make it disable-able.
    WDF_DEVICE_STATE_INIT(&deviceState);
    deviceState.NotDisableable = WdfFalse;
    WdfDeviceSetDeviceState(Device, &deviceState);


    SENSOR_FunctionExit(status);
    return status;
}

// This routine is called by the framework when the PnP manager sends an
// IRP_MN_START_DEVICE request to the driver stack. This routine is
// responsible for performing operations that are necessary to make the
// driver's device operational (for e.g. mapping the hardware resources
// into memory).
NTSTATUS AlsDevice::OnPrepareHardware(
    _In_ WDFDEVICE Device,                  // Supplies a handle to the framework device object
    _In_ WDFCMRESLIST ResourcesRaw,         // Supplies a handle to a collection of framework resource
                                            // objects. This collection identifies the raw (bus-relative) hardware
                                            // resources that have been assigned to the device.
    _In_ WDFCMRESLIST ResourcesTranslated)  // Supplies a handle to a collection of framework
                                            // resource objects. This collection identifies the translated
                                            // (system-physical) hardware resources that have been assigned to the
                                            // device. The resources appear from the CPU's point of view.
{
    PAlsDevice pDevice = nullptr;
    NTSTATUS status;
    SENSOROBJECT SensorInstance = NULL;
    SENSOR_CONFIG SensorConfig;
    
    SENSOR_FunctionEnter();

    // Create WDFOBJECT for the sensor
    WDF_OBJECT_ATTRIBUTES sensorAttributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&sensorAttributes, AlsDevice);

    // Register sensor instance with clx
    
    status = SensorsCxSensorCreate(Device, &sensorAttributes, &SensorInstance);
    if (!NT_SUCCESS(status))
    {
        TraceError("ACC %!FUNC! SensorsCxSensorCreate failed %!STATUS!", status);

        SENSOR_FunctionExit(status);
        return status;
    }
 
    pDevice = GetAlsDeviceContextFromSensorInstance(SensorInstance);
    if (nullptr == pDevice)
    {
        status = STATUS_INSUFFICIENT_RESOURCES;
        TraceError("ACC %!FUNC! SensorsCxSensorCreate failed %!STATUS!", status);

        SENSOR_FunctionExit(status);
        return status;
    }

    // Fill out sensor context
    status = pDevice->Initialize(Device, SensorInstance);
    if (!NT_SUCCESS(status))
    {
        TraceError("ACC %!FUNC! Initialize device object failed %!STATUS!", status);

        SENSOR_FunctionExit(status);
        return status;
    }

    // Initialize sensor instance with clx    
    SENSOR_CONFIG_INIT(&SensorConfig);
    SensorConfig.pEnumerationList = pDevice->m_pEnumerationProperties;
    status = SensorsCxSensorInitialize(SensorInstance, &SensorConfig);
    if (!NT_SUCCESS(status))
    {
        TraceError("ACC %!FUNC! SensorsCxSensorInitialize failed %!STATUS!", status);

        SENSOR_FunctionExit(status);
        return status;
    }
    
    
    // ACPI and IoTarget configuration
    status = pDevice->ConfigureIoTarget(ResourcesRaw, ResourcesTranslated);
    if (!NT_SUCCESS(status))
    {
        TraceError("ACC %!FUNC! Failed to configure IoTarget %!STATUS!", status);

        SENSOR_FunctionExit(status);
        return status;
    }    

    SENSOR_FunctionExit(status);
    return status;
}

// This routine is called by the framework when the PnP manager is revoking
// ownership of our resources. This may be in response to either
// IRP_MN_STOP_DEVICE or IRP_MN_REMOVE_DEVICE. This routine is responsible for
// performing cleanup of resources allocated in PrepareHardware callback.
// This callback is invoked before passing  the request down to the lower driver.
// This routine will also be invoked by the framework if the prepare hardware
// callback returns a failure.
NTSTATUS AlsDevice::OnReleaseHardware(
    _In_ WDFDEVICE Device,                      // Supplies a handle to the framework device object
    _In_ WDFCMRESLIST /*ResourcesTranslated*/)  // Supplies a handle to a collection of framework
                                                // resource objects. This collection identifies the translated
                                                // (system-physical) hardware resources that have been assigned to the
                                                // device. The resources appear from the CPU's point of view.
{
    PAlsDevice pDevice = nullptr;
    NTSTATUS status;
    ULONG SensorInstanceCount = 1;
    SENSOROBJECT SensorInstance = NULL;

    SENSOR_FunctionEnter();

    // Get the sensor instance
    status = SensorsCxDeviceGetSensorList(Device, &SensorInstance, &SensorInstanceCount);
    if (!NT_SUCCESS(status) || 0 == SensorInstanceCount || NULL == SensorInstance)
    {
        status = STATUS_INVALID_PARAMETER;
        TraceError("ACC %!FUNC! SensorsCxDeviceGetSensorList failed %!STATUS!", status);

        SENSOR_FunctionExit(status);
        return status;
    }

    pDevice = GetAlsDeviceContextFromSensorInstance(SensorInstance);
    if (nullptr == pDevice)
    {
        status = STATUS_INVALID_PARAMETER;
        TraceError("ACC %!FUNC! GetAlsDeviceContextFromSensorInstance failed %!STATUS!", status);

        SENSOR_FunctionExit(status);
        return status;
    }

    pDevice->DeInit();

    SENSOR_FunctionExit(status);
    return status;
}

// This routine is invoked by the framework to program the device to goto 
// D0, which is the working state. The framework invokes callback every
// time the hardware needs to be (re-)initialized.  This includes after
// IRP_MN_START_DEVICE, IRP_MN_CANCEL_STOP_DEVICE, IRP_MN_CANCEL_REMOVE_DEVICE,
// and IRP_MN_SET_POWER-D0.
NTSTATUS AlsDevice::OnD0Entry(
    _In_  WDFDEVICE Device,                         // Supplies a handle to the framework device object
    _In_  WDF_POWER_DEVICE_STATE /*PreviousState*/) // WDF_POWER_DEVICE_STATE-typed enumerator that identifies
                                                    // the device power state that the device was in before this transition to D0
{
    PAlsDevice pDevice = nullptr;
    NTSTATUS status;
    ULONG SensorInstanceCount = 1;
    SENSOROBJECT SensorInstance = NULL;

    SENSOR_FunctionEnter();

    // Get the sensor instance    
    status = SensorsCxDeviceGetSensorList(Device, &SensorInstance, &SensorInstanceCount);
    if (!NT_SUCCESS(status) || 0 == SensorInstanceCount || NULL == SensorInstance)
    {
        status = STATUS_INVALID_PARAMETER;
        TraceError("ACC %!FUNC! SensorsCxDeviceGetSensorList failed %!STATUS!", status);

        SENSOR_FunctionExit(status);
        return status;
    }

    // Get the device context
    pDevice = GetAlsDeviceContextFromSensorInstance(SensorInstance);
    if (nullptr == pDevice)
    {
        status = STATUS_INVALID_PARAMETER;
        TraceError("ACC %!FUNC! GetAlsDeviceContextFromSensorInstance failed %!STATUS!", status);

        SENSOR_FunctionExit(status);
        return status;
    }
    

    status = pDevice->PowerOn();

    SENSOR_FunctionExit(status);
    return status;
}

// This routine is invoked by the framework to program the device to go into
// a certain Dx state. The framework invokes callback every the the device is 
// leaving the D0 state, which happens when the device is stopped, when it is 
// removed, and when it is powered off. EvtDeviceD0Exit event callback must 
// perform any operations that are necessary before the specified device is 
// moved out of the D0 state.  If the driver needs to save hardware state 
// before the device is powered down, then that should be done here.
NTSTATUS AlsDevice::OnD0Exit(
    _In_ WDFDEVICE Device,                      // Supplies a handle to the framework device object
    _In_ WDF_POWER_DEVICE_STATE)/*TargetState*/ // Supplies the device power state which the device will be put
                                                // in once the callback is complete
{
    PAlsDevice pDevice = nullptr;
    NTSTATUS status;
    ULONG SensorInstanceCount = 1;
    SENSOROBJECT SensorInstance = NULL;

    SENSOR_FunctionEnter();

    // Get the sensor instance
    status = SensorsCxDeviceGetSensorList(Device, &SensorInstance, &SensorInstanceCount);
    if (!NT_SUCCESS(status) || 0 == SensorInstanceCount || NULL == SensorInstance)
    {
        status = STATUS_INVALID_PARAMETER;
        TraceError("ACC %!FUNC! SensorsCxDeviceGetSensorList failed %!STATUS!", status);

        SENSOR_FunctionExit(status);
        return status;
    }

    // Get the device context   
    pDevice = GetAlsDeviceContextFromSensorInstance(SensorInstance);
    if (nullptr == pDevice)
    {
        status = STATUS_INVALID_PARAMETER;
        TraceError("ACC %!FUNC! GetAlsDeviceContextFromSensorInstance failed %!STATUS!", status);

        SENSOR_FunctionExit(status);
        return status;
    }

    status = pDevice->PowerOff();

    SENSOR_FunctionExit(status);
    return status;
}

// Get the HW resource from the ACPI, then configure and store the IoTarget
NTSTATUS AlsDevice::ConfigureIoTarget(
    _In_ WDFCMRESLIST ResourcesRaw,         // Supplies a handle to a collection of framework resource
                                            // objects. This collection identifies the raw (bus-relative) hardware
                                            // resources that have been assigned to the device.
    _In_ WDFCMRESLIST ResourcesTranslated)  // Supplies a handle to a collection of framework
                                            // resource objects. This collection identifies the translated
                                            // (system-physical) hardware resources that have been assigned to the
                                            // device. The resources appear from the CPU's point of view.
{
    NTSTATUS status;
    ULONG I2CConnectionResourceCount = 0;
    LARGE_INTEGER I2CConnId = {};
    WDF_IO_TARGET_OPEN_PARAMS OpenParams;

    DECLARE_UNICODE_STRING_SIZE(deviceName, RESOURCE_HUB_PATH_SIZE);

    SENSOR_FunctionEnter();

    // Get hardware resource from ACPI and set up IO target
    ULONG ResourceCount = WdfCmResourceListGetCount(ResourcesTranslated);
    for (ULONG i = 0; i < ResourceCount; i++)
    {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR DescriptorRaw = WdfCmResourceListGetDescriptor(ResourcesRaw, i);
        PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor = WdfCmResourceListGetDescriptor(ResourcesTranslated, i);
        switch (Descriptor->Type) 
        {
            // Check we have I2C bus assigned in ACPI
            case CmResourceTypeConnection:
                TraceInformation("ACC %!FUNC! I2C resource found.");
                if (Descriptor->u.Connection.Class == CM_RESOURCE_CONNECTION_CLASS_SERIAL &&
                    Descriptor->u.Connection.Type == CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C) 
                {
                    I2CConnId.LowPart = Descriptor->u.Connection.IdLowPart;
                    I2CConnId.HighPart = Descriptor->u.Connection.IdHighPart;
                    I2CConnectionResourceCount++;
                }
                break;
    
            // Check we have an interrupt assigned in ACPI and create interrupt
            case CmResourceTypeInterrupt:
                TraceInformation("ACC %!FUNC! GPIO interrupt resource found.");

                WDF_INTERRUPT_CONFIG InterruptConfig;

                WDF_INTERRUPT_CONFIG_INIT(&InterruptConfig, OnInterruptIsr, NULL);
                InterruptConfig.InterruptRaw = DescriptorRaw;
                InterruptConfig.InterruptTranslated = Descriptor;

                // Configure an interrupt work item which runs at IRQL = PASSIVE_LEVEL
                // Note: to configure to run at IRQL = DISPATCH_LEVEL, set up an InterruptDpc instead of an InterruptWorkItem
                InterruptConfig.EvtInterruptWorkItem = OnInterruptWorkItem;
                InterruptConfig.PassiveHandling = true;

                status = WdfInterruptCreate(m_Device, &InterruptConfig, WDF_NO_OBJECT_ATTRIBUTES, &m_Interrupt);
                if (!NT_SUCCESS(status))
                {
                    TraceError("ACC %!FUNC! WdfInterruptCreate failed %!STATUS!", status);

                    SENSOR_FunctionExit(status);
                    return status;
                }
                break;

            default:
                break;
        }
    }

    if (I2CConnectionResourceCount != 1)
    {
        status = STATUS_UNSUCCESSFUL;
        TraceError("ACC %!FUNC! Did not find I2C resource! %!STATUS!", status);

        SENSOR_FunctionExit(status);
        return status;
    }

    // Set up I2C I/O target. Issued with I2C R/W transfers
    m_I2CIoTarget = NULL;
    status = WdfIoTargetCreate(m_Device, WDF_NO_OBJECT_ATTRIBUTES, &m_I2CIoTarget);
    
    if (!NT_SUCCESS(status))
    {
        TraceError("ACC %!FUNC! WdfIoTargetCreate failed! %!STATUS!", status);

        SENSOR_FunctionExit(status);
        return status;
    }
    

    // Setup Target string (\\\\.\\RESOURCE_HUB\\<ConnID from ResHub>
    status = StringCbPrintfW(deviceName.Buffer, RESOURCE_HUB_PATH_SIZE, L"%s\\%0*I64x",
        RESOURCE_HUB_DEVICE_NAME, static_cast<unsigned int>(sizeof(LARGE_INTEGER) * 2), I2CConnId.QuadPart);
    deviceName.Length = _countof(deviceName_buffer);
    
    if (!NT_SUCCESS(status))
    {
        TraceError("ACC %!FUNC! RESOURCE_HUB_CREATE_PATH_FROM_ID failed!");

        SENSOR_FunctionExit(status);
        return status;
    }    

    // Connect to I2C target
    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&OpenParams, &deviceName, FILE_ALL_ACCESS);
    
    status = WdfIoTargetOpen(m_I2CIoTarget, &OpenParams);
    if (!NT_SUCCESS(status))
    {
        TraceError("ACC %!FUNC! WdfIoTargetOpen failed! %!STATUS!", status);

        SENSOR_FunctionExit(status);
        return status;
    }    

    SENSOR_FunctionExit(status);
    return status;
}

// Write the default device configuration to the device
NTSTATUS AlsDevice::PowerOn()
{
    NTSTATUS status = STATUS_SUCCESS;

    WdfWaitLockAcquire(m_I2CWaitLock, NULL);
    
    for (DWORD i = 0; i < ARRAYSIZE(g_ConfigurationSettings); i++)
    {
        REGISTER_SETTING setting = g_ConfigurationSettings[i];
        status = I2CSensorWriteRegister(m_I2CIoTarget, setting.Register, &setting.Value, sizeof(setting.Value));

        if (!NT_SUCCESS(status))
        {
            TraceError("ACC %!FUNC! I2CSensorReadRegister from 0x%02x failed! %!STATUS!", setting.Register, status);
            WdfWaitLockRelease(m_I2CWaitLock);

            return status;
        }
    }

    WdfWaitLockRelease(m_I2CWaitLock);

    InitPropVariantFromUInt32(SensorState_Idle, &(m_pSensorProperties->List[SENSOR_PROPERTY_STATE].Value));

    m_PoweredOn = true;
    return status;
}

NTSTATUS AlsDevice::PowerOff()
{
    NTSTATUS status;
    REGISTER_SETTING setting = { ISL29018_REG_ADD_COMMAND1, ISL29018_CMD1_OPMODE_POWER_DOWN << ISL29018_CMD1_OPMODE_SHIFT };
    
    WdfWaitLockAcquire(m_I2CWaitLock, NULL);
    status = I2CSensorWriteRegister(m_I2CIoTarget, setting.Register, &setting.Value, sizeof(setting.Value));
    WdfWaitLockRelease(m_I2CWaitLock);
        
    if (!NT_SUCCESS(status))
    {
        TraceError("ACC %!FUNC! Failed to put device into standby %!STATUS!", status);

        return status;
    }

    m_PoweredOn = false;
    return status;
}


// Write the default device configuration to the device
NTSTATUS AlsDevice::IsrOn()
{
    NTSTATUS status = STATUS_SUCCESS;
    REGISTER_SETTING settings[] =
    {
        { ISL29018_REG_ADD_INT_LT_LSB, 0xFF },
        { ISL29018_REG_ADD_INT_LT_MSB, 0xFF },
        { ISL29018_REG_ADD_INT_HT_LSB, 0x00 },
        { ISL29018_REG_ADD_INT_HT_MSB, 0x00 },
    };

    WdfWaitLockAcquire(m_I2CWaitLock, NULL);
    for (DWORD i = 0; i < ARRAYSIZE(settings); i++)
    {
        REGISTER_SETTING setting = settings[i];
        status = I2CSensorWriteRegister(m_I2CIoTarget, setting.Register, &setting.Value, sizeof(setting.Value));

        if (!NT_SUCCESS(status))
        {
            TraceError("ACC %!FUNC! I2CSensorReadRegister from 0x%02x failed! %!STATUS!", setting.Register, status);
            WdfWaitLockRelease(m_I2CWaitLock);

            return status;
        }
    }
    WdfWaitLockRelease(m_I2CWaitLock);

    if (!NT_SUCCESS(status))
    {
        TraceError("ACC %!FUNC! Failed to put device into standby %!STATUS!", status);

        return status;
    }

    return status;
}

NTSTATUS AlsDevice::IsrOff()
{
    NTSTATUS status = STATUS_SUCCESS;
    REGISTER_SETTING settings[] =
    {        
        { ISL29018_REG_ADD_INT_LT_LSB, 0x00 },
        { ISL29018_REG_ADD_INT_LT_MSB, 0x00 },
        { ISL29018_REG_ADD_INT_HT_LSB, 0xFF },
        { ISL29018_REG_ADD_INT_HT_MSB, 0xFF },       
    };

    WdfWaitLockAcquire(m_I2CWaitLock, NULL);
    for (DWORD i = 0; i < ARRAYSIZE(settings); i++)
    {
        REGISTER_SETTING setting = settings[i];
        status = I2CSensorWriteRegister(m_I2CIoTarget, setting.Register, &setting.Value, sizeof(setting.Value));

        if (!NT_SUCCESS(status))
        {
            TraceError("ACC %!FUNC! I2CSensorReadRegister from 0x%02x failed! %!STATUS!", setting.Register, status);
            WdfWaitLockRelease(m_I2CWaitLock);

            return status;
        }
    }
    WdfWaitLockRelease(m_I2CWaitLock);

    if (!NT_SUCCESS(status))
    {
        TraceError("ACC %!FUNC! Failed to put device into standby %!STATUS!", status);

        return status;
    }

    return status;
}
