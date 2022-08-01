/***************************************************************************|
|  OMM - Open Multimedia                                                    |
|                                                                           |
|  Copyright (C) 2009, 2010, 2011                                           |
|  Jörg Bakker                                                              |
|                                                                           |
|  This file is part of OMM.                                                |
|                                                                           |
|  OMM is free software: you can redistribute it and/or modify              |
|  it under the terms of the MIT License                                    |
 ***************************************************************************/

#include <iostream>
#include <fstream>

#include <Poco/Timestamp.h>

#include "Log.h"
// #include <Omm/Util.h>
#include "Device.h"
#include "Frontend.h"
#include "Transponder.h"
#include "Service.h"
#include "TransportStream.h"


void
recordService(std::string serviceName)
{
    Omm::Dvb::Device* pDevice = Omm::Dvb::Device::instance();
    Poco::Timestamp t;
    Poco::Timestamp::TimeDiff maxTime = 5000000;  // record approx. 5,000,000 microsec
    const unsigned int streamBufPacketCount = 500;

    Omm::Dvb::Transponder* pTransponder = pDevice->getFirstTransponder(serviceName);
    if (pTransponder) {
        Omm::Dvb::Service* pService = pTransponder->getService(serviceName);
        if (pService && pService->getStatus() == Omm::Dvb::Service::StatusRunning && !pService->getScrambled()
                && (pService->isAudio() || pService->isSdVideo())) {
            LOG(dvb, information, "recording service: " + serviceName);
            std::istream* pDvbStream = pDevice->getStream(serviceName);
            if (pDvbStream) {
                std::ofstream serviceStream((serviceName + std::string(".ts")).c_str());
                const std::streamsize bufSize = Omm::Dvb::TransportStreamPacket::Size * streamBufPacketCount;
                char buf[bufSize];
                while (t.elapsed() < maxTime) {
                    pDvbStream->read(buf, bufSize);
                    serviceStream.write(buf, bufSize);
                    LOG(dvb, debug, "received stream packets: " + Poco::NumberFormatter::format(streamBufPacketCount));
                }
                pDevice->freeStream(pDvbStream);
            }
        }
    }
}


int
main(int argc, char** argv)
{
    Omm::Dvb::Device* pDevice = Omm::Dvb::Device::instance();
    std::ifstream dvbXml;
    std::string serviceName;

    if (argc < 2 || argc > 3) {
        std::cerr << "usage: tunedvb [-s<service name>] <dvb config file> " << std::endl;
        return 1;
    }
    if (argc == 3) {
        serviceName = std::string(argv[1]).substr(2);
        dvbXml.open(argv[2]);
    }
    else {
        dvbXml.open(argv[1]);
    }

	pDevice->detectAdapters();
    pDevice->readXml(dvbXml);
    pDevice->open();

    if (serviceName.length()) {
        recordService(serviceName);
    }
    else {
        for (Omm::Dvb::Device::ServiceIterator it = pDevice->serviceBegin(); it != pDevice->serviceEnd(); ++it) {
            recordService(it->first);
        }
    }

    pDevice->close();

    return 0;
}
