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

#ifndef Dvr_INCLUDED
#define Dvr_INCLUDED

#include <sys/poll.h>


namespace Omm {
namespace Dvb {

class Adapter;
class Remux;
class Service;

class Dvr
{
    friend class Adapter;
    friend class Device;

public:
    Dvr(Adapter* pAdapter, int num);
    ~Dvr();

    void openDvr();
    void closeDvr();
    void clearBuffer();
    void prefillBuffer();
    void startReadThread();
    void stopReadThread();
    bool readThreadRunning();

    Service* addService(Service* pService);
    void delService(Service* pService);

    std::istream* getStream();

private:
    void readThread();

    Adapter*                            _pAdapter;
    std::string                         _deviceName;
    int                                 _num;
    int                                 _fileDescDvr;
    Remux*                              _pRemux;
};

}  // namespace Omm
}  // namespace Dvb

#endif
