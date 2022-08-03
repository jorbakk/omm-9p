#ifndef __OMMDVB__
#define __OMMDVB__

extern const int dvb_transport_stream_packet_size;

struct DvbStream;

int dvb_init(const char *conf_xml);
void dvb_open();
void dvb_close();

struct DvbStream* dvb_stream(const char *service_name);
int dvb_read_stream(struct DvbStream *stream, char *buf, int nbuf);
void dvb_free_stream(struct DvbStream *stream);

#endif
