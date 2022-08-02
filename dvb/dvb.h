#ifndef __OMMDVB__
#define __OMMDVB__

struct DvbTransponder;
struct DvbService;
struct DvbStream;

int dvb_init(const char *conf_xml);
void dvb_open();
void dvb_close();
struct DvbTransponder* dvb_first_transponder(const char *service_name);
struct DvbService* dvb_service(struct DvbTransponder *transponder, const char *service_name);
int dvb_service_status(struct DvbService *service);
int dvb_service_scrambled(struct DvbService *service);
int dvb_service_has_audio(struct DvbService *service);
int dvb_service_has_sdvideo(struct DvbService *service);
struct DvbStream* dvb_stream(const char *service_name);
int dvb_read(struct DvbStream *stream, char *buf, int nbuf);
void free_transponder(struct DvbTransponder *transponder);
void free_service(struct DvbService *service);
void free_stream(struct DvbStream *stream);

#endif
