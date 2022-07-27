/***************************************************************************|
|  OMM - Open Multimedia                                                    |
|                                                                           |
|  Copyright (C) 2009, 2010, 2011                                           |
|  Jörg Bakker                                                              |
|                                                                           |
|  This file is part of OMM.                                                |
|                                                                           |
|  OMM is free software: you can redistribute it and/or modify              |
|  it under the terms of the MIT License.                                   |
***************************************************************************/

#include <libudev.h>

#include "Poco/Environment.h"

#include "Log.h"
#include "Sys.h"

namespace Omm {
namespace Sys {


const std::string Sys::System::DeviceTypeOther("");
const std::string Sys::System::DeviceTypeDvb("dvb");
const std::string Sys::System::DeviceTypeDisk("block");


System::System()
{
    _pUdev = udev_new();
    if (!_pUdev) {
        LOG(sys, error, "initialization of udev failed");
    }
}


System::~System()
{
    udev_unref(_pUdev);
}


void
System::getDevicesForType(std::vector<Device*>& devices, const std::string& deviceType)
{
    struct udev_enumerate* pUdevEnumerate = udev_enumerate_new(_pUdev);

    udev_enumerate_add_match_subsystem(pUdevEnumerate, deviceType.c_str());
	udev_enumerate_scan_devices(pUdevEnumerate);
    struct udev_list_entry* deviceList;
    deviceList = udev_enumerate_get_list_entry(pUdevEnumerate);

    struct udev_list_entry* deviceIterator;
    udev_list_entry_foreach(deviceIterator, deviceList) {
        std::string deviceId(udev_list_entry_get_name(deviceIterator));
		udev_device* device = udev_device_new_from_syspath(_pUdev, deviceId.c_str());
        std::string deviceNode(udev_device_get_devnode(device));

        devices.push_back(new Device(deviceId, deviceType, deviceNode));

        udev_device_unref(device);
    }

    udev_enumerate_unref(pUdevEnumerate);
}


Device::Device(const std::string& id, const std::string& type, const std::string& node) :
_id(id),
_node(node)
{
}


std::string
Device::getId()
{
    return _id;
}


std::string
Device::getType()
{
    return _type;
}


std::string
Device::getNode()
{
    return _node;
}


}  // namespace Sys
} // namespace Omm
