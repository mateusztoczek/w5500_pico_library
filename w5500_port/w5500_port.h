
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "hardware/spi.h"
#include "pico/types.h"

#define W5500_CONFIG_MAGIC 0x57434631u

#define ERROR_BOARD_INIT 1
#define ERROR_NETWORK_INIT 2
#define ERROR_CONNECT 3
#define ERROR_SERVER_CONFIG 4
#define ERROR_HTTP_POST 5

typedef enum {
    W5500_CONFIG_RESULT_ERROR = -1,
    W5500_CONFIG_RESULT_LOADED = 1,
    W5500_CONFIG_RESULT_DEFAULT_CREATED = 2
} W5500_Config_Result_t;

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
void W5500_PrintConfig(void);
int W5500_Connect(void);
//int W5500_EnsureServerConfig(void)
int W5500_UDP_Discovery(W5500_Network_Config_t *cfg);
int W5500_HTTP_POST(const char* endpoint, const char* payload);
void W5500_PrintCurrentAppConfig(void);
W5500_Config_Result_t W5500_LoadOrCreateConfig(W5500_Network_Config_t *cfg);
int W5500_EnsureServerConfig(void);
int app_ensure_server_ready(void);
int app_ensure_network_ready(void);