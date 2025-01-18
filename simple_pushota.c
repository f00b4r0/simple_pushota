//
//  simple_pushota.c
//
//
//  (C) 2021-2022 Thibaut VARENE
//  License: GPLv2 - http://www.gnu.org/licenses/gpl-2.0.html
//

/**
 * @file
 * (Very) basic push OTA update system.
 *
 * This is a very crude implementation, not suitable for production environments.
 * It (ab)uses a classic HTTP POST request sent by e.g. curl in the following fashion:
 *
 * `curl <esphost>:OTA_PORT --data-binary @build/project.bin`
 * OTA_PORT is configurable through CONFIG_SIMPLE_PUSHOTA_PORT.
 *
 * It will extract payload length from the request headers and write the binary payload to flash as is.
 * It assumes that headers are separated from payload by "\r\n\r\n" (per RFC - curl satisfies this condition).
 *
 * The second stage bootloader verifies app integrity, see:
 * https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/startup.html#second-stage-bootloader
 *
 * To abort the update before it begins, send a "DELETE" request using e.g. `curl <esphost>:OTA_PORT -X DELETE`
 * To query the current firmware version, if CONFIG_SIMPLE_PUSHOTA_GETVERSION is defined, send a "GET" request using e.g. `curl <esphost>:OTA_PORT`
 */

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_ota_ops.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"

#define OTA_PORT		CONFIG_SIMPLE_PUSHOTA_PORT
#define OTA_BUFSIZE		1024

#define KEEPALIVE_IDLE		5	// delay (s) before starting sending keepalives
#define KEEPALIVE_INTERVAL	5	// keepalive probes period (s)
#define KEEPALIVE_COUNT		3	// max unanswered before timeout

#ifdef CONFIG_SIMPLE_PUSHOTA_ENABLED
static const char * TAG = "pushota";

/**
 * Perform OTA firmware update.
 * Parse basic HTTP requests containing either:
 * - POST request with:
 *  - Header "Content-Length": binary image size
 *  - Payload: raw binary image
 * - DELETE request with no content to abort the OTA process
 * - GET request (if enabled via CONFIG_SIMPLE_PUSHOTA_GETVERSION) to query the current firmware version
 * @param sock accept()'d input socket
 * @return execution status: it is safe to call esp_restart() after this returns ESP_OK
 */
static int ota_receive(int sock)
{
	char buf[OTA_BUFSIZE];
	const esp_partition_t *upart = esp_ota_get_next_update_partition(NULL);
	esp_ota_handle_t ota_handle;
	char c, *s, *binstart;
	const char *needle, *status = "500 Internal Server Error";
	int binlen, len, ret = ESP_FAIL;

	// assume the HTTP headers fit the buffer
	s = buf;
	do {
		// recv until we detect end of headers (or buffer full)
		len = recv(sock, s, sizeof(buf)-1 - (s-buf), 0);	// last char must be '\0'
		if (len <= 0)
			return ESP_FAIL;

		s[len] = '\0';	// string ops need null-terminated haystack, make sure it is

		// locate end of header
		needle = "\r\n\r\n";	// separator between headers and content
		binstart = strstr(s, needle);

		s += len;
	} while ((s-buf < sizeof(buf)-1) && !binstart);

	if (!binstart) {
		status = "431 Request Header Fields Too Large";
		goto outstatus;
	}

	binstart += strlen(needle);	// stays within buf

	// leftover buffer, start of app image
	len = s - binstart;

	// move null termination to header end before further processing
	c = *binstart;
	*binstart = '\0';

	/*
	 parse POST header (no space around '/' in Accept), e.g.:

	 POST / HTTP/1.1
	 Host: localhost:8888
	 Authorization: Basic dXNlcjpwYXNz
	 User-Agent: curl/7.64.1
	 Accept: * / *
	 Content-Length: 182
	 Content-Type: application/octet-stream
	*/

#ifdef CONFIG_SIMPLE_PUSHOTA_GETVERSION
	if (!strncmp(buf, "GET ", 4)) {
		const esp_app_desc_t *desc = esp_app_get_description();
		len = sprintf(buf, "HTTP/1.0 200 OK\r\n\r\nVersion: %s\n", desc->version);
		send(sock, buf, len, 0);
		return ESP_FAIL;
	}
#endif

	// provide a way to abort
	if (!strncmp(buf, "DELETE ", 7)) {
		status = "204 No Content";
		ESP_LOGI(TAG, "Aborting.");
		ret = ESP_OK;
		goto outstatus;
	}

	if (strncmp(buf, "POST ", 5)) {
		status = "405 Method Not Allowed";
		goto outstatus;
	}

	if (!upart) {
		status = "501 Not Implemented";
		ESP_LOGE(TAG, "No OTA part available!");
		ret = ESP_ERR_NOT_SUPPORTED;
		goto outstatus;
	}

	ESP_LOGI(TAG, "target OTA part %s subtype %#x addr %#" PRIx32, upart->label, upart->subtype, upart->address);

	needle = "Content-Length:";
	s = strstr(buf, needle);
	if (!s) {
		status = "411 Length Required";
		goto outstatus;
	}

	s += strlen(needle);
	binlen = strtol(s, NULL, 10);
	if (!binlen) {
		status = "411 Length Required";
		goto outstatus;
	}

	ESP_LOGI(TAG, "Image size: %d bytes", binlen);

	// done with headers, setup OTA
	if (esp_ota_begin(upart, binlen, &ota_handle) != ESP_OK)
		goto failota;

	// write leftover buf pertaining to app image
	*binstart = c;
	if (len) {
		if (esp_ota_write(ota_handle, binstart, len) != ESP_OK)
			goto failota;
		binlen -= len;
	}

	// loop until we receive the full image
	while (binlen) {
		len = recv(sock, buf, (sizeof(buf) < binlen) ? sizeof(buf) : binlen, 0);
		if (len < 0)
			goto failota;
		if (!len)	// EOF
			break;

		if (esp_ota_write(ota_handle, buf, len) != ESP_OK)
			goto failota;

		binlen -= len;
	}

	if (binlen)	// incomplete transfer
		goto failota;

	ret = esp_ota_end(ota_handle);
	if (ret != ESP_OK)
		return ret;

	ESP_LOGI(TAG, "Flash complete");

	ret = esp_ota_set_boot_partition(upart);
	if (ret == ESP_OK)
		len = sprintf(buf, "HTTP/1.0 200 OK\r\n\r\nNext boot partition: %s\n", upart->label);
	else
		len = sprintf(buf, "HTTP/1.0 500 Internal Server Error\r\n\r\nFailed (%d).\n", ret);

	send(sock, buf, len, 0);

	return ret;

failota:
	ESP_LOGE(TAG, "ota_receive() failed");
#ifndef CONFIG_IDF_TARGET_ESP8266
	esp_ota_abort(ota_handle);
#endif
outstatus:
	s = stpcpy(buf, "HTTP/1.0 ");
	s = stpcpy(s, status);
	s = stpcpy(s, "\r\n\r\n");
	send(sock, buf, s-buf, 0);
	return ret;
}
#endif /* CONFIG_SIMPLE_PUSHOTA_ENABLED */

