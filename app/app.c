#include "m_app.h"
#include "heartbeat.h"

 /*
    W5500_Board_Config_t board_config = {
        .spi_port = SPI_PORT,
        .miso_pin = PIN_MISO,
        .mosi_pin = PIN_MOSI,
        .sck_pin = PIN_SCK,
        .cs_pin = PIN_CS,
        .rst_pin = PIN_RST,
        .int_pin = PIN_INT
    };
    int ret = W5500_Board_Init(&board_config);
    */


App_Init_Result_t App_Init(const W5500_Board_Config_t *board_cfg, W5500_Network_Config_t *network_cfg) {
    if (network_cfg == NULL) return APP_INIT_ERR_INVALID_ARGUMENT;
    int ret = W5500_Board_Init(board_cfg);
    if (ret != 0) {
        printf("W5500_Board_Init failed: %d\r\n", ret);
        return APP_INIT_ERR_BOARD;
    }

    W5500_Config_Result_t cfg_result = W5500_LoadOrInitNetworkConfig(network_cfg);
    printf("W5500 config result = %d\r\n", cfg_result);
    if (cfg_result == W5500_CONFIG_RESULT_ERROR) return APP_INIT_ERR_CONFIG;
    ret = W5500_Network_Init(network_cfg);
    if (ret != 0) return APP_INIT_ERR_NETWORK;

    return APP_INIT_OK;
}


static void App_EnsureNetworkReady(void){
    while (true) {
        int ret = W5500_Connect();
        printf("W5500_Connect = %d\r\n", ret);
        if (ret == 0) return;

        Heartbeat_BlinkCode(HEARTBEAT_ERROR_CONNECT);
        Heartbeat_Delay(1000, 100);
    }
}


static void App_EnsureServerConfigured(void){
    while (true) {
        int ret = W5500_ResolveServerConfig();
        printf("W5500_ResolveServerConfig = %d\r\n", ret);
        stdio_flush();
        if (ret == 0) return;

        Heartbeat_BlinkCode(HEARTBEAT_ERROR_SERVER_CONFIG);
        Heartbeat_Delay(3000, 100);
    }
}


void App_EnsureCommunicationReady(void){
    App_EnsureNetworkReady();
    App_EnsureServerConfigured();
}