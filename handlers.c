#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "comms.h"
#include "cJSON.h"

// Forward declaration for interactive mode
void interactive_mode(char* ip);

char* handler_associate(int argc, char* argv[])
{
	if (argc < 6) {
		fprintf(stderr, "not enough arguments\n");
		exit(1);
	}
	char* plug_addr = argv[1];
	char* ssid = argv[3];
	char* password = argv[4];
	char* key_type = argv[5];

	errno = 0;
	char* endptr;
	int key_type_num = (int)strtol(key_type, &endptr, 10);
	if (errno || endptr == key_type) {
		fprintf(stderr, "invalid key type: %s\n", key_type);
		exit(1);
	}

	const char* template =
		"{\"netif\":{\"set_stainfo\":{\"ssid\":\"%s\",\"password\":"
		"\"%s\",\"key_type\":%d}}}";

	size_t len = snprintf(NULL, 0, template, ssid, password,
		key_type_num);
	len++;   // snprintf does not count the null terminator

	char* msg = calloc(1, len);
	snprintf(msg, len, template, ssid, password, key_type_num);

	char* response = hs100_send(plug_addr, msg);
	free(msg);
	return response;
}

char* handler_set_server(int argc, char* argv[])
{
	if (argc < 4) {
		fprintf(stderr, "not enough arguments\n");
		exit(1);
	}
	char* plug_addr = argv[1];
	char* server = argv[3];

	const char* template =
		"{\"cnCloud\":{\"set_server_url\":{\"server\":\"%s\"}}}";

	size_t len = snprintf(NULL, 0, template, server);
	len++;  // snprintf does not count the null terminator

	char* msg = calloc(1, len);
	snprintf(msg, len, template, server);

	char* response = hs100_send(plug_addr, msg);

	return response;
}

char* handler_set_alias(int argc, char* argv[])
{
	if (argc < 4) {
		fprintf(stderr, "not enough arguments\n");
		exit(1);
	}
	char* plug_addr = argv[1];
	char* alias = argv[3];   // adjust based on your command syntax

	// Build JSON payload for setting alias (exact method depends on device)
	const char* template = "{\"system\":{\"set_dev_alias\":{\"alias\":\"%s\"}}}";
	size_t len = snprintf(NULL, 0, template, alias) + 1;
	char* msg = calloc(1, len);
	snprintf(msg, len, template, alias);

	char* response = hs100_send(plug_addr, msg);
	free(msg);
	return response;
}

char* handler_set_relay_state(int argc, char* argv[])
{
	const char* template =
		"{\"context\":{\"child_ids\":[\"%s\"]},"
		"\"system\":{\"set_relay_state\":{\"state\":%c}}}";
	char* plug_addr = argv[1];
	char* onoff = argv[2];
	char* plug = argv[3];
	size_t len;
	char* msg, * response;

	if (argc < 4) {
		return NULL;
	}

	len = snprintf(NULL, 0, template, plug, (onoff[1] == 'n' ? '1' : '0'));
	len++;	/* snprintf does not count the null terminator */

	msg = calloc(1, len);
	snprintf(msg, len, template, plug, (onoff[1] == 'n' ? '1' : '0'));

	response = hs100_send(plug_addr, msg);

	return response;
}

char* handler_get_realtime(int argc, char* argv[])
{
	const char* template =
		"{\"context\":{\"child_ids\":[\"%s\"]},"
		"\"emeter\":{\"get_realtime\":{}}}";
	char* plug_addr = argv[1];
	char* plug = argv[3];
	size_t len;
	char* msg, * response;

	if (argc < 4) {
		return NULL;
	}

	len = snprintf(NULL, 0, template, plug);
	len++;	/* snprintf does not count the null terminator */

	msg = calloc(1, len);
	snprintf(msg, len, template, plug);

	response = hs100_send(plug_addr, msg);

	return response;
}

char* handler_outlet(int argc, char* argv[])
{
	if (argc < 5) {
		fprintf(stderr, "Usage: hs100 <ip> outlet <num> <on|off>\n");
		exit(1);
	}

	char* plug_addr = argv[1];
	int outlet_num = atoi(argv[3]);
	int state = (strcmp(argv[4], "on") == 0) ? 1 : 0;

	// First, get the sysinfo to check for children
	char* info_response = hs100_send(plug_addr, "{\"system\":{\"get_sysinfo\":{}}}");
	if (!info_response) {
		fprintf(stderr, "Failed to get device info\n");
		return NULL;
	}

	cJSON* json = cJSON_Parse(info_response);
	free(info_response);
	if (!json) {
		fprintf(stderr, "Invalid JSON from device\n");
		return NULL;
	}

	cJSON* sysinfo = cJSON_GetObjectItem(json, "system");
	if (sysinfo) sysinfo = cJSON_GetObjectItem(sysinfo, "get_sysinfo");
	cJSON* children = sysinfo ? cJSON_GetObjectItem(sysinfo, "children") : NULL;

	char* msg = NULL;

	if (children && cJSON_IsArray(children) && cJSON_GetArraySize(children) > 0) {
		// Multi-outlet device
		int child_count = cJSON_GetArraySize(children);
		if (outlet_num < 1 || outlet_num > child_count) {
			fprintf(stderr, "Invalid outlet number %d (1..%d)\n", outlet_num, child_count);
			cJSON_Delete(json);
			return NULL;
		}

		cJSON* child = cJSON_GetArrayItem(children, outlet_num - 1);
		char* child_id = cJSON_GetObjectItem(child, "id")->valuestring;

		const char* template = "{\"context\":{\"child_ids\":[\"%s\"]},\"system\":{\"set_relay_state\":{\"state\":%d}}}";
		size_t len = snprintf(NULL, 0, template, child_id, state) + 1;
		msg = calloc(1, len);
		snprintf(msg, len, template, child_id, state);
	}
	else {
		// Single-outlet device - only outlet 1 is valid
		if (outlet_num != 1) {
			fprintf(stderr, "This device has only one outlet. Use outlet 1.\n");
			cJSON_Delete(json);
			return NULL;
		}

		const char* template = "{\"system\":{\"set_relay_state\":{\"state\":%d}}}";
		size_t len = snprintf(NULL, 0, template, state) + 1;
		msg = calloc(1, len);
		snprintf(msg, len, template, state);
	}

	cJSON_Delete(json);

	char* response = hs100_send(plug_addr, msg);
	free(msg);
	return response;
}

char* handler_interactive(int argc, char* argv[]) {
	if (argc < 3) {
		fprintf(stderr, "Usage: hs100 <ip> interactive\n");
		exit(1);
	}
	char* plug_addr = argv[1];
	interactive_mode(plug_addr);
	return NULL;
}