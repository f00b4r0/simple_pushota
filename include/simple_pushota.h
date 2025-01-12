//
//  simple_pushota.h
//
//
//  (C) 2022 Thibaut VARENE
//  License: GPLv2 - http://www.gnu.org/licenses/gpl-2.0.html
//

#ifndef simple_pushota_h
#define simple_pushota_h

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t pushota(void (*conn_cb)(void));

#ifdef __cplusplus
}
#endif

#endif /* simple_pushota_h */
