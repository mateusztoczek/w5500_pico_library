
#include "w5500_port.h"
 
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "hardware/flash.h"
#include "wizchip_conf.h"
#include "socket.h"
#include "DHCP/dhcp.h"
#include "DNS/dns.h"
#include "pico/unique_id.h"

#define CONFIG_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define DISCOVERY_LOCAL_PORT 50000
#define DISCOVERY_SERVER_PORT 40001

#define SOCK_DHCP 0
#define SOCK_HTTP 1
#define SOCK_DISCOVERY 2

#define W5500_DHCP_DEFAULT_TIMEOUT_MS 10000u

#define W5500_CFG_FLAG_SERVER_CONFIGURED (1u << 0)
#define W5500_CFG_FLAG_PROVISIONED (1u << 1)
#define W5500_CFG_FLAG_TOKEN_VALID (1u << 2)

#define W5500_MIN_INTERVAL_S 1
#define W5500_MAX_INTERVAL_S 500

#define W5500_LINK_TIMEOUT_MS 5000
#define W5500_LINK_POLL_MS 100

#define DISCOVERY_MAX_ATTEMPTS 5
#define DISCOVERY_RESPONSE_TIMEOUT_MS 2000
#define DISCOVERY_POLL_MS 10


typedef enum {
    W5500_CONFIG_RESULT_ERROR = -1,
    W5500_CONFIG_RESULT_LOADED = 1,
    W5500_CONFIG_RESULT_DEFAULT_CREATED = 2
} W5500_Config_Result_t;

static W5500_Board_Config_t g_board;
static W5500_Network_Config_t g_conn;
static wiz_NetInfo g_netinfo;

static bool g_board_initialized = false;
static bool g_conn_initialized = true;

static volatile bool g_dhcp_ip_found = false;
static uint8_t g_dhcp_buffer[1024];
static uint8_t g_udp_discover_buffer[1024];


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

static void w5500_spi_write_burst(uint8_t *buf, uint16_t len){
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


static void W5500_New_Local_MAC(uint8_t mac[6]){
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);

    mac[0] = 0x02;
    mac[1] = 0x46;
    mac[2] = 0x52;
    mac[3] = id.id[5];
    mac[4] = id.id[6];
    mac[5] = id.id[7];
}


void W5500_Default_config(W5500_Network_Config_t *cfg){
    memset(cfg, 0, sizeof(*cfg));
    cfg->magic = W5500_CONFIG_MAGIC;
    W5500_New_Local_MAC(cfg->mac);
    cfg->use_dhcp = true;
    cfg->server_ip[0] = 0;
    cfg->server_port = 0;
    cfg->http_path[0] = '\0';
    cfg->interval_s = 60;
    cfg->config_flags = 0;
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
    if (sn == NULL) return false;

    uint32_t mask = ((uint32_t)sn[0] << 24) | ((uint32_t)sn[1] << 16) | ((uint32_t)sn[2] << 8) | ((uint32_t)sn[3]);
    if (mask == 0u || mask == 0xFFFFFFFFu) return false;
    uint32_t inv = ~mask;

    return (inv & (inv + 1u)) == 0u;
}


static uint32_t w5500_crc32_compute(const void *data, size_t len){
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;

    for (size_t i = 0; i < len; i++) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; bit++) {
            if (crc & 1u) crc = (crc >> 1) ^ 0xEDB88320u;
            else crc >>= 1;
        }
    }

    return ~crc;
}

static bool w5500_is_valid_crc(const W5500_Network_Config_t *cfg){
    if (cfg == NULL) return false;
    if (cfg->crc == 0u || cfg->crc == 0xFFFFFFFFu) return false;
    uint32_t calculated_crc = w5500_crc32_compute(cfg, offsetof(W5500_Network_Config_t, crc));

    return calculated_crc == cfg->crc;
}


