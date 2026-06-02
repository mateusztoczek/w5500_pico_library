
#include "w5500_port.h"
 
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "wizchip_conf.h"
#include "socket.h"
#include "DHCP/dhcp.h"
#include "DNS/dns.h"

#define SOCK_DHCP 0
#define SOCK_HTTP 1
#define SOCK_UDP 2
#define W5500_DHCP_DEFAULT_TIMEOUT_MS 10000u

static W5500_Board_Config_t g_board;
static W5500_Network_Config_t g_conn;
static wiz_NetInfo g_netinfo;

static bool g_board_initialized = false;
static bool g_conn_initialized = true;

static volatile bool g_dhcp_ip_found = false;
static uint8_t g_dhcp_buffer[1024];


static void w5500_cs_select(void){
    gpio_put(g_board.cs_pin, 0);
}

static void w5500_cs_deselect(void){
    gpio_put(g_board.cs_pin, 1);
}

static uint8_t w5500_spi_read_byte(void){
    uint8_t rx = 0;
    spi_read_blocking(g_board.spi_port, 0x00, &rx, 1);
    return rx;
}

static void w5500_spi_write_byte(uint8_t data){
    spi_write_blocking(g_board.spi_port, &data, 1);
}

static void w5500_spi_read_burst(uint8_t *buf, uint16_t len){
    if (buf == NULL || len == 0) return;
    spi_read_blocking(g_board.spi_port, 0x00, buf, len);
}

static void w5500_spi_write_burst(const uint8_t *buf, uint16_t len){
    if (buf == NULL || len == 0) return;
    spi_write_blocking(g_board.spi_port, buf, len);
}

void w5500_reset(void){
    gpio_put(g_board.rst_pin, 0);
    sleep_ms(5);
    gpio_put(g_board.rst_pin, 1);
    sleep_ms(50);
}


int W5500_Board_Init(W5500_Board_Config_t *cfg){
    if (cfg == NULL || cfg->spi_port == NULL) return -1;

    if (cfg->miso_pin > 29 || cfg->mosi_pin > 29 ||
        cfg->sck_pin > 29 || cfg->cs_pin > 29 ||
        cfg->rst_pin > 29 || cfg->int_pin > 29) {
            return -3;
    }

    g_board = *cfg;

    spi_init(g_board.spi_port, 10 * 1000 * 1000);

    gpio_set_function(g_board.miso_pin, GPIO_FUNC_SPI);
    gpio_set_function(g_board.mosi_pin, GPIO_FUNC_SPI);
    gpio_set_function(g_board.sck_pin,  GPIO_FUNC_SPI);

    gpio_init(g_board.cs_pin);
    gpio_set_dir(g_board.cs_pin, GPIO_OUT);
    gpio_put(g_board.cs_pin, 1);

    gpio_init(g_board.rst_pin);
    gpio_set_dir(g_board.rst_pin, GPIO_OUT);
    gpio_put(g_board.rst_pin, 1);

    gpio_init(g_board.int_pin);
    gpio_set_dir(g_board.int_pin, GPIO_IN);
    gpio_pull_up(g_board.int_pin);
        
    w5500_reset();

    reg_wizchip_cs_cbfunc(w5500_cs_select, w5500_cs_deselect);
    reg_wizchip_spi_cbfunc(w5500_spi_read_byte, w5500_spi_write_byte);
    reg_wizchip_spiburst_cbfunc(w5500_spi_read_burst, w5500_spi_write_burst);

    uint8_t memsize[2][8] = {{2,2,2,2,2,2,2,2},{2,2,2,2,2,2,2,2}};
    if (ctlwizchip(CW_INIT_WIZCHIP, (void*)memsize) == -1) return -1;

    if (getVERSIONR() != 0x04) return -2;

    g_board_initialized = true;
    return 0;
}


void W5500_PrintConfig(void) {
    wiz_NetInfo data;

    ctlnetwork(CN_GET_NETINFO, &data);
    printf("--- W5500 NETWORK SETTINGS ---\r\n");
    printf("MAC: %02X:%02X:%02X:%02X:%02X:%02X\r\n", data.mac[0], data.mac[1], data.mac[2], data.mac[3], data.mac[4], data.mac[5]);
    printf("IP: %d.%d.%d.%d\r\n", data.ip[0], data.ip[1], data.ip[2], data.ip[3]);
    printf("SUBNET: %d.%d.%d.%d\r\n", data.sn[0], data.sn[1], data.sn[2], data.sn[3]);
    printf("GATEWAY: %d.%d.%d.%d\r\n", data.gw[0], data.gw[1], data.gw[2], data.gw[3]);
    printf("----------------------------\r\n");
}

/////////////////////////////////////////////////


