/** @file
  UEFI Component Name and Name2 protocol for the Sunxi OHCI driver.

  Copyright (c) 2013-2015 Intel Corporation.
  Copyright (c) 2026, carpi-os contributors.
  Portions derived from edk2-a733 (github.com/vehoelite/edk2-a733),
  Copyright (c) 2026 carpi-os contributors, BSD-2-Clause-Patent.
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "Ohci.h"

//
// EFI Component Name Functions
//
EFI_STATUS
EFIAPI
SunxiOhciComponentNameGetDriverName (
  IN  EFI_COMPONENT_NAME_PROTOCOL  *This,
  IN  CHAR8                        *Language,
  OUT CHAR16                       **DriverName
  );

EFI_STATUS
EFIAPI
SunxiOhciComponentNameGetControllerName (
  IN  EFI_COMPONENT_NAME_PROTOCOL  *This,
  IN  EFI_HANDLE                   ControllerHandle,
  IN  EFI_HANDLE                   ChildHandle        OPTIONAL,
  IN  CHAR8                        *Language,
  OUT CHAR16                       **ControllerName
  );

//
// EFI Component Name Protocol
//
GLOBAL_REMOVE_IF_UNREFERENCED EFI_COMPONENT_NAME_PROTOCOL  gSunxiOhciComponentName = {
  SunxiOhciComponentNameGetDriverName,
  SunxiOhciComponentNameGetControllerName,
  "eng"
};

//
// EFI Component Name 2 Protocol
//
GLOBAL_REMOVE_IF_UNREFERENCED EFI_COMPONENT_NAME2_PROTOCOL  gSunxiOhciComponentName2 = {
  (EFI_COMPONENT_NAME2_GET_DRIVER_NAME)SunxiOhciComponentNameGetDriverName,
  (EFI_COMPONENT_NAME2_GET_CONTROLLER_NAME)SunxiOhciComponentNameGetControllerName,
  "en"
};

GLOBAL_REMOVE_IF_UNREFERENCED EFI_UNICODE_STRING_TABLE  mSunxiOhciDriverNameTable[] = {
  { "eng;en", L"Sunxi USB OHCI Driver" },
  { NULL,     NULL                    }
};

/**
  Retrieves a Unicode string that is the user readable name of the driver.

  @param  This[in]              A pointer to the EFI_COMPONENT_NAME2_PROTOCOL or
                                EFI_COMPONENT_NAME_PROTOCOL instance.
  @param  Language[in]          A pointer to a Null-terminated ASCII string
                                array indicating the language.
  @param  DriverName[out]       A pointer to the Unicode string to return.

  @retval EFI_SUCCESS           The Unicode string for the Driver specified by
                                This and the language specified by Language was
                                returned in DriverName.
  @retval EFI_INVALID_PARAMETER Language is NULL.
  @retval EFI_INVALID_PARAMETER DriverName is NULL.
  @retval EFI_UNSUPPORTED       The driver specified by This does not support
                                the language specified by Language.

**/
EFI_STATUS
EFIAPI
SunxiOhciComponentNameGetDriverName (
  IN  EFI_COMPONENT_NAME_PROTOCOL  *This,
  IN  CHAR8                        *Language,
  OUT CHAR16                       **DriverName
  )
{
  return LookupUnicodeString2 (
           Language,
           This->SupportedLanguages,
           mSunxiOhciDriverNameTable,
           DriverName,
           (BOOLEAN)(This == &gSunxiOhciComponentName)
           );
}

/**
  Retrieves a Unicode string that is the user readable name of the controller
  that is being managed by a driver.

  @param  This[in]              A pointer to the EFI_COMPONENT_NAME2_PROTOCOL or
                                EFI_COMPONENT_NAME_PROTOCOL instance.
  @param  ControllerHandle[in]  The handle of a controller that the driver
                                specified by This is managing.
  @param  ChildHandle[in]       The handle of the child controller to retrieve
                                the name of.
  @param  Language[in]          A pointer to a Null-terminated ASCII string
                                array indicating the language.
  @param  ControllerName[out]   A pointer to the Unicode string to return.

  @retval EFI_SUCCESS           The Unicode string for the user readable name in
                                the language specified by Language for the
                                driver specified by This was returned in
                                DriverName.
  @retval EFI_INVALID_PARAMETER ControllerHandle is not a valid EFI_HANDLE.
  @retval EFI_INVALID_PARAMETER ChildHandle is not NULL and it is not a valid
                                EFI_HANDLE.
  @retval EFI_INVALID_PARAMETER Language is NULL.
  @retval EFI_INVALID_PARAMETER ControllerName is NULL.
  @retval EFI_UNSUPPORTED       The driver specified by This is not currently
                                managing the controller specified by
                                ControllerHandle and ChildHandle.
  @retval EFI_UNSUPPORTED       The driver specified by This does not support
                                the language specified by Language.

**/
EFI_STATUS
EFIAPI
SunxiOhciComponentNameGetControllerName (
  IN  EFI_COMPONENT_NAME_PROTOCOL  *This,
  IN  EFI_HANDLE                   ControllerHandle,
  IN  EFI_HANDLE                   ChildHandle        OPTIONAL,
  IN  CHAR8                        *Language,
  OUT CHAR16                       **ControllerName
  )
{
  EFI_STATUS            Status;
  USB_OHCI_HC_DEV       *OhciDev;
  EFI_USB2_HC_PROTOCOL  *Usb2Hc;

  //
  // This is a device driver, so ChildHandle must be NULL.
  //
  if (ChildHandle != NULL) {
    return EFI_UNSUPPORTED;
  }

  //
  // Make sure this driver is currently managing ControllerHandle
  //
  Status = EfiTestManagedDevice (
             ControllerHandle,
             gSunxiOhciDriverBinding.DriverBindingHandle,
             &gEfiPciIoProtocolGuid
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Get the device context
  //
  Status = gBS->OpenProtocol (
                  ControllerHandle,
                  &gEfiUsb2HcProtocolGuid,
                  (VOID **)&Usb2Hc,
                  gSunxiOhciDriverBinding.DriverBindingHandle,
                  ControllerHandle,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  OhciDev = USB_OHCI_HC_DEV_FROM_THIS (Usb2Hc);

  return LookupUnicodeString2 (
           Language,
           This->SupportedLanguages,
           OhciDev->ControllerNameTable,
           ControllerName,
           (BOOLEAN)(This == &gSunxiOhciComponentName)
           );
}
