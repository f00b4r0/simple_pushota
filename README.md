#  ESP-IDF simple push OTA component

A standalone component for ESP-IDF that adds basic push OTA capability to your project.

This component is geared toward development stages, it is definitely **not production ready**,
if only because the update is not even password-protected. It leverages the ESP "app_update" routines.

I use it when developing new ESP apps and so I make it available to others as-is,
with no warranty of any kind :)

It enables quick and easy remote flashing of a device under development.

This component supports (and has been successfully tested with) both **ESP8266** and **ESP32**.

## License

GPLv2-only - http://www.gnu.org/licenses/gpl-2.0.html

Copyright: (C) 2021-2022 Thibaut VARÈNE

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License version 2,
as published by the Free Software Foundation.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See LICENSE.md for details

## Usage

### Integration with your project

* Drop this component in your project's `components` folder
* run `idf.py menuconfig`
* Set the partition table to allow OTA (using e.g. "Factory + 2 OTA partitions")
* Under Components config:
  * In LwIP, enable `SO_REUSEADDR` support (*)
  * In simple_pushota, enable and set the desired listen port

Add the `simple_pushota.h` include and simply call `pushota(NULL)` in your project to launch the listener.

The function will block and wait for an update.
Thus it can either be called as needed (triggered from another part of the project when enabling OTA is wanted,
suspending code execution), or it can run as a separate thread, always available (in which case special attention
must be paid to concurrent threads execution during the OTA process).

The function accepts an optional callback as argument, which can be used to e.g. stop other threads and/or
reclaim memory before the update process begins (see example below).

Errors are reported to the HTTP client via response headers, using standard HTTP response codes.

The component needs approximately 2340 bytes of stack for its operation (on ESP8266 with stack smashing protection disabled).

### Flashing OTA

When `pushota()` has been called, it will block and listen on the configured listen port for an incoming HTTP POST request.
The payload is the firmware image.

In simple words, using [curl](https://curl.se):

* Build your project using `idf.py build`
* Execute e.g. `curl <esphost>:<OTA_PORT> --data-binary @build/<project>.bin`

Where:

* `<esphost>` is your device's IP address
* `<OTA_PORT>` is the configured OTA listen port
* `<project>` is your project name

A successful flash will be greeted with a 200 OK response and the next OTA boot partition will be sent in the reply content,
otherwise an error will be reported to the client. 

It is possible to abort a call to `pushota()` without flashing by sending an HTTP DELETE request using e.g.
`curl <esphost>:<OTA_PORT> -X DELETE`

If enabled in menuconfig, it is possible to query the running firmware version by sending an HTTP GET request using e.g.
`curl <esphost>:<OTA_PORT>`. The version will be provided in the response content.

## Implementation details

The system is very crude, it provides just enough HTTP glue for a basic HTTP client to be able to upload the new firmware binary.

It accepts a single connection (for obvious reasons): once a connection has been established,
subsequent ones will be denied until the process completes.

The code will check that a payload length is provided in the request headers,
and that the upload content is actually at least the same length as what was specified in the POST request.
Integrity checks are "delegated" to the underlying app_update subsystem, and the implementation gracefully handles the case where no
OTA partitions are available.

If the `conn_cb` parameter is not `NULL` the pointed function will be executed immediately after a connection is established,
before any processing is done on the content of the HTTP request.

Upon success `pushota()` will return `ESP_OK`, at which point it is typically safe to call `esp_restart()` to boot into the new firmware.

If the operation is aborted through an HTTP DELETE request, `pushota()` does not touch the flash and exits successfully with `ESP_OK`.

If `CONFIG_SIMPLE_PUSHOTA_GETVERSION` is enabled in menuconfig, it is possible to query the current firmware version through
an HTTP GET request. The firmware version will be returned as a string in the response content and `pushota()` will return `ESP_FAIL`.

The reason for returning `ESP_FAIL` is so that the caller can distinguish between e.g. an abort request, which from the point of view of the
caller simulates a successful update without actually doing anything (hence returning "success"); and a version request which is not a 
successful update (hence returning "faillure") and which may be followed by another call to `pushota()` without restarting to perform the
actual update.

When it is enabled in menuconfig, the component defines `CONFIG_SIMPLE_PUSHOTA_ENABLED` which can be used to
selectively disable header inclusion and code compilation. Doing so allows entirely removing the component
from your project without having to touch the project's code.

When not enabled, `pushota()` will still be defined and will unconditionally return `ESP_ERR_NOT_SUPPORTED`.

**(*) NOTE**:  `SO_REUSEADDR` is not *strictly* necessary and it is possible to use this code without it.
It is used (and necessary) to allow immediate reuse of the listening port in the event the system is *not* restarted
after `pushota()` has returned. Otherwise on the next run of `pushota()` the system will fail with the following error:

```
pushota: bind(): Address already in use
```

A warning is printed to console when `SO_REUSEADDR` is not enabled.

## Example code

### Standalone with selective compilation

```c
#ifdef CONFIG_SIMPLE_PUSHOTA_ENABLED
 #include "simple_pushota.h"
#endif

int app_main(void)
{
	bool wantota;
	
	/* ... */
	
#ifdef CONFIG_SIMPLE_PUSHOTA_ENABLED
	if (wantota) {
		ESP_LOGI(TAG, "Starting OTA");
		if (pushota(NULL) == ESP_OK)	// will block
			esp_restart();
	}
#endif
}
```

### In separate task with a callback to kill another task

```c
#include "simple_pushota.h"

TaskHandle_t taskHandle;

static void killtask(void)
{
	if (taskHandle) {
		vTaskDelete(taskHandle);
		taskHandle = NULL;
	}
}

static void pushota_task(void *pvParameter)
{
	while (pushota(killtask) != ESP_OK);	// will block
	esp_restart();	// restart on success
}

int app_main(void)
{
	/* ... */
	
	ret = xTaskCreate(&mytask, "mytask", 4096, NULL, 5, &taskHandle);
	if (ret != pdPASS) {
		ESP_LOGE(TAG, "Failed to create my task");
		abort();
	}

	ret = xTaskCreate(&pushota_task, "ota", 2560, NULL, 2, NULL);
	if (ret != pdPASS)
		ESP_LOGE(TAG, "Failed to create pushota task");
}
```
