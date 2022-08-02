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

struct DvbTransponder {
	Omm::Dvb::Transponder* pTransponder;
};

struct DvbService {
	Omm::Dvb::Service* pService;
};

struct DvbStream {
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


DvbTransponder*
dvb_first_transponder(const char *service_name)
{
	DvbTransponder *ret = (DvbTransponder*)malloc(sizeof(DvbTransponder));
	ret->pTransponder = Omm::Dvb::Device::instance()->getFirstTransponder(service_name);
	if (!ret->pTransponder) {
		return NULL;
	}
	return ret;
}


DvbService*
dvb_service(DvbTransponder *transponder, const char *service_name)
{
	if (!transponder->pTransponder) {
		return NULL;
	}
	DvbService *ret = (DvbService*)malloc(sizeof(DvbService));
	ret->pService = transponder->pTransponder->getService(service_name);
	if (!ret->pService) {
		return NULL;
	}
	return ret;
}


int
dvb_service_status(DvbService *service)
{
	if (!service->pService) {
		return -1;
	}
	if (service->pService->getStatus() == Omm::Dvb::Service::StatusUndefined) {
		return DvbServiceStatusUndefined;
	}
	if (service->pService->getStatus() == Omm::Dvb::Service::StatusNotRunning) {
		return DvbServiceStatusNotRunning;
	}
	if (service->pService->getStatus() == Omm::Dvb::Service::StatusStartsShortly) {
		return DvbServiceStatusStartsShortly;
	}
	if (service->pService->getStatus() == Omm::Dvb::Service::StatusPausing) {
		return DvbServiceStatusPausing;
	}
	if (service->pService->getStatus() == Omm::Dvb::Service::StatusRunning) {
		return DvbServiceStatusRunning;
	}
	if (service->pService->getStatus() == Omm::Dvb::Service::StatusOffAir) {
		return DvbServiceStatusOffAir;
	}
	return -1;
}


int
dvb_service_scrambled(DvbService *service)
{
	if (!service->pService) {
		return -1;
	}
	return service->pService->getScrambled() ? 1 : 0;
}


int
dvb_service_has_audio(DvbService *service)
{
	if (!service->pService) {
		return -1;
	}
	return service->pService->isAudio() ? 1 : 0;
}


int
dvb_service_has_sdvideo(DvbService *service)
{
	if (!service->pService) {
		return -1;
	}
	return service->pService->isSdVideo() ? 1 : 0;
}


int
dvb_service_has_hdvideo(DvbService *service)
{
	if (!service->pService) {
		return -1;
	}
	return service->pService->isHdVideo() ? 1 : 0;
}


DvbStream*
dvb_stream(const char *service_name)
{
	DvbStream *ret = (DvbStream*)malloc(sizeof(DvbStream));
	ret->pByteQueue = Omm::Dvb::Device::instance()->getByteQueue(service_name);
	if (!ret->pByteQueue) {
		return NULL;
	}
	return ret;
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
dvb_free_transponder(DvbTransponder *transponder)
{
	if (!transponder) {
		return;
	}
	delete transponder->pTransponder;
	free(transponder);
}


void
dvb_free_service(DvbService *service)
{
	if (!service) {
		return;
	}
	delete service->pService;
	free(service);
}


void
dvb_free_stream(DvbStream *stream)
{
	if (!stream) {
		return;
	}
	free(stream);
}
