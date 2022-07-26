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

#ifndef ElementaryStream_INCLUDED
#define ElementaryStream_INCLUDED

#include "DvbUtil.h"

namespace Omm {
namespace Dvb {


class ElementaryStreamPacket : public BitField
{
public:
    ElementaryStreamPacket();

    Poco::UInt8 getStreamId();
    Poco::UInt16 getSize();
    static const int getMaxSize();
    void* getDataAfterStartcodePrefix();
    void* getDataStart();

    bool isAudio();
    bool isVideo();

private:
    static const int    _maxSize;
    Poco::UInt16        _size;
};


}  // namespace Omm
}  // namespace Dvb

#endif
