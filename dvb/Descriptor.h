/***************************************************************************|
|  OMM - Open Multimedia                                                    |
|                                                                           |
|  Copyright (C) 2009, 2010, 2011, 2012, 2022                               |
|  Jörg Bakker                                                              |
|                                                                           |
|  This file is part of OMM.                                                |
|                                                                           |
|  OMM is free software: you can redistribute it and/or modify              |
|  it under the terms of the MIT License                                    |
 ***************************************************************************/

#ifndef Descriptor_INCLUDED
#define Descriptor_INCLUDED


#include "DvbUtil.h"

namespace Omm {
namespace Dvb {

class Service;

class Descriptor : public BitField
{
public:
//    Descriptor(void* data);
    static Descriptor* createDescriptor(void* data);

    virtual void foo() {}
    Poco::UInt8 getId();
    Poco::UInt8 getDescriptorLength();
    Poco::UInt8 getContentLength();

protected:
    void* content();
};


class NetworkNameDescriptor : public Descriptor
{
public:
    std::string getNetworkName();
};


class ServiceDescriptor : public Descriptor
{
public:
    Poco::UInt8 serviceType();
    std::string providerName();
    std::string serviceName();
};


class ServiceListDescriptor : public Descriptor
{
public:
    Poco::UInt8 serviceCount();
    Poco::UInt16 serviceId(Poco::UInt8 index);
    Poco::UInt8 serviceType(Poco::UInt8 index);
};


class SatelliteDeliverySystemDescriptor : public Descriptor
{
public:
    unsigned int frequency();
    /// returns frequency in kHz
    std::string orbitalPosition();
    std::string polarization();
    std::string modulationSystem();
    std::string modulationType();
    Poco::UInt32 symbolRate();
    std::string fecInner();
};


class TerrestrialDeliverySystemDescriptor : public Descriptor
{
public:
    unsigned int centreFrequency();
    std::string bandwidth();
    std::string priority();
    bool timeSlicingIndicator();
    bool MpeFecIndicator();
    std::string constellation();
    std::string hierarchyInformation();
    std::string codeRateHpStream();
    std::string codeRateLpStream();
    std::string guardInterval();
    std::string transmissionMode();
    bool otherFrequencyFlag();
};


class FrequencyListDescriptor : public Descriptor
{
public:
    std::string codingType();
    Poco::UInt8 centreFrequencyCount();
    unsigned int centreFrequency(Poco::UInt8 index);
};


class CellFrequencyLinkDescriptor : public Descriptor
{
public:
};


}  // namespace Omm
}  // namespace Dvb

#endif
