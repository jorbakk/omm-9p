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

#ifndef Remux_INCLUDED
#define Remux_INCLUDED

#include <sys/poll.h>

#include <Poco/Thread.h>
#include <Poco/Mutex.h>

#include "TransportStream.h"
#include "Service.h"
//#include "Stream.h"
//#include "../AvStream.h"


namespace Omm {
namespace Dvb {


class Remux;

class TsPacketBlock : public TransportStreamPacketBlock
{
public:
    TsPacketBlock(Remux* pRemux) : _pRemux(pRemux) {}

private:
    virtual void free();

    Remux*  _pRemux;
};


class Remux
{
    friend class TsPacketBlock;

public:
    Remux(int multiplex);
    ~Remux();

    Service* addService(Service* pService);
    void delService(Service* pService);

    void startRemux();
    void stopRemux();
    void waitForStopRemux();
    void flush();

private:
    TransportStreamPacket* getTransportStreamPacket();
    TsPacketBlock* getFreePacketBlock();
    void putFreePacketBlock(TsPacketBlock* pPacketBlock);
    void queuePacketBlock(TsPacketBlock* pPacketBlock);
    TsPacketBlock* readPacketBlock();
    void readThread();
    bool readThreadRunning();
    void queueThread();
    bool queueThreadRunning();

    int                                                 _multiplex;
    std::vector<Service*>                               _services;
//    std::map<Poco::UInt16, ElementaryTransportStream*>  _pStreams;

    Poco::FastMutex                                     _remuxLock;
    struct pollfd                                       _fileDescPoll[1];
    const int                                           _readTimeout;
    Poco::Thread*                                       _pReadThread;
    Poco::RunnableAdapter<Remux>                        _readThreadRunnable;
    bool                                                _readThreadRunning;
    Poco::Thread*                                       _pQueueThread;
    Poco::RunnableAdapter<Remux>                        _queueThreadRunnable;
    bool                                                _queueThreadRunning;

    const int                                           _packetBlockQueueSize;
    Poco::Condition                                     _packetBlockQueueReadCondition;
    std::queue<TsPacketBlock*>                          _packetBlockQueue;
    std::stack<TsPacketBlock*>                          _packetPool;
};


//class ElementaryTransportStream
//{
//public:
//private:
//    void dispatchThread();
//    bool dispatchThreadRunning();
//
//    Poco::UInt16                                        _pid;
//    std::queue<TransportStreamPacket*>                  _packetQueue;
//    Poco::FastMutex                                     _streamLock;
//    Poco::Thread*                                       _pDispatchThread;
//    Poco::RunnableAdapter<ElementaryTransportStream>    _dispatchThreadRunnable;
//    bool                                                _dispatchThreadRunning;
//};


}  // namespace Omm
}  // namespace Dvb

#endif
