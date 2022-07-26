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

#ifndef TransportStream_INCLUDED
#define TransportStream_INCLUDED

#include <Poco/Thread.h>
#include <Poco/Mutex.h>
#include <Poco/ScopedLock.h>
#include "Poco/AtomicCounter.h"

#include "DvbUtil.h"


namespace Omm {
namespace Dvb {

class Stream;
class Remux;
class TransportStreamPacket;


class TransportStreamPacketBlock
{
public:
    static const int SizeInPackets;
    static const int Size;

    TransportStreamPacketBlock();
    ~TransportStreamPacketBlock();

    Poco::UInt8* getPacketData() { return _pPacketData; }
    TransportStreamPacket* getPacket();

    virtual void free() {}

    void incRefCounter()
    {
//        LOG(dvb, debug, "packet block inc ref counter: " + Poco::NumberFormatter::format(_refCounter));
        ++_refCounter;
    }

    void decRefCounter()
    {
//        LOG(dvb, debug, "packet block dec ref counter: " + Poco::NumberFormatter::format(_refCounter));
        if (!(--_refCounter)) {
//            LOG(dvb, debug, "packet block free");
            _packetIndex = 0;
            _refCounter = 1;
            free();
        }
    }

private:
    std::vector<TransportStreamPacket*>     _packetBlock;
    Poco::UInt8*                            _pPacketData;
    int                                     _packetIndex;
    Poco::AtomicCounter                     _refCounter;
};


class TransportStreamPacket : public BitField
{
    friend class TransportStreamPacketBlock;

public:
    enum { ScrambledNone = 0x00, ScrambledReserved = 0x01, ScrambledEvenKey = 0x10, ScrambledOddKey = 0x11 };
    enum { AdaptionFieldPayloadOnly = 0x01, AdaptionFieldOnly = 0x10, AdaptionFieldAndPayload = 0x11 };

    static const Poco::UInt8   SyncByte;
    static const int           Size;
    static const int           HeaderSize;
    static const int           PayloadSize;

    TransportStreamPacket(bool allocateData = true);
    ~TransportStreamPacket();

//    void writePayloadFromStream(Stream* pStream, int timeout);
    void clearPayload();
    void stuffPayload(int actualPayloadSize);

    // header fields
    void setTransportErrorIndicator(bool uncorrectableError);
    void setPayloadUnitStartIndicator(bool PesOrPsi);
    void setTransportPriority(bool high);
    Poco::UInt16 getPacketIdentifier();
    void setPacketIdentifier(Poco::UInt16 pid);
    void setScramblingControl(Poco::UInt8 scramble);
    void setAdaptionFieldExists(Poco::UInt8 exists);
    void setContinuityCounter(Poco::UInt8 counter);
    void setPointerField(Poco::UInt8 pointer);

    // optional header adaption fields
    void setAdaptionFieldLength(Poco::UInt8 length);
    void clearAllAdaptionFieldFlags();
    void setDiscontinuityIndicator(bool discontinuity);
    void setRandomAccessIndicator(bool randomAccess);
    void setElementaryStreamPriorityIndicator(bool high);
    void setPcrFlag(bool containsPcr);
    void setOPcrFlag(bool containsOPcr);
    void setSplicingPointFlag(bool spliceCountdownPresent);
    void setTransportPrivateDataFlag(bool privateDataPresent);
    void setExtensionFlag(bool extensionPresent);
    void setPcr(Poco::UInt64 base, Poco::UInt8 padding, Poco::UInt16 extension);
    void setSpliceCountdown(Poco::UInt8 countdown);
    void setStuffingBytes(int count);

    void incRefCounter() const
    {
        if (_pPacketBlock) {
            _pPacketBlock->incRefCounter();
        }
        else {
            ++_refCounter;
        }
    }

    void decRefCounter() const
    {
        if (_pPacketBlock) {
            _pPacketBlock->decRefCounter();
        }
        else if (!(--_refCounter)) {
            delete this;
        }
    }

private:
    int                             _adaptionFieldSize;
    bool                            _adaptionFieldPcrSet;
    bool                            _adaptionFieldSplicingPointSet;
    mutable Poco::AtomicCounter     _refCounter;
    TransportStreamPacketBlock*     _pPacketBlock;
};


}  // namespace Omm
}  // namespace Dvb

#endif
