
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "hardware/spi.h"
#include "pico/types.h"

#define W5500_CONFIG_MAGIC 0x57434631u

typedef enum {
    W5500_CONFIG_ERROR_NOT_PASSED = -1,
    W5500_CONFIG_RESULT_LOADED = 1,
    W5500_CONFIG_DEFAULT_INITIALIZED= 2
} W5500_Config_Result_t;

typedef enum {
    W5500_HTTP_OK = 0,
    W5500_HTTP_ERR_SOCKET_OPEN = -1,
    W5500_HTTP_ERR_CONNECT = -2,
    W5500_HTTP_ERR_SEND = -3,
    W5500_HTTP_ERR_REMOTE_CLOSED = -4,
    W5500_HTTP_ERR_REQUEST_TOO_LARGE = -5,
    W5500_HTTP_ERR_RECEIVE = -6,
    W5500_HTTP_ERR_STATUS_CODE = -7,
    W5500_HTTP_ERR_SOCKET_CLOSE = -8,
    W5500_HTTP_ERR_RESPONSE_TIMEOUT = -9,
    W5500_HTTP_ERR_NULL_PAYLOAD = -10,
    W5500_HTTP_ERR_INVALID_CONFIG = -11
} W5500_HTTP_Result_t;

typedef struct {
    spi_inst_t *spi_port;
    uint miso_pin;
    uint mosi_pin;
    uint sck_pin;
    uint cs_pin;
    uint rst_pin;
    uint int_pin;
} W5500_Board_Config_t;


typedef struct {
    uint32_t magic;
    char device_id[32];
    uint8_t mac[6];
    bool use_dhcp;
    uint8_t ip[4];
    uint8_t sn[4];
    uint8_t gw[4];
    uint8_t dns[4];
    uint32_t config_flags;
    uint8_t server_ip[4];
    uint16_t server_port;
    char http_path[64];
    uint32_t interval_s;
    uint32_t crc;
} W5500_Network_Config_t;


int W5500_Board_Init(const W5500_Board_Config_t *cfg);
const W5500_Board_Config_t *W5500_Board_DefaultConfig(void);
int W5500_Network_Init(const W5500_Network_Config_t *cfg);
W5500_Config_Result_t  W5500_LoadOrInitNetworkConfig(W5500_Network_Config_t *cfg);
int W5500_GetCurrentConfig(W5500_Network_Config_t *out_cfg);
void W5500_PrintChipNetworkInfo(void);
int W5500_Connect(void);
int W5500_UDP_Discovery(W5500_Network_Config_t *cfg);
int W5500_Network_Poll(void);
int W5500_ResolveServerConfig(void);
int W5500_RefreshServerConfig(void);
int W5500_SendMeasurement(float measurement);
W5500_Config_Result_t W5500_LoadOrCreateConfig(W5500_Network_Config_t *cfg);
int W5500_ResolveServerConfig(void);