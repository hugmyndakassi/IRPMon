
#include <ntifs.h>
#include <tdikrnl.h>
#include <bthdef.h>
#include <BthIoctl.h>
#include <Bthddi.h>
#include "preprocessor.h"
#include "allocator.h"
#include "utils.h"
#include "driver-settings.h"
#include "data-loggers.h"



/************************************************************************/
/*               GLOBAL VARIABLES                                       */
/************************************************************************/


static PIRPMNDRV_SETTINGS _driverSettings = NULL;

/************************************************************************/
/*                    HELPER FUNCTIONS                                  */
/************************************************************************/


static SIZE_T _StringSize(const wchar_t *S)
{
	SIZE_T ret = 0;
	DEBUG_ENTER_FUNCTION("S=%ls", S);

	if (S != NULL)
		ret = (wcslen(S) + 1) * sizeof(wchar_t);

	DEBUG_EXIT_FUNCTION("%Iu", ret);
	return ret;
}


static SIZE_T _MultiStringSize(const wchar_t *MS)
{
	SIZE_T len = 0;
	SIZE_T ret = 0;
	DEBUG_ENTER_FUNCTION("MS=0x%p", MS);

	if (MS != NULL) {
		ret = sizeof(wchar_t);
		while (*MS != L'\0') {
			len = wcslen(MS) + 1;
			ret += len * sizeof(wchar_t);
			MS += len;
		}
	}

	DEBUG_EXIT_FUNCTION("%Iu", ret);
	return ret;
}


static SIZE_T _DeviceRelationSize(const DEVICE_RELATIONS *R)
{
	SIZE_T ret = 0;
	DEBUG_ENTER_FUNCTION("R=0x%p", R);

	if (R != NULL)
		ret = sizeof(DEVICE_RELATIONS) - sizeof(PDEVICE_OBJECT) + sizeof(PDEVICE_OBJECT)*R->Count;

	DEBUG_EXIT_FUNCTION("%Iu", ret);
	return ret;
}


static void _ProcessBluetooth(PDEVICE_OBJECT DeviceObject, PIRP Irp, PIO_STACK_LOCATION IrpStack, BOOLEAN Completion, PDATA_LOGGER_RESULT Result)
{
	ULONG controlCode = 0;
	BRB_HEADER *brbHeader = NULL;
	DEBUG_ENTER_FUNCTION("DeviceObject=0x%p; Irp=0x%p; IrpStack=0x%p; Completion=%u; Result=0x%p", DeviceObject, Irp, IrpStack, Completion, Result);

	switch (IrpStack->MajorFunction) {
		case IRP_MJ_INTERNAL_DEVICE_CONTROL:
			controlCode = IrpStack->Parameters.DeviceIoControl.IoControlCode;
			switch (controlCode) {
				case IOCTL_INTERNAL_BTH_SUBMIT_BRB:
					brbHeader = IrpStack->Parameters.Others.Argument1;

					if ((ULONG_PTR)brbHeader >= MmUserProbeAddress) {
						Result->Buffer = brbHeader;
						if (!Completion)
							Result->BufferSize = brbHeader->Length;
						else Result->BufferSize = Irp->IoStatus.Information;
					}
					break;
			}
			break;
	}

	DEBUG_EXIT_FUNCTION("void, *Buffer=0x%p, *BufferSize=%Iu, *BfferMdl=0x%p", Result->Buffer, Result->BufferSize, Result->BufferMdl);
	return;
}

typedef struct _TDI_DATA {
	ULONG TotalLength;
	ULONG MinorFunction;
	ULONG LocalLength;
	ULONG RemoteLength;
	unsigned char LocalAddress[16];
	unsigned char RemoteAddress[16];
	ULONG Flags;
	ULONG Length;
	// Buffer
} TDI_DATA, *PTDI_DATA;


