#include "Device.h"
#include "Frontend.h"
#include "Transponder.h"
#include "Service.h"
#include "TransportStream.h"
#include "AvStream.h"

extern "C" {
#include "dvb.h"
}

const int dvb_transport_stream_packet_size = Omm::Dvb::TransportStreamPacket::Size;

struct DvbStream {
	Omm::Dvb::Transponder* pTransponder;
	Omm::Dvb::Service* pService;
	Omm::AvStream::ByteQueue* pByteQueue;
};


int
dvb_init(const char *conf_xml)
{
    Omm::Dvb::Device* pDevice = Omm::Dvb::Device::instance();
	pDevice->detectAdapters();
	std::ifstream conf_xml_stream(conf_xml);
    pDevice->readXml(conf_xml_stream);
    return 0;
}


void
dvb_open()
{
	Omm::Dvb::Device::instance()->open();
}


void
dvb_close()
{
	Omm::Dvb::Device::instance()->close();
}


DvbStream*
dvb_stream(const char *service_name)
{
	DvbStream *stream = (DvbStream*)malloc(sizeof(DvbStream));

	stream->pTransponder = Omm::Dvb::Device::instance()->getFirstTransponder(service_name);
	if (stream->pTransponder == NULL) {
		return NULL;
	}
	stream->pService = stream->pTransponder->getService(service_name);
	if (stream->pService == NULL || 
		stream->pService->getStatus() != Omm::Dvb::Service::StatusRunning ||
		stream->pService->getScrambled() ||
		(!stream->pService->isAudio() && !stream->pService->isSdVideo())) {
		delete stream->pTransponder;
		free(stream);
		return NULL;
	}
	stream->pByteQueue = Omm::Dvb::Device::instance()->getByteQueue(service_name);
	if (!stream->pByteQueue) {
		delete stream->pTransponder;
		delete stream->pService;
		free(stream);
		return NULL;
	}
	return stream;
}


int
dvb_read_stream(DvbStream *stream, char *buf, int nbuf)
{
	if (!stream->pByteQueue) {
		return -1;
	}
	return stream->pByteQueue->readSome(buf, nbuf);
}


void
dvb_free_stream(DvbStream *stream)
{
	if (!stream) {
		return;
	}
	// delete stream->pTransponder;
	// delete stream->pService;
	Omm::Dvb::Device::instance()->stopService(stream->pService);
	free(stream);
}
