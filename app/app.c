#include <stdio.h>
#include "m_app.h"
#include "heartbeat.h"


App_Init_Result_t App_Init(const W5500_Board_Config_t *board_cfg, W5500_Network_Config_t *network_cfg) {
    if (network_cfg == NULL) return APP_INIT_ERR_INVALID_ARGUMENT;

    int ret = W5500_Board_Init(board_cfg);
    if (ret != 0) {
        printf("W5500_Board_Init failed: %d\r\n", ret);
        return APP_INIT_ERR_BOARD;
    }

    W5500_Config_Result_t cfg_result= W5500_LoadOrInitNetworkConfig(network_cfg);
    if (cfg_result == W5500_CONFIG_RESULT_ERROR) {
        return APP_INIT_ERR_CONFIG;
    }

    ret = W5500_Network_Init(network_cfg);
    if (ret != 0) return APP_INIT_ERR_NETWORK;

    return APP_INIT_OK;
}


static void App_EnsureNetworkReady(void){
    while (true) {
        const int ret = W5500_Connect();
        if (ret == 0) return;

        Heartbeat_BlinkCode(HEARTBEAT_ERROR_CONNECT);
        Heartbeat_Delay(1000, 100);
    }
}


static void App_EnsureServerConfigured(void){
    while (true) {
        const int ret = W5500_ResolveServerConfig();
        if (ret == 0) return;

        Heartbeat_BlinkCode(HEARTBEAT_ERROR_SERVER_CONFIG);
        Heartbeat_Delay(3000, 100);
    }
}


void App_EnsureCommunicationReady(void){
    App_EnsureNetworkReady();
    App_EnsureServerConfigured();
}


App_Network_Result_t App_NetworkPoll(void){
    const int ret = W5500_Network_Poll();
    if (ret == 0) return APP_NETWORK_OK;
    if (ret > 0) return APP_NETWORK_BUSY;
    switch (ret) {
        case -3:
        case -4:
            return APP_NETWORK_ERR_LINK;
        case -5:
        case -6:
        case -7:
        case -8:
            return APP_NETWORK_ERR_DHCP;
        default:
            return APP_NETWORK_ERR_INTERNAL;
    }
}


int App_RefreshServerConfig(void){
    const int ret = W5500_RefreshServerConfig();
    if (ret != 0) {
        Heartbeat_BlinkCode(HEARTBEAT_ERROR_SERVER_CONFIG);
        return -1;
    }

    return 0;
}