static PTDI_DATA _TDIDataAlloc(ULONG MinorFunction, BOOLEAN Completed, PVOID Parameters, PMDL Mdl)
{
	PMDL mdl = NULL;
	void *buffer = NULL;
	PTDI_DATA ret = NULL;
	ULONG totalLength = 0;
	ULONG addrLen = 0;
	PTDI_CONNECTION_INFORMATION ci = NULL;
	DEBUG_ENTER_FUNCTION("MinorFunction=%u; Completed=%u; Parameters=0x%p; Mdl=0x%p", MinorFunction, Completed, Parameters, Mdl);

	totalLength = sizeof(TDI_DATA);
	if (!Completed && (MinorFunction == TDI_SEND || MinorFunction == TDI_SEND_DATAGRAM || MinorFunction == TDI_DIRECT_SEND || MinorFunction == TDI_DIRECT_SEND_DATAGRAM)) {
		mdl = Mdl;
		if (mdl != NULL)
			buffer = MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority);
	} else if (Completed && (MinorFunction == TDI_RECEIVE || MinorFunction == TDI_RECEIVE_DATAGRAM)) {
		mdl = Mdl;
		if (mdl != NULL)
			buffer = MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority);
	}

	switch (MinorFunction) {
		case TDI_ASSOCIATE_ADDRESS:
			break;
		case TDI_DISASSOCIATE_ADDRESS:
			if (!Completed)
				ci = ((PTDI_REQUEST_KERNEL_DISASSOCIATE)Parameters)->RequestConnectionInformation;
			break;
		case TDI_LISTEN:
			if (Completed)
				ci = ((PTDI_REQUEST_KERNEL_LISTEN)Parameters)->ReturnConnectionInformation;
			break;
		case TDI_ACCEPT:
			if (Completed)
				ci = ((PTDI_REQUEST_KERNEL_ACCEPT)Parameters)->ReturnConnectionInformation;
			break;
		case TDI_CONNECT:
			if (!Completed)
				ci = ((PTDI_REQUEST_KERNEL_CONNECT)Parameters)->RequestConnectionInformation;
			break;
		case TDI_SEND:
			if (!Completed)
				totalLength += ((PTDI_REQUEST_KERNEL_SEND)Parameters)->SendLength;
			break;
		case TDI_SEND_DATAGRAM:
			if (!Completed) {
				totalLength += ((PTDI_REQUEST_KERNEL_SENDDG)Parameters)->SendLength;
				ci = ((PTDI_REQUEST_KERNEL_SENDDG)Parameters)->SendDatagramInformation;
			}
			break;
		case TDI_RECEIVE:
			if (Completed)
				totalLength += ((PTDI_REQUEST_KERNEL_RECEIVE)Parameters)->ReceiveLength;
			break;
		case TDI_RECEIVE_DATAGRAM:
			if (Completed) {
				totalLength += ((PTDI_REQUEST_KERNEL_RECEIVEDG)Parameters)->ReceiveLength;
				ci = ((PTDI_REQUEST_KERNEL_RECEIVEDG)Parameters)->ReturnDatagramInformation;
			}
			break;
	}

	ret = HeapMemoryAllocNonPaged(totalLength);
	if (ret != NULL) {
		memset(ret, 0, totalLength);
		ret->MinorFunction = MinorFunction;
		ret->TotalLength = totalLength;
		ret->Length = totalLength - sizeof(TDI_DATA);
		if (buffer != NULL)
			memcpy(ret + 1, buffer, ret->Length);

		if (ci != NULL) {
			addrLen = ci->RemoteAddressLength;
			if (addrLen > sizeof(ret->RemoteAddress))
				addrLen = sizeof(ret->RemoteAddress);

			ret->RemoteLength = addrLen;
			memcpy(ret->RemoteAddress, ci->RemoteAddress, ret->RemoteLength);
		}
	}

	if (buffer != NULL)
		MmUnmapLockedPages(buffer, mdl);

	DEBUG_EXIT_FUNCTION("0x%p", ret);
	return ret;
}


