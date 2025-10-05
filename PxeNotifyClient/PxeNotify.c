//
// PxeNotify.c
// UEFI app that uses EFI_HTTP_PROTOCOL to POST /pxe-status with JSON payload
// Binario UEFI que usa o EFI_HTTP_PROTOCOL para realizar o POST para os PXE server (/pxe-status) utilizando um payload de JSON.
//

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PrintLib.h>
#include <Library/DebugLib.h>
#include <Protocol/Http.h>
#include <Protocol/Ip4Config2.h>
#include <Protocol/ServiceBinding.h>
#include <Protocol/SimpleNetwork.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#define NOTIFY_HOST "192.168.56.10"  // PXE server IP (ASCII)
#define NOTIFY_PORT 5000
#define NOTIFY_PATH "/pxe-status"

//
// Helper: wait for event with timeout (seconds)
//
EFI_STATUS WaitForEventSeconds(IN EFI_EVENT Event, IN UINTN Seconds) {
  UINTN Index;
  EFI_STATUS Status;
  
  for (Index = 0; Index < Seconds * 10; Index++) {
    Status = gBS->CheckEvent(Event);
    if (Status == EFI_SUCCESS) {
      return EFI_SUCCESS;
    }
    gBS->Stall(100000); // 100 ms
  }
  
  return EFI_TIMEOUT;
}

