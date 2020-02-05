
#ifndef __DEVEXT_HOOKS_H__
#define __DEVEXT_HOOKS_H__



#include <ntifs.h>



typedef struct _PROXY_DEVICE_EXTENSION {
	PDEVICE_OBJECT TargetDevice;
	PDEVICE_OBJECT UpperDevice;
	PDEVICE_OBJECT *UpdatePlace;
	size_t LowerDeviceOffset;
} PROXY_DEVICE_EXTENSION, *PPROXY_DEVICE_EXTENSION;



NTSTATUS ProxyDeviceCreate(PDEVICE_OBJECT TargetDevice, PDEVICE_OBJECT *ProxyDevice);
void ProxyDeviceDelete(PDEVICE_OBJECT ProxyDevice);

NTSTATUS DevExtHooksModuleInit(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath, PVOID Context);
void DevExtHooksModuleFinit(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath, PVOID Context);



#endif