/**
 * Setup push OTA tcp socket and perform OTA update.
 * @param conn_cb an optional callback to a function executed when a new connection is made,
 * immediately prior to reading from it. Can be used to stop tasks and reclaim memory.
 * @return execution status
 */
esp_err_t pushota(void (*conn_cb)(void))
{
#ifdef CONFIG_SIMPLE_PUSHOTA_ENABLED
	// we only care about ipv4
	struct sockaddr_in dest_addr, source_addr;
	socklen_t addr_len;
	int sock, ret;

	dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	dest_addr.sin_family = AF_INET;
	dest_addr.sin_port = htons(OTA_PORT);

	// don't run in a loop as we will only accept one stream

	sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
	if (sock < 0) {
		ESP_LOGE(TAG, "socket(): %s", strerror(errno));
		return ESP_FAIL;
	}

	ret = ESP_FAIL;

#ifdef CONFIG_LWIP_SO_REUSE
	if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &(int){ 1 }, sizeof(int))) {
		ESP_LOGE(TAG, "SO_REUSEADDR: %s", strerror(errno));
		goto out;
	}
#else
	ESP_LOGW(TAG, "Warning: SO_REUSEADDR is not available!");
#endif

	if (bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr))) {
		ESP_LOGE(TAG, "bind(): %s", strerror(errno));
		goto out;
	}

	if (listen(sock, 1)) {
		ESP_LOGE(TAG, "listen(): %s", strerror(errno));
		goto out;
	}

	ESP_LOGI(TAG, "Socket port %d", OTA_PORT);
	addr_len = sizeof(source_addr);

	ret = accept(sock, (struct sockaddr *)&source_addr, &addr_len);
	if (ret < 0) {
		ESP_LOGE(TAG, "accept(): %d", errno);
		ret = ESP_FAIL;
		goto out;
	}

	close(sock);	// only allow exactly one connection, others get ECONNREFUSED
	sock = ret;

	if (conn_cb) {
		ESP_LOGD(TAG, "running conn_cb");
		conn_cb();
	}

	// make sure unclean client shutdown won't DoS us
	if (setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &(int){ 1 }, sizeof(int))) {
		ESP_LOGE(TAG, "SO_KEEPALIVE: %d", errno);
		ret = ESP_FAIL;
		goto out;
	}
	// assume cannot fail if the above succeeds
	setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &(int){ KEEPALIVE_IDLE }, sizeof(int));
	setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &(int){ KEEPALIVE_INTERVAL }, sizeof(int));
	setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &(int){ KEEPALIVE_COUNT }, sizeof(int));
	setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &(int){ 1 }, sizeof(int));

	ret = ota_receive(sock);

out:
	shutdown(sock, SHUT_RDWR);
	close(sock);

	return ret;
#else	/* CONFIG_SIMPLE_PUSHOTA_ENABLED */
	return ESP_ERR_NOT_SUPPORTED;
#endif
}