//
// Helper: Get MAC address from SNP protocol
//
EFI_STATUS GetMacAddress(OUT CHAR8 *MacStr, IN UINTN MacStrSize) {
  EFI_STATUS Status;
  EFI_HANDLE *Handles = NULL;
  UINTN HandleCount = 0;
  EFI_SIMPLE_NETWORK_PROTOCOL *Snp = NULL;

  // Try to locate SNP protocol
  Status = gBS->LocateHandleBuffer(
                  ByProtocol,
                  &gEfiSimpleNetworkProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  
  if (EFI_ERROR(Status) || HandleCount == 0) {
    AsciiSPrint(MacStr, MacStrSize, "00:00:00:00:00:00");
    if (Handles) FreePool(Handles);
    return EFI_NOT_FOUND;
  }

  // Get first SNP
  Status = gBS->HandleProtocol(
                  Handles[0],
                  &gEfiSimpleNetworkProtocolGuid,
                  (VOID **)&Snp
                  );
  
  if (EFI_ERROR(Status)) {
    AsciiSPrint(MacStr, MacStrSize, "00:00:00:00:00:00");
    FreePool(Handles);
    return Status;
  }

  // Format MAC address
  AsciiSPrint(
    MacStr,
    MacStrSize,
    "%02x:%02x:%02x:%02x:%02x:%02x",
    Snp->Mode->CurrentAddress.Addr[0],
    Snp->Mode->CurrentAddress.Addr[1],
    Snp->Mode->CurrentAddress.Addr[2],
    Snp->Mode->CurrentAddress.Addr[3],
    Snp->Mode->CurrentAddress.Addr[4],
    Snp->Mode->CurrentAddress.Addr[5]
  );

  FreePool(Handles);
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS Status;
  EFI_HANDLE *Handles = NULL;
  UINTN HandleCount = 0;
  EFI_SERVICE_BINDING_PROTOCOL *ServiceBinding = NULL;
  EFI_HANDLE HttpChildHandle = NULL;
  EFI_HTTP_PROTOCOL *Http = NULL;
  EFI_HTTP_CONFIG_DATA HttpConfigData;
  EFI_HTTPv4_ACCESS_POINT IPv4Node;
  EFI_HTTP_TOKEN RequestToken;
  EFI_HTTP_TOKEN ResponseToken;
  EFI_HTTP_MESSAGE RequestMessage;
  EFI_HTTP_MESSAGE ResponseMessage;
  EFI_HTTP_REQUEST_DATA RequestData;
  EFI_HTTP_HEADER *RequestHeaders = NULL;
  CHAR8 *RequestBody = NULL;
  CHAR8 *ResponseBody = NULL;
  CHAR8 MacStr[32];
  CHAR8 JsonBody[256];
  UINTN JsonBodyLen;
  CHAR8 ContentLengthStr[32];
  UINTN Index;
  BOOLEAN BootSuccess = TRUE; // Assume success unless we detect failure
  
  // Unused parameters
  (VOID)ImageHandle;
  (VOID)SystemTable;

  Print(L"PxeNotify: Starting\n");

  // Get MAC address
  Status = GetMacAddress(MacStr, sizeof(MacStr));
  if (EFI_ERROR(Status)) {
    Print(L"PxeNotify: Warning - Could not get MAC address, using default\n");
  }
  Print(L"PxeNotify: MAC Address: %a\n", MacStr);

  // Boot status
  CHAR8 *StatusStr = BootSuccess ? "booted" : "failed";

  // Build JSON body: {"client_id": "XX:XX:XX:XX:XX:XX", "status": "booted"}
  AsciiSPrint(JsonBody, sizeof(JsonBody), "{\"client_id\":\"%a\",\"status\":\"%a\"}", MacStr, StatusStr);
  JsonBodyLen = AsciiStrLen(JsonBody);
  Print(L"PxeNotify: JSON Payload: %a\n", JsonBody);

  // Locate HTTP Service Binding Protocol
  Status = gBS->LocateHandleBuffer(
                  ByProtocol,
                  &gEfiHttpServiceBindingProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  
  if (EFI_ERROR(Status) || HandleCount == 0) {
    Print(L"PxeNotify: HTTP Service Binding not found (status=%r)\n", Status);
    return Status;
  }

  Print(L"PxeNotify: Found %d HTTP Service Binding handle(s)\n", HandleCount);

  // Get Service Binding Protocol
  Status = gBS->HandleProtocol(
                  Handles[0],
                  &gEfiHttpServiceBindingProtocolGuid,
                  (VOID **)&ServiceBinding
                  );
  
  if (EFI_ERROR(Status)) {
    Print(L"PxeNotify: Failed to get Service Binding Protocol: %r\n", Status);
    FreePool(Handles);
    return Status;
  }

  // Create HTTP child instance
  HttpChildHandle = NULL;
  Status = ServiceBinding->CreateChild(ServiceBinding, &HttpChildHandle);
  if (EFI_ERROR(Status)) {
    Print(L"PxeNotify: CreateChild failed: %r\n", Status);
    FreePool(Handles);
    return Status;
  }

  Print(L"PxeNotify: HTTP child created\n");

  // Get HTTP Protocol
  Status = gBS->HandleProtocol(
                  HttpChildHandle,
                  &gEfiHttpProtocolGuid,
                  (VOID **)&Http
                  );
  
  if (EFI_ERROR(Status)) {
    Print(L"PxeNotify: Failed to get HTTP Protocol: %r\n", Status);
    goto cleanup;
  }

  // Configure HTTP
  ZeroMem(&HttpConfigData, sizeof(HttpConfigData));
  ZeroMem(&IPv4Node, sizeof(IPv4Node));
  
  HttpConfigData.HttpVersion = HttpVersion11;
  HttpConfigData.TimeOutMillisec = 10000; // 10 second timeout
  HttpConfigData.LocalAddressIsIPv6 = FALSE;
  
  IPv4Node.UseDefaultAddress = TRUE;
  IPv4Node.LocalPort = 0; // Ephemeral port
  HttpConfigData.AccessPoint.IPv4Node = &IPv4Node;

  Status = Http->Configure(Http, &HttpConfigData);
  if (EFI_ERROR(Status)) {
    Print(L"PxeNotify: HTTP Configure failed: %r\n", Status);
    goto cleanup;
  }

  Print(L"PxeNotify: HTTP configured\n");

  // Allocate request body
  RequestBody = AllocateZeroPool(JsonBodyLen + 1);
  if (!RequestBody) {
    Print(L"PxeNotify: Failed to allocate request body\n");
    Status = EFI_OUT_OF_RESOURCES;
    goto cleanup;
  }
  CopyMem(RequestBody, JsonBody, JsonBodyLen);

  // Build request headers (Host, Content-Type, Content-Length)
  RequestHeaders = AllocateZeroPool(sizeof(EFI_HTTP_HEADER) * 3);
  if (!RequestHeaders) {
    Print(L"PxeNotify: Failed to allocate headers\n");
    Status = EFI_OUT_OF_RESOURCES;
    goto cleanup;
  }

  // Header 0: Host
  RequestHeaders[0].FieldName = AllocateCopyPool(AsciiStrSize("Host"), "Host");
  RequestHeaders[0].FieldValue = AllocateCopyPool(AsciiStrSize(NOTIFY_HOST), NOTIFY_HOST);

  // Header 1: Content-Type
  RequestHeaders[1].FieldName = AllocateCopyPool(AsciiStrSize("Content-Type"), "Content-Type");
  RequestHeaders[1].FieldValue = AllocateCopyPool(AsciiStrSize("application/json"), "application/json");

  // Header 2: Content-Length
  AsciiSPrint(ContentLengthStr, sizeof(ContentLengthStr), "%d", JsonBodyLen);
  RequestHeaders[2].FieldName = AllocateCopyPool(AsciiStrSize("Content-Length"), "Content-Length");
  RequestHeaders[2].FieldValue = AllocateCopyPool(AsciiStrSize(ContentLengthStr), ContentLengthStr);

  // Setup request data
  ZeroMem(&RequestData, sizeof(RequestData));
  RequestData.Method = HttpMethodPost;
  RequestData.Url = AllocateCopyPool(AsciiStrSize(NOTIFY_PATH), NOTIFY_PATH);

  // Setup request message
  ZeroMem(&RequestMessage, sizeof(RequestMessage));
  RequestMessage.Data.Request = &RequestData;
  RequestMessage.HeaderCount = 3;
  RequestMessage.Headers = RequestHeaders;
  RequestMessage.BodyLength = JsonBodyLen;
  RequestMessage.Body = RequestBody;

  // Setup request token
  ZeroMem(&RequestToken, sizeof(RequestToken));
  Status = gBS->CreateEvent(0, TPL_NOTIFY, NULL, NULL, &RequestToken.Event);
  if (EFI_ERROR(Status)) {
    Print(L"PxeNotify: Failed to create request event: %r\n", Status);
    goto cleanup;
  }

  RequestToken.Status = EFI_NOT_READY;
  RequestToken.Message = &RequestMessage;

  // Send the request
  Print(L"PxeNotify: Sending POST request to %a:%d%a\n", NOTIFY_HOST, NOTIFY_PORT, NOTIFY_PATH);
  Status = Http->Request(Http, &RequestToken);
  if (EFI_ERROR(Status)) {
    Print(L"PxeNotify: HTTP Request failed: %r\n", Status);
    goto cleanup;
  }

  // Wait for request completion
  Print(L"PxeNotify: Waiting for request completion...\n");
  Status = WaitForEventSeconds(RequestToken.Event, 10);
  if (EFI_ERROR(Status)) {
    Print(L"PxeNotify: Request timeout: %r\n", Status);
    goto cleanup;
  }

  if (EFI_ERROR(RequestToken.Status)) {
    Print(L"PxeNotify: Request completed with error: %r\n", RequestToken.Status);
    Status = RequestToken.Status;
    goto cleanup;
  }

  Print(L"PxeNotify: Request sent successfully\n");

  // Setup response message
  ResponseBody = AllocateZeroPool(4096);
  if (!ResponseBody) {
    Print(L"PxeNotify: Failed to allocate response body\n");
    goto cleanup;
  }

  ZeroMem(&ResponseMessage, sizeof(ResponseMessage));
  ResponseMessage.BodyLength = 4096;
  ResponseMessage.Body = ResponseBody;

  // Setup response token
  ZeroMem(&ResponseToken, sizeof(ResponseToken));
  Status = gBS->CreateEvent(0, TPL_NOTIFY, NULL, NULL, &ResponseToken.Event);
  if (EFI_ERROR(Status)) {
    Print(L"PxeNotify: Failed to create response event: %r\n", Status);
    goto cleanup;
  }

  ResponseToken.Status = EFI_NOT_READY;
  ResponseToken.Message = &ResponseMessage;

  // Get response
  Print(L"PxeNotify: Waiting for response...\n");
  Status = Http->Response(Http, &ResponseToken);
  if (EFI_ERROR(Status)) {
    Print(L"PxeNotify: HTTP Response failed: %r\n", Status);
    goto cleanup;
  }

  // Wait for response completion
  Status = WaitForEventSeconds(ResponseToken.Event, 10);
  if (EFI_ERROR(Status)) {
    Print(L"PxeNotify: Response timeout: %r\n", Status);
    goto cleanup;
  }

  if (EFI_ERROR(ResponseToken.Status)) {
    Print(L"PxeNotify: Response completed with error: %r\n", ResponseToken.Status);
    Status = ResponseToken.Status;
    goto cleanup;
  }

  // Check response
  if (ResponseMessage.Data.Response) {
    Print(L"PxeNotify: HTTP Status Code: %d\n", ResponseMessage.Data.Response->StatusCode);
    if (ResponseMessage.BodyLength > 0 && ResponseBody) {
      Print(L"PxeNotify: Response Body: %a\n", ResponseBody);
    }
  }

  Print(L"PxeNotify: Notification sent successfully!\n");
  Status = EFI_SUCCESS;

cleanup:

  if (RequestToken.Event) 
    gBS->CloseEvent(RequestToken.Event);
  if (ResponseToken.Event) 
    gBS->CloseEvent(ResponseToken.Event);
  if (RequestBody) 
    FreePool(RequestBody);
  if (ResponseBody) 
    FreePool(ResponseBody);

  if (RequestHeaders) {
    for (Index = 0; Index < 3; Index++) {
      if (RequestHeaders[Index].FieldName) FreePool(RequestHeaders[Index].FieldName);
      if (RequestHeaders[Index].FieldValue) FreePool(RequestHeaders[Index].FieldValue);
    }
    FreePool(RequestHeaders);
  }

  if (RequestData.Url) 
    FreePool(RequestData.Url);
  
  if (Http) 
    Http->Configure(Http, NULL); // Unconfigure

  if (HttpChildHandle && ServiceBinding) {
    ServiceBinding->DestroyChild(ServiceBinding, HttpChildHandle);
  }

  if (Handles) 
    FreePool(Handles);

  return Status;
}