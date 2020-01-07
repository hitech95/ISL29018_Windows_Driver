//Copyright (C) Microsoft Corporation, All Rights Reserved.
//
//Abstract:
//
//    This module contains the implementation of entry and exit point
//    of sample ADXL345 accelerometer driver.
//
//Environment:
//
//   Windows User-Mode Driver Framework (UMDF)

#include "Device.h"
#include "Driver.h"

#include "Driver.tmh"

// This routine is the driver initialization entry point.
NTSTATUS DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    NTSTATUS status;

    // Initialize WPP Tracing
    WPP_INIT_TRACING(DriverObject, NULL);

    SENSOR_FunctionEnter();

    config.DriverPoolTag = SENSORV2_POOL_TAG_ACCELEROMETER;

    // Register a cleanup callback so that we can call WPP_CLEANUP when
    // the framework driver object is deleted during driver unload.
    WDF_DRIVER_CONFIG_INIT(&config, AlsDevice::OnDeviceAdd);
    config.EvtDriverUnload = OnDriverUnload;

    status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);

    if (!NT_SUCCESS(status))
    {
        TraceError("WdfDriverCreate failed %!STATUS!", status);
    }

    SENSOR_FunctionExit(status);
    return status;
}

// This routine is called when the driver unloads.
VOID OnDriverUnload(
    _In_ WDFDRIVER Driver)      // Driver object
{
    SENSOR_FunctionEnter();

    SENSOR_FunctionExit(STATUS_SUCCESS);

    // WPP_CLEANUP doesn't actually use the Driver parameter so we need to set it as unreferenced.
    UNREFERENCED_PARAMETER(Driver);
    WPP_CLEANUP(WdfDriverWdmGetDriverObject(Driver));

    return;
}
