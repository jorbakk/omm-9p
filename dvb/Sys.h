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

#ifndef Sys_INCLUDED
#define Sys_INCLUDED

#include <string>
#include <vector>

struct udev;

namespace Omm {
namespace Sys {

class Device;

class System
{
public:
    static const std::string DeviceTypeOther;
    static const std::string DeviceTypeDvb;
    static const std::string DeviceTypeDisk;

    /* static System* instance(); */
    System();
    ~System();

    void getDevicesForType(std::vector<Device*>& devices, const std::string& deviceType);

private:

    /* static System*     _pInstance; */
    /* SysImpl*           _pImpl; */
    struct udev*       _pUdev;
};


class Device
{
public:
    Device(const std::string& id, const std::string& type = System::DeviceTypeOther, const std::string& node = "");

    std::string getId();
    std::string getType();
    std::string getNode();

private:
    std::string     _id;
    std::string     _type;
    std::string     _node;
};

}  // namespace Sys
}  // namespace Omm

#endif