static void _ProcessTDI(PDEVICE_OBJECT DeviceObject, PIRP Irp, PIO_STACK_LOCATION IrpStack, BOOLEAN Completion, PDATA_LOGGER_RESULT Result)
{
	ULONG minorFunction = 0;
	PTDI_DATA tdiData = NULL;
	DEBUG_ENTER_FUNCTION("DeviceObject=0x%p; Irp=0x%p; IrpStack=0x%p; Completion=%u; Result=0x%p", DeviceObject, Irp, IrpStack, Completion, Result);

	switch (IrpStack->MajorFunction) {
		case IRP_MJ_INTERNAL_DEVICE_CONTROL:
			minorFunction = IrpStack->MinorFunction;
			if (minorFunction != 0) {
				tdiData = _TDIDataAlloc(minorFunction, Completion, &IrpStack->Parameters, Irp->MdlAddress);
				if (tdiData != NULL) {
					Result->Buffer = tdiData;
					Result->BufferAllocated = TRUE;
					Result->BufferSize = tdiData->TotalLength;
				}
			}
			break;
	}

	DEBUG_EXIT_FUNCTION("void, *Buffer=0x%p, *BufferSize=%Iu, *BfferMdl=0x%p", Result->Buffer, Result->BufferSize, Result->BufferMdl);
	return;
}


/************************************************************************/
/*            PUBLIC FUNCTIONS                                          */
/************************************************************************/


