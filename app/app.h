#pragma once
#include "w5500_port.h"

#define HEARTBEAT_ERROR_BOARD_INIT 1
#define HEARTBEAT_ERROR_NETWORK_INIT 2
#define HEARTBEAT_ERROR_CONNECT 3
#define HEARTBEAT_ERROR_SERVER_CONFIG 4
#define HEARTBEAT_ERROR_HTTP_POST 5

typedef enum {
    APP_INIT_OK = 0,
    APP_INIT_ERR_INVALID_ARGUMENT,
    APP_INIT_ERR_BOARD,
    APP_INIT_ERR_CONFIG,
    APP_INIT_ERR_NETWORK
} App_Init_Result_t;

typedef enum {
    APP_NETWORK_OK = 0,
    APP_NETWORK_BUSY,
    APP_NETWORK_ERR_LINK,
    APP_NETWORK_ERR_DHCP,
    APP_NETWORK_ERR_INTERNAL
} App_Network_Result_t;

App_Init_Result_t App_Init(const W5500_Board_Config_t *board_cfg, W5500_Network_Config_t *network_cfg);
void App_EnsureCommunicationReady(void);
App_Network_Result_t App_NetworkPoll(void);
int App_RefreshServerConfig(void);