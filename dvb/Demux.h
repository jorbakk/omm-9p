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

#ifndef Demux_INCLUDED
#define Demux_INCLUDED

#include <map>
#include <sys/poll.h>


namespace Omm {
namespace Dvb {

class Adapter;
class TransportStreamPacket;
class Multiplex;
class Remux;
class PidSelector;


class Demux
{
    friend class Adapter;
    friend class Device;

public:
    enum Target { TargetDemux, TargetDvr };

    Demux(Adapter* pAdapter, int num);
    ~Demux();

    bool selectService(Service* pService, Target target, bool blocking = true);
    bool unselectService(Service* pService);
    bool runService(Service* pService, bool run = true);

    bool selectStream(Stream* pStream, Target target, bool blocking = true);
    bool unselectStream(Stream* pStream);
    bool runStream(Stream* pStream, bool run = true);
    bool setSectionFilter(Stream* pStream, Poco::UInt8 tableId);

    void readStream(Stream* pStream, Poco::UInt8* buf, int size, int timeout);
    bool readSection(Section* pSection);
    bool readTable(Table* pTable);

private:
    Adapter*                                _pAdapter;
    std::string                             _deviceName;
    int                                     _num;
    std::map<Poco::UInt16, PidSelector*>    _pidSelectors;
};

}  // namespace Omm
}  // namespace Dvb

#endif