void IRPDataLogger(PDEVICE_OBJECT DeviceObject, PIRP Irp, PIO_STACK_LOCATION IrpStack, BOOLEAN Completion, PDATA_LOGGER_RESULT Result)
{
	KPROCESSOR_MODE mode;
	KPROCESSOR_MODE reqMode;
	DEBUG_ENTER_FUNCTION("DeviceObject=0x%p; Irp=0x%p; IrpStack=0x%p; Completion=%u; Result=0x%p", DeviceObject, Irp, IrpStack, Completion, Result);

	reqMode = Irp->RequestorMode;
	mode = ExGetPreviousMode();
	memset(Result, 0, sizeof(DATA_LOGGER_RESULT));
	switch (IrpStack->MajorFunction) {
		case IRP_MJ_CREATE: {
			if (!Completion) {
				SIZE_T bufSize = IrpStack->Parameters.Create.EaLength;
				POOL_TYPE pt = (KeGetCurrentIrql() < DISPATCH_LEVEL) ? PagedPool : NonPagedPool;

				if (IrpStack->Parameters.Create.SecurityContext != NULL)
					bufSize += sizeof(ACCESS_MASK);

				Result->Buffer = HeapMemoryAlloc(pt, bufSize);
				if (Result->Buffer != NULL) {
					Result->BufferAllocated = TRUE;
					Result->BufferSize = bufSize;
					memcpy(Result->Buffer, Irp->AssociatedIrp.SystemBuffer, IrpStack->Parameters.Create.EaLength);
					if (IrpStack->Parameters.Create.SecurityContext != NULL)
						memcpy((unsigned char *)Result->Buffer + Result->BufferSize - sizeof(ACCESS_MASK), &IrpStack->Parameters.Create.SecurityContext->DesiredAccess, sizeof(ACCESS_MASK));
				}
			}
		} break;
		case IRP_MJ_CREATE_NAMED_PIPE: {
			SIZE_T bufSize = 0;
			POOL_TYPE pt = (KeGetCurrentIrql() < DISPATCH_LEVEL) ? PagedPool : NonPagedPool;
			PREQUEST_IRP_CREATE_NAMED_PIPE_DATA buf = NULL;

			if (!Completion && IrpStack->Parameters.CreatePipe.SecurityContext != NULL) {
				bufSize = sizeof(REQUEST_IRP_CREATE_NAMED_PIPE_DATA);
				buf = HeapMemoryAlloc(pt, bufSize);
				if (buf != NULL) {
					buf->DesiredAccess = IrpStack->Parameters.CreatePipe.SecurityContext->DesiredAccess;
					buf->Parameters = *IrpStack->Parameters.CreatePipe.Parameters;
					Result->Buffer = buf;
					Result->BufferAllocated = TRUE;
					Result->BufferSize = bufSize;
				}
			}
		} break;
		case IRP_MJ_CREATE_MAILSLOT: {
			SIZE_T bufSize = 0;
			POOL_TYPE pt = (KeGetCurrentIrql() < DISPATCH_LEVEL) ? PagedPool : NonPagedPool;
			PREQUEST_IRP_CREATE_MAILSLOT_DATA buf = NULL;

			if (!Completion && IrpStack->Parameters.CreateMailslot.SecurityContext != NULL) {
				bufSize = sizeof(REQUEST_IRP_CREATE_MAILSLOT_DATA);
				buf = HeapMemoryAlloc(pt, bufSize);
				if (buf != NULL) {
					buf->DesiredAccess = IrpStack->Parameters.CreateMailslot.SecurityContext->DesiredAccess;
					buf->Parameters = *IrpStack->Parameters.CreateMailslot.Parameters;
					Result->Buffer = buf;
					Result->BufferAllocated = TRUE;
					Result->BufferSize = bufSize;
				}
			}
		} break;
		case IRP_MJ_READ: {
			if (Completion) {
				if (Irp->MdlAddress != NULL) {
					Result->BufferMdl = Irp->MdlAddress;
					Result->Buffer = MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority);
				} else if (Irp->AssociatedIrp.SystemBuffer != NULL)
					Result->Buffer = Irp->AssociatedIrp.SystemBuffer;
				else if (KeGetCurrentIrql() < DISPATCH_LEVEL && Irp->UserBuffer != NULL)
					Result->Buffer = Irp->UserBuffer;

				if (Result->Buffer != NULL)
					Result->BufferSize = Irp->IoStatus.Information;
			}
		} break;
		case IRP_MJ_WRITE:
			if (!Completion) {
				if (Irp->MdlAddress != NULL) {
					Result->BufferMdl = Irp->MdlAddress;
					Result->Buffer = MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority);
				} else if (Irp->AssociatedIrp.SystemBuffer != NULL)
					Result->Buffer = Irp->AssociatedIrp.SystemBuffer;
				else if (Irp->UserBuffer != NULL)
					Result->Buffer = Irp->UserBuffer;

				if (Result->Buffer != NULL)
					Result->BufferSize = IrpStack->Parameters.Write.Length;
			}
			break;
		case IRP_MJ_DEVICE_CONTROL:
		case IRP_MJ_INTERNAL_DEVICE_CONTROL:
		case IRP_MJ_FILE_SYSTEM_CONTROL: {
			if (IrpStack->MajorFunction != IRP_MJ_FILE_SYSTEM_CONTROL ||
				IrpStack->MinorFunction == IRP_MN_KERNEL_CALL ||
				IrpStack->MinorFunction == IRP_MN_USER_FS_REQUEST) {
				ULONG ioctl = IrpStack->Parameters.DeviceIoControl.IoControlCode;
				ULONG method = ioctl & 3;

				if (IrpStack->MajorFunction != IRP_MJ_INTERNAL_DEVICE_CONTROL || (ioctl != IOCTL_INTERNAL_BTH_SUBMIT_BRB && IrpStack->MinorFunction == 0)) {
					switch (method) {
						case METHOD_NEITHER:
							if (!Completion) {
								Result->Buffer = IrpStack->Parameters.DeviceIoControl.Type3InputBuffer;
								Result->BufferSize = IrpStack->Parameters.DeviceIoControl.InputBufferLength;
								if (IrpStack->MajorFunction == IRP_MJ_DEVICE_CONTROL ||
									(IrpStack->MajorFunction == IRP_MJ_FILE_SYSTEM_CONTROL && IrpStack->MinorFunction == IRP_MN_USER_FS_REQUEST)) {
									if (reqMode == UserMode &&
										Result->BufferSize > 0) {
										__try {
											ProbeForRead(Result->Buffer, Result->BufferSize, 1);
										} __except (EXCEPTION_EXECUTE_HANDLER) {
											DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL, "DANGEROUS BUFFER: Irp=0x%p; IrpStack=0x%p; Buffer=0x%p; Size=%zu\n", Irp, IrpStack, Result->Buffer, Result->BufferSize);
											Result->Buffer = NULL;
											Result->BufferSize = 0;
										}
									}

									if (reqMode == UserMode &&
										IrpStack->Parameters.DeviceIoControl.OutputBufferLength > 0) {
										__try {
											ProbeForRead(Irp->UserBuffer, IrpStack->Parameters.DeviceIoControl.OutputBufferLength, 1);
										} __except (EXCEPTION_EXECUTE_HANDLER) {
											DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL, "DANGEROUS BUFFER: Irp=0x%p; IrpStack=0x%p; Buffer=0x%p; Size=%zu\n", Irp, IrpStack, Irp->UserBuffer, IrpStack->Parameters.DeviceIoControl.OutputBufferLength);
											Result->Buffer = NULL;
											Result->BufferSize = 0;
										}
									}
								}
							} else {
								Result->Buffer = Irp->UserBuffer;
								Result->BufferSize = IrpStack->Parameters.DeviceIoControl.OutputBufferLength;
							}
							break;
						case METHOD_IN_DIRECT:
							if (!Completion) {
								Result->BufferMdl = Irp->MdlAddress;
								if (Irp->MdlAddress != NULL)
									Result->Buffer = MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority);
								else Result->Buffer = Irp->AssociatedIrp.SystemBuffer;

								Result->BufferSize = IrpStack->Parameters.DeviceIoControl.InputBufferLength;
							}
							break;
						case METHOD_OUT_DIRECT:
							if (Completion) {
								Result->BufferMdl = Irp->MdlAddress;
								if (Irp->MdlAddress != NULL)
									Result->Buffer = MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority);
								else Result->Buffer = Irp->AssociatedIrp.SystemBuffer;

								Result->BufferSize = Irp->IoStatus.Information;
							} else {
								Result->Buffer = Irp->AssociatedIrp.SystemBuffer;
								Result->BufferSize = IrpStack->Parameters.DeviceIoControl.InputBufferLength;
							}
							break;
						case METHOD_BUFFERED:
							Result->Buffer = Irp->AssociatedIrp.SystemBuffer;
							if (!Completion)
								Result->BufferSize = IrpStack->Parameters.DeviceIoControl.InputBufferLength;
							else Result->BufferSize = Irp->IoStatus.Information;
							break;
					}
				} else {
					switch (ioctl) {
						case IOCTL_INTERNAL_BTH_SUBMIT_BRB:
							_ProcessBluetooth(DeviceObject, Irp, IrpStack, Completion, Result);
							break;
						default:
							_ProcessTDI(DeviceObject, Irp, IrpStack, Completion, Result);
							break;
					}
				}
			} else {
				if (!Completion) {
					switch (IrpStack->MinorFunction) {
						case IRP_MN_MOUNT_VOLUME:
							Result->Buffer = IrpStack->Parameters.MountVolume.Vpb;
							Result->BufferSize = sizeof(VPB);
							break;
						case IRP_MN_VERIFY_VOLUME:
							Result->Buffer = IrpStack->Parameters.MountVolume.Vpb;
							Result->BufferSize = sizeof(VPB);
							break;
						case IRP_MN_LOAD_FILE_SYSTEM:
							break;
					}

					if (Result->Buffer == NULL)
						Result->BufferSize = 0;
				}
			}
			} break;
		case IRP_MJ_QUERY_VOLUME_INFORMATION:
		case IRP_MJ_QUERY_INFORMATION:
			if (Completion) {
				Result->Buffer = Irp->AssociatedIrp.SystemBuffer;
				Result->BufferSize = Irp->IoStatus.Information;
			}
			break;
		case IRP_MJ_SET_INFORMATION:
			if (!Completion) {
				Result->Buffer = Irp->AssociatedIrp.SystemBuffer;
				Result->BufferSize = IrpStack->Parameters.SetFile.Length;
			}
			break;
		case IRP_MJ_SET_VOLUME_INFORMATION:
			if (!Completion) {
				Result->Buffer = Irp->AssociatedIrp.SystemBuffer;
				Result->BufferSize = IrpStack->Parameters.SetVolume.Length;
			}
			break;
		case IRP_MJ_DIRECTORY_CONTROL:
			switch (IrpStack->MinorFunction) {
				case IRP_MN_QUERY_DIRECTORY:
					if (!Completion) {
						PUNICODE_STRING fileMask = NULL;
						
						fileMask = IrpStack->Parameters.QueryDirectory.FileName;
						if (fileMask != NULL) {
							Result->Buffer = HeapMemoryAllocNonPaged(sizeof(UNICODE_STRING) + fileMask->Length);
							if (Result->Buffer != NULL) {
								Result->BufferAllocated = TRUE;
								Result->BufferSize = sizeof(UNICODE_STRING) + fileMask->Length;
								*(PUNICODE_STRING)(Result->Buffer) = *fileMask;
								memcpy((unsigned char *)Result->Buffer + sizeof(UNICODE_STRING), fileMask->Buffer, fileMask->Length);
							}
						}
					} else {
						Result->Buffer = Irp->UserBuffer;
						Result->BufferSize = Irp->IoStatus.Information;
					}
					break;
				case IRP_MN_NOTIFY_CHANGE_DIRECTORY:
					// Already probed by the kernel
					if (!Completion) {
						Result->Buffer = Irp->UserBuffer;
						Result->BufferSize = Irp->IoStatus.Information;
					}
					break;
			}
			break;
		case IRP_MJ_QUERY_EA: {
			if (!Completion) {
				Result->Buffer = IrpStack->Parameters.QueryEa.EaList;
				Result->BufferSize = IrpStack->Parameters.QueryEa.EaListLength;
			} else {
				if (DeviceObject != NULL) {
					if (DeviceObject->Flags & DO_BUFFERED_IO)
						Result->Buffer = Irp->AssociatedIrp.SystemBuffer;
					else if (DeviceObject->Flags & DO_DIRECT_IO) {
						Result->BufferMdl = Irp->MdlAddress;
						Result->Buffer = MmGetSystemAddressForMdlSafe(Result->BufferMdl, NormalPagePriority);
					} else Result->Buffer = Irp->UserBuffer;

					Result->BufferSize = Irp->IoStatus.Information;
				}
			}
		} break;
		case IRP_MJ_SET_EA:
			if (Completion) {
				if (DeviceObject != NULL) {
					if (DeviceObject->Flags & DO_BUFFERED_IO)
						Result->Buffer = Irp->AssociatedIrp.SystemBuffer;
					else if (DeviceObject->Flags & DO_DIRECT_IO) {
						Result->BufferMdl = Irp->MdlAddress;
						Result->Buffer = MmGetSystemAddressForMdlSafe(Result->BufferMdl, NormalPagePriority);
					} else Result->Buffer = Irp->UserBuffer;

					Result->BufferSize = IrpStack->Parameters.SetEa.Length;
				}
			}
			break;
		case IRP_MJ_PNP: {
			if (Completion && NT_SUCCESS(Irp->IoStatus.Status)) {
				switch (IrpStack->MinorFunction) {
					case IRP_MN_QUERY_ID:
						Result->Buffer = (void *)Irp->IoStatus.Information;
						switch (IrpStack->Parameters.QueryId.IdType) {
							case BusQueryDeviceID:
							case BusQueryInstanceID:
							case BusQueryContainerID:
							case BusQueryDeviceSerialNumber:
								Result->BufferSize = _StringSize(Result->Buffer);
								break;
							case BusQueryHardwareIDs:
							case BusQueryCompatibleIDs:
								Result->BufferSize = _MultiStringSize(Result->Buffer);
								break;
						}
						break;
					case IRP_MN_QUERY_DEVICE_TEXT:
						Result->Buffer = (void *)Irp->IoStatus.Information;
						Result->BufferSize = _StringSize(Result->Buffer);
						break;
					case IRP_MN_QUERY_CAPABILITIES:
						Result->Buffer = IrpStack->Parameters.DeviceCapabilities.Capabilities;
						Result->BufferSize = sizeof(DEVICE_CAPABILITIES);
						break;
					case IRP_MN_QUERY_DEVICE_RELATIONS:
						Result->Buffer = (void *)Irp->IoStatus.Information;
						Result->BufferSize = _DeviceRelationSize(Result->Buffer);
						break;
					case IRP_MN_QUERY_BUS_INFORMATION:
						Result->Buffer = (void *)Irp->IoStatus.Information;
						Result->BufferSize = sizeof(PNP_BUS_INFORMATION);
						break;
					case IRP_MN_QUERY_INTERFACE:
						Result->Buffer = IrpStack->Parameters.QueryInterface.Interface;
						Result->BufferSize = IrpStack->Parameters.QueryInterface.Size;
						break;
				}
			} else if (!Completion) {
				switch (IrpStack->MinorFunction) {
					case IRP_MN_QUERY_INTERFACE:
						Result->Buffer = (void *)IrpStack->Parameters.QueryInterface.InterfaceType;
						Result->BufferSize = sizeof(GUID);
						break;
				}
			}
		} break;
		case IRP_MJ_QUERY_SECURITY: {
			// Already probed by the kernel
			if (Completion && NT_SUCCESS(Irp->IoStatus.Status)) {
				Result->Buffer = Irp->UserBuffer;
				Result->BufferSize = IrpStack->Parameters.QuerySecurity.Length;
			}
		} break;
		case IRP_MJ_SET_SECURITY: {
			if (!Completion) {
				Result->Buffer = IrpStack->Parameters.SetSecurity.SecurityDescriptor;
				Result->BufferSize = RtlLengthSecurityDescriptor(IrpStack->Parameters.SetSecurity.SecurityDescriptor);
			}
		} break;
		default:
			break;
	}

	if (_driverSettings->StripData && Result->BufferSize > _driverSettings->DataStripThreshold) {
		Result->BufferSize = _driverSettings->DataStripThreshold;
		Result->Stripped = TRUE;
	}

	DEBUG_EXIT_FUNCTION("void, *Buffer=0x%p, *BufferSize=%Iu, *BfferMdl=0x%p", Result->Buffer, Result->BufferSize, Result->BufferMdl);
	return;
}


