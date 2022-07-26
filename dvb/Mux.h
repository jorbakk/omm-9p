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

#ifndef Mux_INCLUDED
#define Mux_INCLUDED

#include <Poco/Thread.h>

#include "Stream.h"
#include "../AvStream.h"


namespace Omm {
namespace Dvb {

// Mux is currently not needed (was attempt to mux elementary streams coming from demuxer)

class InStream;

class Mux
{
    friend class InStream;

public:
    Mux();
    ~Mux();

    void addStream(Stream* pStream);
    std::istream* getMux();

    void start();
    void stop();

private:
    std::vector<InStream*>              _inStreams;
    std::istream*                       _pOutStream;
    AvStream::ByteQueue                 _byteQueue;
};


class InStream
{
public:
    InStream(Stream* pStream, Mux* pMux);

    void startReadThread();
    void stopReadThread();
    bool readThreadRunning();

private:
    void readThread();

    Stream*                             _pStream;
    Mux*                                _pMux;

    Poco::Thread*                       _pReadThread;
    Poco::RunnableAdapter<InStream>     _readThreadRunnable;
    bool                                _readThreadRunning;
    Poco::FastMutex                     _readThreadLock;
    const int                           _readTimeout;
};


}  // namespace Omm
}  // namespace Dvb

#endif