// pobieranie config z flash
// potem: sprawdzenie czy jest config i czy jest ok. jesli nie ma albo cos jest nie tak idz do default config
static int W5500_Load_Flash_Config(W5500_Network_Config_t *cfg){
    if (cfg == NULL) return -1;

    const uint8_t *flash_ptr = (const uint8_t *)(XIP_BASE + CONFIG_FLASH_OFFSET);
    memcpy(cfg, flash_ptr, sizeof(W5500_Network_Config_t));

    return 0;
}


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
    g_dhcp_ip_found = true;
}

static void DHCP_IP_Updated(void){
    g_dhcp_ip_found = true;
    //TODO
}

static void DHCP_IP_Conflict(void){
    g_dhcp_ip_found = false;
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

    if(W5500_DHCP_Init() != 0){
        return -1;
    }

    absolute_time_t start = get_absolute_time();
    absolute_time_t last_tick = start;

    int64_t timeout_us = (int64_t)timeout_ms * 1000;

    while (absolute_time_diff_us(start, get_absolute_time()) < timeout_us) {
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

    return -2;
}


static int W5500_Ethernet_Link(uint32_t timeout_ms){
    uint8_t link = PHY_LINK_OFF;
    uint32_t time_ms = 0;

    while (time_ms < timeout_ms) {
        if (ctlwizchip(CW_GET_PHYLINK, &link) == -1) return -1;
        if (link == PHY_LINK_ON) return 0;
        sleep_ms(W5500_LINK_POLL_MS);
        time_ms += W5500_LINK_POLL_MS;
    }

    return -2;
}


int W5500_Connect(void){
    if (!g_board_initialized) return -1;
    if (!g_conn_initialized) return -2;

    int link_ret = W5500_Ethernet_Link(W5500_LINK_TIMEOUT_MS);
    if (link_ret != 0) return -3;

    if (g_conn.use_dhcp) return W5500_DHCP_Connect(W5500_DHCP_DEFAULT_TIMEOUT_MS);
    if (ctlnetwork(CN_SET_NETINFO, (void*)&g_netinfo) == -1) return -4;

    return 0;
}


static bool W5500_ServerConfig_IsValid(const W5500_Network_Config_t *cfg){
    if(cfg == NULL) return false;
    if (!(cfg->config_flags & W5500_CFG_FLAG_SERVER_CONFIGURED)) return false;
    if (!w5500_is_valid_ipv4_addr(cfg->server_ip)) return false;
    if (cfg->server_port == 0) return false;
    if (cfg->http_path[0] == '\0') return false;

    return true;

}


static bool W5500_Is_Config_Valid(const W5500_Network_Config_t *cfg){
    if (cfg == NULL) return false;
    if(cfg->magic != W5500_CONFIG_MAGIC) return false;
    if(!w5500_is_valid_crc(cfg)) return false;
    if(!w5500_is_valid_mac(cfg->mac)) return false;
    if(cfg-> interval_s < W5500_MIN_INTERVAL_S  || cfg-> interval_s > W5500_MAX_INTERVAL_S) return false; 
    if(!cfg->use_dhcp){
        if (!w5500_is_valid_ipv4_addr(cfg->ip)) return false;
        if (!w5500_is_valid_netmask(cfg->sn)) return false;
    }
    
    if((cfg->config_flags & W5500_CFG_FLAG_SERVER_CONFIGURED) != 0){
        if (!W5500_ServerConfig_IsValid(cfg)) return false;
    }

    return true;
}


//TODO: Save config

W5500_Config_Result_t W5500_LoadOrCreateConfig(W5500_Network_Config_t *cfg){
    if(cfg == NULL) return W5500_CONFIG_RESULT_ERROR;
    if(W5500_Load_Flash_Config(cfg) == 0){
        if(W5500_Is_Config_Valid(cfg)) return W5500_CONFIG_RESULT_LOADED;
    }
    W5500_Default_config(cfg);
    
    return W5500_CONFIG_RESULT_DEFAULT_CREATED;
}


static int UDP_BuildDiscover(const W5500_Network_Config_t *cfg, uint8_t *buffer, size_t buffer_size) {
    if (cfg == NULL || buffer == NULL || buffer_size == 0) return -1;

    int len = snprintf((char *)buffer, buffer_size,
        "DISCOVER freezer_sensor_v1 mac=%02X:%02X:%02X:%02X:%02X:%02X fw=0.1.0 hw=w5500-pico",
        cfg->mac[0], cfg->mac[1], cfg->mac[2], cfg->mac[3], cfg->mac[4], cfg->mac[5]
    );
    
    if (len < 0) return -2;
    if ((size_t)len >= buffer_size) return -3;

    return len;
}


static int UDP_ParseAddress(const char *text, uint8_t ip[4]){
    if (text == NULL || ip == NULL) return -1;

    unsigned int a, b, c, d;
    if (sscanf(text, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return -2;
    if (a > 255 || b > 255 || c > 255 || d > 255) return -3;

    ip[0]= (uint8_t)a;
    ip[1]= (uint8_t)b;
    ip[2]= (uint8_t)c;
    ip[3]= (uint8_t)d;

    return 0;
}


static int UDP_ExtractValue(const char *text, const char *key, char *out, size_t out_size){
    if (text == NULL || key == NULL || out == NULL || out_size == 0) return -1;

    const char *start = strstr(text, key);
    if (start == NULL) return -2;
    start += strlen(key);

    const char *end = strchr(start, ' ');
    size_t len;

    if (end != NULL) {
        len = (size_t)(end - start);
    } else {
        len = strlen(start);
    }

    if (len == 0) return -3;
    if (len >= out_size) return -4;

    memcpy(out, start, len);
    out[len] = '\0';

    return 0;
}


//example response: CONFIG freezer_sensor_v1 device_id=freezer-test-001 server_ip=192.168.1.100 server_port=8090 http_path=/measurement interval_s=10
static int UDP_ParseDiscovery(const char *response, const uint8_t remote_ip[4], W5500_Network_Config_t *cfg){
    if (response == NULL || cfg == NULL) return -1;
    if (strstr(response, "CONFIG freezer_sensor_v1") == NULL) return -2;

    char device_id[32];
    char server_ip_text[24];
    char server_port_text[8];
    char http_path[64];
    char interval_text[12];
    uint8_t server_ip[4];
    unsigned long server_port_ul;
    unsigned long interval_ul;

    if(UDP_ExtractValue(response, "device_id=", device_id, sizeof(device_id)) != 0) return -3;
    if(UDP_ExtractValue(response, "server_ip=", server_ip_text, sizeof(server_ip_text)) != 0) return -4;
    if(UDP_ParseAddress(server_ip_text, server_ip) != 0) return -5;
    if(UDP_ExtractValue(response, "server_port=", server_port_text, sizeof(server_port_text)) != 0) return -6;

    server_port_ul = strtoul(server_port_text, NULL, 10);
    if(server_port_ul == 0 || server_port_ul > 65535) return -7;
    if(UDP_ExtractValue(response, "http_path=", http_path, sizeof(http_path)) != 0) return -8;
    if(http_path[0] != '/') return -9;
    if(UDP_ExtractValue(response, "interval_s=", interval_text, sizeof(interval_text)) != 0) return -10;

    interval_ul = strtoul(interval_text, NULL, 10);
    if(interval_ul < W5500_MIN_INTERVAL_S || interval_ul > W5500_MAX_INTERVAL_S) return -11;

    memset(cfg->device_id, 0, sizeof(cfg->device_id));
    strncpy(cfg->device_id, device_id, sizeof(cfg->device_id) - 1);
    memcpy(cfg->server_ip, server_ip, 4);
    cfg->server_port = (uint16_t)server_port_ul;
    memset(cfg->http_path, 0, sizeof(cfg->http_path));
    strncpy(cfg->http_path, http_path, sizeof(cfg->http_path) - 1);
    cfg->interval_s = (uint32_t)interval_ul;
    cfg->config_flags |= W5500_CFG_FLAG_SERVER_CONFIGURED;
    cfg->config_flags |= W5500_CFG_FLAG_PROVISIONED;

    return 0;
}


int W5500_UDP_Discovery(W5500_Network_Config_t *cfg){
    if (cfg == NULL) return -1;
    if (!w5500_is_valid_mac(cfg->mac)) return -2;

    close(SOCK_DISCOVERY);
    sleep_ms(10);

    int8_t r = socket(SOCK_DISCOVERY, Sn_MR_UDP, DISCOVERY_LOCAL_PORT, 0);
    if (r != SOCK_DISCOVERY) return -3;

    uint8_t sr = getSn_SR(SOCK_DISCOVERY);
    if (sr != SOCK_UDP) {
        close(SOCK_DISCOVERY);
        return -4;
    }

    int msg_len = UDP_BuildDiscover(cfg, g_udp_discover_buffer, sizeof(g_udp_discover_buffer));
    if (msg_len < 0) {
        close(SOCK_DISCOVERY);
        return -5;
    }

    //uint8_t target_ip[4] = {192, 168, 1, 100};
    uint8_t target_ip[4] = {255, 255, 255, 255};

    for (int attempt = 1; attempt <= DISCOVERY_MAX_ATTEMPTS; attempt++) {
        printf("Sending UDP DISCOVER to %u.%u.%u.%u:%u\r\n", target_ip[0], target_ip[1], target_ip[2], target_ip[3], DISCOVERY_SERVER_PORT);

        int32_t sent = sendto(SOCK_DISCOVERY, g_udp_discover_buffer, (uint16_t)msg_len, target_ip, DISCOVERY_SERVER_PORT);
        if (sent != msg_len) {
            printf("sendto failed on attempt %d\r\n", attempt);
            sleep_ms(100);
            continue;
        }

        absolute_time_t start = get_absolute_time();
        while (absolute_time_diff_us(start, get_absolute_time()) < (int64_t)DISCOVERY_RESPONSE_TIMEOUT_MS * 1000) {

            uint16_t rx_size = getSn_RX_RSR(SOCK_DISCOVERY);
            if (rx_size > 0) {
                if (rx_size >= sizeof(g_udp_discover_buffer)) rx_size = sizeof(g_udp_discover_buffer) - 1;

                uint8_t remote_ip[4] = {0};
                uint16_t remote_port = 0;

                int32_t received = recvfrom(SOCK_DISCOVERY, g_udp_discover_buffer, rx_size, remote_ip, &remote_port);
                if (received > 0) {
                    g_udp_discover_buffer[received] = '\0';
                    printf("UDP response: %s\r\n", (char *)g_udp_discover_buffer);

                    int parse_ret = UDP_ParseDiscovery((char *)g_udp_discover_buffer, remote_ip, cfg );
                    if (parse_ret == 0 && W5500_ServerConfig_IsValid(cfg)) {
                        close(SOCK_DISCOVERY);
                        return 0;
                    }

                    printf("Invalid CONFIG response, ignoring\r\n");
                }
                break;
            }
            sleep_ms(DISCOVERY_POLL_MS);
        }
        sleep_ms(200);
    }

    printf("UDP discovery failed: no response\r\n");

    close(SOCK_DISCOVERY);
    return -6;
}

int W5500_EnsureServerConfig(void){
    if (W5500_ServerConfig_IsValid(&g_conn)) {
        return 0;
    }

    return W5500_UDP_Discovery(&g_conn);
}



/////////////////////////////////////////////////
// TODO

//int W5500_HTTP_POST(const char *endpoint, const char *payload);

