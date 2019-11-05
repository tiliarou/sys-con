#include <switch.h>
#include <variant>
#include "log.h"

#include "SwitchUSBDevice.h"
#include "Controllers.h"
#include "SwitchHDLHandler.h"
#include "SwitchAbstractedPadHandler.h"

struct VendorEvent
{
    uint16_t vendor;
    Event event;
};

Result mainLoop()
{
    Result rc = 0;
    UsbHsInterface interfaces[16];
    s32 total_entries;
    std::vector<uint16_t> vendors = GetVendors();
    bool useAbstractedPad = hosversionBetween(5, 7);
    //hidPermitVibration(false);
    //hidPermitVibration(true);
    VendorEvent events[vendors.size()];
    std::vector<std::unique_ptr<SwitchVirtualGamepadHandler>> controllerInterfaces;

    WriteToLog("\n\nNew sysmodule session started");

    {
        int i = 0;
        for (auto &&vendor : vendors)
        {
            UsbHsInterfaceFilter filter;
            filter.Flags = UsbHsInterfaceFilterFlags_idVendor;
            filter.idVendor = vendor;
            if (vendor == VENDOR_SONY)
            {
                filter.Flags |= UsbHsInterfaceFilterFlags_idProduct;
                filter.idProduct = PRODUCT_DUALSHOCK3;
            }
            auto &&event = events[i] = {vendor, Event()};

            rc = usbHsCreateInterfaceAvailableEvent(&event.event, true, i, &filter);
            if (R_FAILED(rc))
                WriteToLog("Failed to open event ", event.vendor);
            else
                WriteToLog("Successfully created event ", event.vendor);
            ++i;
        }
    }

    controllerInterfaces.reserve(8);

    while (appletMainLoop())
    {

#ifdef __APPLET__
        hidScanInput();
        u64 kDown = hidKeysDown(CONTROLLER_P1_AUTO);

        if (kDown & KEY_Y)
        {
            for (int i = 0; i != 8; ++i)
            {
                bool isConnected = hidIsControllerConnected(static_cast<HidControllerID>(i));
                WriteToLog("Is controller ", i, " connected: ", isConnected);
                if (isConnected)
                {
                    HidControllerType type = hidGetControllerType(static_cast<HidControllerID>(i));
                    WriteToLog("Controller ", i, " type: ", type);
                }
            }
        }

        if (kDown & KEY_X)
        {
            WriteToLog("Sending rumble to all controllers\n");
            for (auto &&con : controllerInterfaces)
            {
                u32 *vibrationHandle = con->GetVibrationHandle();
                bool flag;
                Result rc = hidIsVibrationDeviceMounted(vibrationHandle, &flag);
                if (R_SUCCEEDED(rc))
                {
                    WriteToLog("Is vibration device mounted: ", flag);
                    HidVibrationValue value;
                    value.amp_high = 0.5f;
                    value.amp_low = 0.5f;
                    value.freq_high = 0.0f;
                    value.freq_low = 0.0f;
                    rc = hidSendVibrationValue(vibrationHandle, &value);
                    WriteToLog("SendVirationValue result: ", rc);
                    rc = hidGetActualVibrationValue(vibrationHandle, &value);
                    WriteToLog("hidGetActualVibrationValue result: ", rc);
                    if (R_SUCCEEDED(rc))
                    {
                        WriteToLog("Amp high: ", value.amp_high, " Amp low: ", value.amp_low, " Freq high: ", value.freq_high, " Freq low: ", value.freq_low);
                    }
                }
                else
                    WriteToLog("failed to check for vibration device");
            }
        }

        if (kDown & KEY_B)
            break;
#endif
        //Iterate over each event and check if it went off, then iterate over each vendor product to see which one fits
        for (auto &&event : events)
        {
            rc = eventWait(&event.event, 0);
            if (R_SUCCEEDED(rc))
            {
                WriteToLog("Succeeded event ", event.vendor);
                WriteToLog("Interfaces size: ", controllerInterfaces.size(), "; capacity: ", controllerInterfaces.capacity());

                auto &&products = GetVendorProducts(event.vendor);
                for (auto &&product : products)
                {
                    if (controllerInterfaces.size() == 8)
                    {
                        WriteToLog("Reached controller limit! skipping initialization");
                        break;
                    }

                    UsbHsInterfaceFilter tempFilter;
                    tempFilter.Flags = UsbHsInterfaceFilterFlags_idVendor | UsbHsInterfaceFilterFlags_idProduct;
                    tempFilter.idVendor = event.vendor;
                    tempFilter.idProduct = product;
                    rc = usbHsQueryAvailableInterfaces(&tempFilter, interfaces, sizeof(interfaces), &total_entries);

                    if (R_FAILED(rc))
                        continue;
                    if (total_entries == 0)
                        continue;

                    std::unique_ptr<SwitchVirtualGamepadHandler> switchHandler;
                    if (useAbstractedPad)
                        switchHandler = std::make_unique<SwitchAbstractedPadHandler>(ConstructControllerFromType(GetControllerTypeFromIds(event.vendor, product), std::make_unique<SwitchUSBDevice>(interfaces, total_entries)));
                    else
                        switchHandler = std::make_unique<SwitchHDLHandler>(ConstructControllerFromType(GetControllerTypeFromIds(event.vendor, product), std::make_unique<SwitchUSBDevice>(interfaces, total_entries)));

                    rc = switchHandler->Initialize();
                    if (R_SUCCEEDED(rc))
                    {
                        controllerInterfaces.push_back(std::move(switchHandler));
                        WriteToLog("Interface created successfully on product ", product);
                    }
                    else
                    {
                        WriteToLog("Error creating interface for product ", product, " with error ", rc);
                    }
                }
            }
        }

        //On interface change event, check if any devices were removed, and erase them from memory appropriately
        rc = eventWait(usbHsGetInterfaceStateChangeEvent(), 0);
        if (R_SUCCEEDED(rc))
        {
            WriteToLog("Interface state was changed");
            eventClear(usbHsGetInterfaceStateChangeEvent());

            rc = usbHsQueryAcquiredInterfaces(interfaces, sizeof(interfaces), &total_entries);
            if (R_SUCCEEDED(rc))
            {
                for (auto it = controllerInterfaces.begin(); it != controllerInterfaces.end(); ++it)
                {
                    bool found_flag = false;

                    for (auto &&ptr : (*it)->GetController()->GetDevice()->GetInterfaces())
                    {
                        //We check if a device was removed by comparing the controller's interfaces and the currently acquired interfaces
                        //If we didn't find a single matching interface ID, we consider a controller removed
                        for (int i = 0; i != total_entries; ++i)
                        {
                            if (interfaces[i].inf.ID == static_cast<SwitchUSBInterface *>(ptr.get())->GetID())
                            {
                                found_flag = true;
                                break;
                            }
                        }
                    }

                    if (!found_flag)
                    {
                        WriteToLog("Erasing controller! ", (*it)->GetController()->GetType());
                        controllerInterfaces.erase(it--);
                        WriteToLog("Controller erased!");
                    }
                }
            }
        }

#ifdef __APPLET__
        consoleUpdate(nullptr);
#else
        svcSleepThread(1e+7L);
#endif
    }

    //After we break out of the loop, close all events and exit
    for (auto &&event : events)
    {
        WriteToLog("Destroying event " + event.vendor);
        usbHsDestroyInterfaceAvailableEvent(&event.event, 0);
    }

    //controllerInterfaces.clear();
    return rc;
}