void IRPDataLoggerSetRequestFlags(PREQUEST_HEADER Request, const DATA_LOGGER_RESULT *Data)
{
	DEBUG_ENTER_FUNCTION("Request=0x%p; Data=0x%p", Request, Data);

	if (Data->Stripped)
		Request->Flags |= REQUEST_FLAG_DATA_STRIPPED;

	DEBUG_EXIT_FUNCTION_VOID();
	return;
}


void DataLoggerResultRelease(PDATA_LOGGER_RESULT Result)
{
	DEBUG_ENTER_FUNCTION("Result=0x%p", Result);

	if (Result->BufferAllocated)
		HeapMemoryFree(Result->Buffer);

	DEBUG_EXIT_FUNCTION_VOID();
	return;
}


/************************************************************************/
/*                INITIALIZATION AND FINALIZATION                       */
/************************************************************************/


NTSTATUS DataLoggerModuleInit(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath, PVOID Context)
{
	NTSTATUS status = STATUS_UNSUCCESSFUL;
	DEBUG_ENTER_FUNCTION("DriverObject=0x%p; RegistryPath=\"%wZ\"; Context=0x%p", DriverObject, RegistryPath, Context);

	_driverSettings = DriverSettingsGet();
	status = STATUS_SUCCESS;

	DEBUG_EXIT_FUNCTION("0x%x", status);
	return status;
}


void DataLoggerModuleFinit(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath, PVOID Context)
{
	DEBUG_ENTER_FUNCTION("DriverObject=0x%p; RegistryPath=\"%wZ\"; Context=0x%p", DriverObject, RegistryPath, Context);

	_driverSettings = NULL;

	DEBUG_EXIT_FUNCTION_VOID();
	return;
}