static bool w5500_is_valid_mac(const uint8_t mac[6]){
    bool all_zero = true;
    bool all_ff = true;

    for (int i = 0; i < 6; i++) {
        if (mac[i] != 0x00) all_zero = false;
        if (mac[i] != 0xFF) all_ff = false;
    }

    if (all_zero || all_ff) return false;
    if (mac[0] & 0x01) return false;

    return true;
}


static bool w5500_is_valid_ipv4_addr(const uint8_t ip[4]){
    bool all_zero = true;
    bool all_ff = true;

    for (int i = 0; i < 4; i++) {
        if (ip[i] != 0x00) all_zero = false;
        if (ip[i] != 0xFF) all_ff = false;
    }

    if (all_zero || all_ff) return false;
    if (ip[0] == 127) return false;
    if (ip[0] >= 224 && ip[0] <= 239) return false;

    return true;
}


static bool w5500_is_valid_netmask(const uint8_t sn[4]){
    uint32_t mask = ((uint32_t)sn[0] << 24) | ((uint32_t)sn[1] << 16) | ((uint32_t)sn[2] << 8) | ((uint32_t)sn[3]);
    if (mask == 0 || mask == 0xFFFFFFFF) return false;

    return (mask & (mask + 1)) == 0;
}

// tutaj jeszzcze jakas funkcja do sprawdzanai config we flash

int W5500_Network_Init(const W5500_Network_Config_t *cfg){
    if (cfg == NULL) return -1;
    if (!g_board_initialized) return -2;

    if (!w5500_is_valid_mac(cfg->mac)) return -3;

    g_conn = *cfg;

    memset(&g_netinfo, 0, sizeof(g_netinfo));

    memcpy(g_netinfo.mac, cfg->mac, 6);
    g_netinfo.dhcp = cfg->use_dhcp ? NETINFO_DHCP : NETINFO_STATIC;

    if (!cfg->use_dhcp) {
        if (!w5500_is_valid_ipv4_addr(cfg->ip)) return -4;
        if (!w5500_is_valid_netmask(cfg->sn)) return -5;

        memcpy(g_netinfo.ip, cfg->ip, 4);
        memcpy(g_netinfo.sn, cfg->sn, 4);
        memcpy(g_netinfo.gw, cfg->gw, 4);
        memcpy(g_netinfo.dns, cfg->dns, 4);
    }

    g_conn_initialized = true;
    return 0;
}


static void DHCP_IP_Assigned(void){
    getIPfromDHCP(g_netinfo.ip);
    getGWfromDHCP(g_netinfo.gw);
    getSNfromDHCP(g_netinfo.sn);
    getDNSfromDHCP(g_netinfo.dns);
    g_netinfo.dhcp = NETINFO_DHCP;

    ctlnetwork(CN_SET_NETINFO, (void*)&g_netinfo);
    dhcp_ip_found = true;
}

static void DHCP_IP_Updated(void){
    dhcp_ip_found = true;
    //TODO
}

static void DHCP_IP_Conflict(void){
    dhcp_ip_found = false;
}


static int W5500_DHCP_Init(void){
    if (g_netinfo.dhcp != NETINFO_DHCP) {
        return 0;
    }

    g_dhcp_ip_found = false;

    DHCP_init(SOCK_DHCP, g_dhcp_buffer);
    reg_dhcp_cbfunc(DHCP_IP_Assigned, DHCP_IP_Updated, DHCP_IP_Conflict);

    return 0;
}


static int W5500_DHCP_Connect(uint32_t timeout_ms){
    absolute_time_t start = get_absolute_time();
    absolute_time_t last_tick = start;

    while (absolute_time_diff_us(start, get_absolute_time()) < timeout_ms * 1000) {
        DHCP_run();
        absolute_time_t now = get_absolute_time();

        if (absolute_time_diff_us(last_tick, now) >= 1000000) {
            DHCP_time_handler();
            last_tick = now;
        }

        if (g_dhcp_ip_found) {
            return 0;
        }

        sleep_ms(10);
    }

    return -1;
}


int W5500_Connect(void){
    if (!g_board_initialized) return -1;
    if (!g_conn_initialized) return -2;

    uint8_t link = PHY_LINK_OFF;
    ctlwizchip(CW_GET_PHYLINK, &link);

    if (link != PHY_LINK_ON) {
        return -3;
    }

    if (g_conn.use_dhcp) {
        //tutaj dhcp musi byc juz po init i po dhcp_run
    } else {
        ctlnetwork(CN_SET_NETINFO, (void*)&g_netinfo);
    }

    return 0;
}



/////////////////////////////////////////////////
// TODO

int W5500_Connect(void);

bool W5500_HasServerConfig(void);

int W5500_UDP_Discovery(W5500_Network_Config_t *cfg);

int W5500_HTTP_POST(const char *endpoint, const char *payload);

