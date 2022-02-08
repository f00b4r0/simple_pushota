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
  * In simple_pushota set the desired listen port

Add the `simple_pushota.h` include and simply call `pushota(NULL)` in your project to launch the listener.

The function will block and wait for an update.
Thus it can either be called as needed (triggered from another part of the project when enabling OTA is wanted,
suspending code execution), or it can run as a separate thread, always available (in which case special attention
must be paid to concurrent threads execution during the OTA process).

The function accepts an optional callback as argument, which can be used to e.g. stop other threads and/or
reclaim memory before the update process begins (see example below).

The component needs approximately 2320 bytes of stack for its operation (on ESP8266).

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

## Implementation details

The system is very crude, it provides just enough HTTP glue for a basic HTTP client to be able to upload the new firmware binary.

It accepts a single connection (for obvious reasons): once a connection has been established,
subsequent ones will be denied until the process completes.

The code will check that a payload length is provided in the request headers,
and that the upload content is actually at least the same length as what was specified in the POST request.
Integrity checks are "delegated" to the underlying app_update subsystem,  

If the `conn_cb` parameter is not `NULL` the pointed function will be executed immediately after a connection is established,
before any processing is done on the content of the HTTP request.

Upon success `pushota()` will return `ESP_OK`, at which point it is typically safe to call `esp_restart()` to boot into the new firmware.

If the operation is aborted through an HTTP DELETE request, `pushota()` does not touch the flash and exits successfully with `ESP_OK`.

**(*) NOTE**:  `SO_REUSEADDR` is not *strictly* necessary and it is possible to use this code without it.
It is used (and necessary) to allow immediate reuse of the listening port in the event the system is *not* restarted
after `pushota()` has returned. Otherwise on the next run of `pushota()` the system will fail with the following error:

```
pushota: bind(): Address already in use
```

A warning is printed to console when `SO_REUSEADDR` is not enabled.

## Example code

### Standalone

```c
int app_main(void)
{
	bool wantota;
	
	/* ... */
	
	if (wantota) {
		ESP_LOGI(TAG, "Starting OTA");
		ret = pushota(NULL);	// will block
		if (!ret)
			esp_restart();
	}
}
```

### In separate task with a callback to kill another task

```c
TaskHandle_t taskHandle;

static void killtask(void)
{
	vTaskDelete(taskHandle);
}

static void pushota_task(void *pvParameter)
{
	pushota(killtask);
	esp_restart();	// always restart when pushota() returns
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
