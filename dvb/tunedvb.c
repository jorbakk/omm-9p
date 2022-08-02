#include "dvb.h"

void
record_service(const char *service_name)
{
}


int
main(int argc, char **argv)
{
	dvb_init(argv[1]);
	dvb_open();
	record_service(argv[2]);
	dvb_close();
	return 0;
}
